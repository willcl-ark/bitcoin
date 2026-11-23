// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <coinsstore.h>
#include <dbwrapper.h>
#include <primitives/block.h>
#include <script/script.h>
#include <test/util/coinsstore.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <undo.h>
#include <util/threadpool.h>
#include <util/byte_units.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

//! Parameters of an in-memory LevelDB for a test, every field named.
static DBParams MemoryDb(const char* name)
{
    return DBParams{
        .path = name,
        .cache_bytes = 1_MiB,
        .memory_only = true,
        .wipe_data = false,
        .obfuscate = false,
        .bloom_filter = true,
        .options = {},
    };
}

//! A started pool for a flush's further workers, as the node gives the store its connect pool.
static std::shared_ptr<ThreadPool> FlushHelpers()
{
    auto pool{std::make_shared<ThreadPool>("flush")};
    pool->Start(3);
    return pool;
}

BOOST_FIXTURE_TEST_SUITE(coinsstore_tests, BasicTestingSetup)

namespace {

COutPoint RandomOutpoint(FastRandomContext& rng, uint32_t txids)
{
    // A small txid space so that the same outpoints are revisited.
    uint256 h;
    h.data()[0] = static_cast<unsigned char>(rng.randrange(txids));
    return COutPoint{Txid::FromUint256(h), static_cast<uint32_t>(rng.randrange(3))};
}

Coin RandomCoin(FastRandomContext& rng, int height)
{
    CTxOut out{static_cast<CAmount>(rng.randrange(1000) + 1), CScript() << std::vector<unsigned char>(rng.randbytes(4))};
    return Coin{std::move(out), height, /*fCoinBaseIn=*/false};
}

//! The visible state of a view over a universe of outpoints.
std::map<COutPoint, std::optional<Coin>> Visible(const CCoinsView& view, const std::vector<COutPoint>& universe)
{
    std::map<COutPoint, std::optional<Coin>> out;
    for (const COutPoint& op : universe) out[op] = view.PeekCoin(op);
    return out;
}

bool Same(const std::optional<Coin>& a, const std::optional<Coin>& b)
{
    if (!a && !b) return true;
    if (!a || !b) return false;
    return a->out == b->out && a->nHeight == b->nHeight && a->fCoinBase == b->fCoinBase;
}

void CheckSameVisible(const CCoinsView& a, const CCoinsView& b, const std::vector<COutPoint>& universe)
{
    const auto va{Visible(a, universe)};
    const auto vb{Visible(b, universe)};
    for (const COutPoint& op : universe) {
        BOOST_CHECK_MESSAGE(Same(va.at(op), vb.at(op)), "mismatch at " << op.ToString());
    }
}

std::vector<COutPoint> Universe(uint32_t txids)
{
    std::vector<COutPoint> out;
    for (uint32_t t{0}; t < txids; ++t) {
        uint256 h;
        h.data()[0] = static_cast<unsigned char>(t);
        for (uint32_t n{0}; n < 3; ++n) out.emplace_back(Txid::FromUint256(h), n);
    }
    return out;
}

//! A name for the block at `height`: any but the null hash.
uint256 BlockLabel(int height) { return uint256{static_cast<uint8_t>(1 + height % 255)}; }

//! A write-out run as the node makes one: begun, streamed to the base and committed.
std::unique_ptr<CoinsStore::WriteOutRun> WriteOut(CoinsStore& store, int bound_height, int head_height, const uint256& new_head, const std::function<uint256(int)>& block_at = BlockLabel)
{
    auto run{store.BeginWriteOut(bound_height, head_height, new_head, /*final=*/false)};
    BOOST_REQUIRE(run);
    store.StreamWriteOut(*run, block_at);
    store.CommitWriteOut(*run);
    return run;
}

//! A base that keeps the changes of the latest run written through it, and calls `m_in_flight` once while a
//! run is in flight: after the run collected its changes and before the first of them is in the database
//! (the tests' runs are a single batch).
class RunBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    std::vector<CoinWrite> m_writes;
    std::function<void()> m_in_flight;
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override
    {
        if (m_in_flight) std::exchange(m_in_flight, nullptr)();
        // A run ends with a write of no changes, which closes or moves its marker.
        if (std::exchange(m_closed, writes.empty())) m_writes.clear();
        m_writes.insert(m_writes.end(), writes.begin(), writes.end());
        CCoinsViewBacked::WriteRun(writes, new_head, old_head, final);
    }

private:
    bool m_closed{false};
};

//! The lowest change height among the dirty entries, or nullopt if there is none.
std::optional<int> OldestDirty(const CoinsStore& store)
{
    const int bound{store.BoundForBytes(1, std::numeric_limits<int>::max())};
    return bound == std::numeric_limits<int>::max() ? std::nullopt : std::make_optional(bound);
}

//! Bytes of the dirty entries changed at or below `height`: the most that a bound no higher frees.
size_t DirtyBytesAtOrBelow(const CoinsStore& store, int height)
{
    const auto within{[&](size_t bytes) { return store.BoundForBytes(bytes, std::numeric_limits<int>::max()) <= height; }};
    size_t low{0};
    size_t high{1};
    while (within(high)) high *= 2;
    while (high - low > 1) {
        const size_t mid{low + (high - low) / 2};
        (within(mid) ? low : high) = mid;
    }
    return low;
}

} // namespace

//! The store, driven as a cache would be (adds, spends, child flushes, full flushes), shows the same coins
//! as a CCoinsViewCache over an identical database at every step. After a bounded run its database holds
//! every outpoint as it is, unless the outpoint changed above the height the run settled at.
BOOST_AUTO_TEST_CASE(same_as_cache_under_random_operations)
{
    constexpr uint32_t TXIDS{24};
    const auto universe{Universe(TXIDS)};
    CCoinsViewDB db_cache{MemoryDb("cache"), {}};
    CCoinsViewDB db_store{MemoryDb("store"), {}};
    CCoinsViewCache cache{&db_cache};
    RunBase base{&db_store};
    CoinsStore store{&base, /*shards=*/8, /*deterministic=*/true};
    const uint256 genesis{m_rng.rand256()};
    cache.SetBestBlock(genesis);
    store.SetBestBlock(genesis, 0);
    // The step each outpoint was last added or spent at.
    std::map<COutPoint, int> last_change;

    for (int step{1}; step < 400; ++step) {
        const uint256 block_hash{m_rng.rand256()};
        store.SetChangeHeight(step);
        // Some operations straight on the tier, some through a child cache as a block connection does.
        if (m_rng.randbool()) {
            for (int i{0}; i < 6; ++i) {
                const COutPoint op{RandomOutpoint(m_rng, TXIDS)};
                last_change[op] = step;
                if (m_rng.randrange(3) == 0) {
                    // SpendCoin() answers true for any entry it holds, spent or not, in both; the store keeps
                    // spent entries of blocks above a run's bound where the cache dropped them at its full
                    // write, so only the answer for a visible coin is comparable.
                    const bool had{cache.HaveCoin(op)};
                    BOOST_CHECK_EQUAL(had, store.HaveCoin(op));
                    const bool a{cache.SpendCoin(op)};
                    const bool b{store.SpendCoin(op)};
                    if (had) BOOST_CHECK(a && b);
                } else {
                    Coin coin{RandomCoin(m_rng, step)};
                    Coin copy{coin};
                    const bool overwrite{cache.HaveCoin(op)};
                    BOOST_CHECK_EQUAL(overwrite, store.HaveCoin(op));
                    cache.AddCoin(op, std::move(coin), overwrite);
                    store.AddCoin(op, std::move(copy), overwrite);
                }
            }
            cache.SetBestBlock(block_hash);
            store.SetBestBlock(block_hash);
        } else {
            CCoinsViewCache child_cache{&cache};
            CCoinsViewCache child_store{&store};
            for (int i{0}; i < 6; ++i) {
                const COutPoint op{RandomOutpoint(m_rng, TXIDS)};
                last_change[op] = step;
                if (m_rng.randrange(3) == 0) {
                    const bool had{child_cache.HaveCoin(op)};
                    BOOST_CHECK_EQUAL(had, child_store.HaveCoin(op));
                    const bool a{child_cache.SpendCoin(op)};
                    const bool b{child_store.SpendCoin(op)};
                    if (had) BOOST_CHECK(a && b);
                } else {
                    Coin coin{RandomCoin(m_rng, step)};
                    Coin copy{coin};
                    const bool overwrite{child_cache.HaveCoin(op)};
                    BOOST_CHECK_EQUAL(overwrite, child_store.HaveCoin(op));
                    child_cache.AddCoin(op, std::move(coin), overwrite);
                    child_store.AddCoin(op, std::move(copy), overwrite);
                }
            }
            child_cache.SetBestBlock(block_hash);
            child_store.SetBestBlock(block_hash);
            child_cache.Flush();
            child_store.Flush();
        }
        CheckSameVisible(cache, store, universe);
        BOOST_CHECK(cache.GetBestBlock() == store.GetBestBlock());

        // Now and then write out: the cache in full, the store either in full or as a lag run of its
        // oldest changes; afterwards the databases must agree on everything the store has written and
        // the views must still agree on everything.
        if (step % 25 == 0) {
            cache.Sync();
            if (m_rng.randbool()) {
                store.Sync();
                CheckSameVisible(db_cache, db_store, universe);
                BOOST_CHECK(db_store.GetBestBlock() == block_hash);
                BOOST_CHECK(db_store.GetHeadBlocks().empty());
            } else {
                const int bound{step - 1 - static_cast<int>(m_rng.randrange(19))};
                auto run{store.BeginWriteOut(bound, step, block_hash, /*final=*/false)};
                if (run) {
                    // Changes made while the run is in flight stay dirty.
                    const COutPoint late{RandomOutpoint(m_rng, TXIDS)};
                    bool late_spent{false};
                    base.m_in_flight = [&] {
                        late_spent = store.HaveCoin(late);
                        store.SpendCoin(late);
                        cache.SpendCoin(late);
                    };
                    store.StreamWriteOut(*run, [&](int height) { return height == step ? block_hash : BlockLabel(height); });
                    store.CommitWriteOut(*run);
                    BOOST_CHECK(run->settled_height >= bound && run->settled_height <= step);
                    BOOST_CHECK(store.WrittenHeight() == run->settled_height);
                    BOOST_CHECK(store.WrittenHead() == run->settled_head);
                    const auto heads{db_store.GetHeadBlocks()};
                    BOOST_REQUIRE_EQUAL(heads.size(), 2U);
                    BOOST_CHECK(heads[0] == block_hash);
                    BOOST_CHECK(db_store.GetBestBlock().IsNull());
                    // Nothing changed at or below the bound is still dirty, unless it changed again since.
                    const auto oldest{OldestDirty(store)};
                    BOOST_CHECK(!oldest || *oldest > bound || late_spent);
                    // The database holds the state at the settled height: every outpoint not changed above
                    // it is there as the views have it. (An outpoint changed above it is the replay's, which
                    // applies that change again; the outpoint spent with the run in flight is one such.)
                    for (const COutPoint& op : universe) {
                        if (op == late) continue;
                        if (const auto changed{last_change.find(op)}; changed != last_change.end() && changed->second > run->settled_height) continue;
                        BOOST_CHECK_MESSAGE(Same(db_store.PeekCoin(op), store.PeekCoin(op)),
                                            "after a run settled at " << run->settled_height << " the database differs at " << op.ToString());
                    }
                    cache.Sync();
                }
            }
            CheckSameVisible(cache, store, universe);
            store.SanityCheck();
        }
    }
    // A final drain leaves both databases identical and consistent.
    cache.Flush();
    store.Flush();
    CheckSameVisible(db_cache, db_store, universe);
    BOOST_CHECK(db_store.GetHeadBlocks().empty());
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
}

/**
 * The settled head after a run is the block whose state the database holds, not the bound: a reorg's
 * inverse changes are tagged with the fork height so that any run carries them all, and the run that
 * does so moves the database to the tier's head. A coin a disconnect restores that was created above
 * the settled height stays out of the database until its creating block is written.
 */
BOOST_AUTO_TEST_CASE(settled_head_follows_the_state_not_the_bound)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    RunBase base{&db};
    CoinsStore store{&base, 4, true};
    const auto hash{[](int h) { return uint256{static_cast<uint8_t>(h)}; }};
    const auto outpoint{[](int n) { return COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(100 + n)}), 0}; }};

    // Blocks 300..307 connected and drained: the database holds the state at 307.
    for (int h{300}; h <= 307; ++h) {
        store.SetBestBlock(hash(h), h);
        store.AddCoin(outpoint(h), Coin{CTxOut{CAmount{h}, CScript() << OP_TRUE}, h, false}, false);
    }
    store.SetChangeHeight(306);
    store.SpendCoin(outpoint(300)); // spent in 306
    store.SetChangeHeight(307);
    store.SpendCoin(outpoint(301)); // spent in 307
    store.Sync();
    BOOST_CHECK_EQUAL(store.WrittenHeight(), 307);
    BOOST_CHECK(store.WrittenHead() == hash(307));

    // Disconnect 307 towards a fork at 301: its inverse changes are tagged 301.
    store.SetChangeHeight(301);
    store.SpendCoin(outpoint(307));                                                              // 307's coinbase goes
    store.AddCoin(outpoint(301), Coin{CTxOut{CAmount{301}, CScript() << OP_TRUE}, 301, false}, false); // 307's spend undone
    store.SetBestBlock(hash(306));
    store.SetTierHeight(306);
    BOOST_CHECK_EQUAL(*OldestDirty(store), 301);

    // A run asked to stop at 301 is final here: the tier fell below the written head (SetTierHeight()), and
    // the changes that take the database back are tagged with the fork. It carries both and leaves nothing
    // dirty: the database then holds 306's state, and that, not 301, is what the next marker must name as
    // its old head.
    auto run{WriteOut(store, /*bound_height=*/301, /*head_height=*/306, hash(306), hash)};
    BOOST_CHECK(run->final);
    BOOST_CHECK_EQUAL(run->count, 2U);
    BOOST_CHECK(run->old_head == hash(307));
    BOOST_CHECK_EQUAL(run->settled_height, 306);
    BOOST_CHECK_EQUAL(store.WrittenHeight(), 306);
    BOOST_CHECK(store.WrittenHead() == hash(306));
    BOOST_CHECK(!OldestDirty(store));

    // Connect 307' and 308' on another branch, unwritten: 307' creates a coin that 308' spends, and 308'
    // also spends a coin the database has (created in 304).
    store.SetBestBlock(hash(57), 307);
    store.AddCoin(outpoint(57), Coin{CTxOut{CAmount{57}, CScript() << OP_TRUE}, 307, false}, false);
    store.SetBestBlock(hash(58), 308);
    store.AddCoin(outpoint(58), Coin{CTxOut{CAmount{58}, CScript() << OP_TRUE}, 308, false}, false);
    BOOST_CHECK(store.SpendCoin(outpoint(57)));
    BOOST_CHECK(store.SpendCoin(outpoint(304)));

    // Disconnect 308' towards a fork at 306. The restored coin from 304 is in the database and takes the
    // fork height; the restored coin from 307' is not, and keeps 307 so that no run below 307 writes it.
    store.SetChangeHeight(306);
    store.SpendCoin(outpoint(58));
    store.AddCoin(outpoint(304), Coin{CTxOut{CAmount{304}, CScript() << OP_TRUE}, 304, false}, false);
    store.AddCoin(outpoint(57), Coin{CTxOut{CAmount{57}, CScript() << OP_TRUE}, 307, false}, false);
    store.SetBestBlock(hash(57));
    store.SetTierHeight(307);
    auto run2{WriteOut(store, /*bound_height=*/306, /*head_height=*/307, hash(57), hash)};
    BOOST_REQUIRE_EQUAL(base.m_writes.size(), 1U);
    BOOST_CHECK(base.m_writes[0].outpoint == outpoint(304));
    BOOST_CHECK(run2->old_head == hash(306));
    // 307' remains dirty (its coinbase and the restored coin), so the database settles at 306 still.
    BOOST_CHECK_EQUAL(run2->settled_height, 306);
    BOOST_CHECK_EQUAL(store.WrittenHeight(), 306);
    BOOST_CHECK(!db.HaveCoin(outpoint(57)));
    BOOST_CHECK(db.HaveCoin(outpoint(304)));

    // A final drain writes 307' in full.
    store.Sync();
    BOOST_CHECK(db.HaveCoin(outpoint(57)));
    BOOST_CHECK(!db.HaveCoin(outpoint(58)));
    BOOST_CHECK_EQUAL(store.WrittenHeight(), 307);
    store.SanityCheck();
}

//! A coin created and spent above the written head cancels in the tier. Disconnecting the spend restores
//! it while the change height is the fork height, which is at or above its creation: it is tagged with
//! its creation height, so that a run bounded between the two carries it. Tagged with the fork height it
//! would be left behind while the run's settled head passed its creation, and a crash in the next run
//! would replay from above the block that creates it, which nothing then writes again.
BOOST_AUTO_TEST_CASE(restored_coin_keeps_its_creation_height)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    const auto hash{[](int h) { return uint256{static_cast<uint8_t>(h - 200)}; }};
    const auto coin{[](int h) { return Coin{CTxOut{CAmount{h}, CScript() << OP_TRUE}, h, false}; }};
    const COutPoint c{Txid::FromUint256(uint256{77}), 0};

    store.SetBestBlock(hash(300), 300);
    store.Sync(); // the database holds the state at 300
    BOOST_REQUIRE_EQUAL(store.WrittenHeight(), 300);
    // 301 to 305; c is created in 302 and spent in 304, each block also creates a coin of its own.
    for (int h{301}; h <= 305; ++h) {
        store.SetBestBlock(hash(h), h);
        store.AddCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(h - 200)}), 0}, coin(h), false);
        if (h == 302) store.AddCoin(c, coin(302), false);
        if (h == 304) BOOST_REQUIRE(store.SpendCoin(c));
    }
    BOOST_CHECK(!store.HaveCoinInCache(c));

    // Disconnect 305 and 304 towards a fork at 303: 304's spend of c is undone.
    store.SetChangeHeight(303);
    BOOST_REQUIRE(store.SpendCoin(COutPoint{Txid::FromUint256(uint256{105}), 0}));
    BOOST_REQUIRE(store.SpendCoin(COutPoint{Txid::FromUint256(uint256{104}), 0}));
    store.AddCoin(c, coin(302), false);
    store.SetBestBlock(hash(303));
    store.SetTierHeight(303);

    // A run bounded at 302 settles the database at 302 or below unless it carries c.
    WriteOut(store, /*bound_height=*/302, /*head_height=*/303, hash(303), hash);
    BOOST_CHECK_EQUAL(store.WrittenHeight(), 302);
    BOOST_CHECK(db.HaveCoin(c));
    store.SanityCheck();
}

//! A coin created and spent inside the lag is never written, not even as an erase, and a spent entry
//! that was written is erased on the next run.
BOOST_AUTO_TEST_CASE(fresh_coins_leave_no_trace)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    RunBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint a{Txid::FromUint256(uint256{10}), 0};
    const COutPoint b{Txid::FromUint256(uint256{11}), 0};
    store.AddCoin(a, RandomCoin(m_rng, 1), false);
    store.AddCoin(b, RandomCoin(m_rng, 1), false);
    store.SetChangeHeight(2);
    BOOST_CHECK(store.SpendCoin(a)); // fresh: dropped outright
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 1U);
    WriteOut(store, 2, 2, uint256{2});
    BOOST_REQUIRE_EQUAL(base.m_writes.size(), 1U); // only b
    BOOST_CHECK(base.m_writes[0].coin.has_value());
    BOOST_CHECK(db.PeekCoin(b).has_value());
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    // b is in the database now; spending it must produce an erase even though it was fresh once.
    store.SetChangeHeight(3);
    BOOST_CHECK(store.SpendCoin(b));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);
    WriteOut(store, 3, 3, uint256{3});
    BOOST_REQUIRE_EQUAL(base.m_writes.size(), 1U);
    BOOST_CHECK(!base.m_writes[0].coin.has_value());
    BOOST_CHECK(!db.PeekCoin(b).has_value());
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
}

//! A spend that lands between collection and commit keeps its entry dirty; a run only settles what it saw.
BOOST_AUTO_TEST_CASE(run_settles_only_unchanged_entries)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    RunBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint a{Txid::FromUint256(uint256{10}), 0};
    store.AddCoin(a, RandomCoin(m_rng, 1), false);
    base.m_in_flight = [&] {
        store.SetChangeHeight(2);
        BOOST_CHECK(store.SpendCoin(a)); // no longer fresh: the run will put it in the database
    };
    WriteOut(store, 1, 1, uint256{1});
    BOOST_CHECK(db.PeekCoin(a).has_value());
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U); // the erase is still owed
    BOOST_CHECK_EQUAL(*OldestDirty(store), 2);
    store.Sync();
    BOOST_CHECK(!db.PeekCoin(a).has_value());
}

//! Clean entries go first, and the bound for a byte target walks the height histogram from the oldest.
BOOST_AUTO_TEST_CASE(evict_clean_and_bound_for_bytes)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    {
        CCoinsViewCache seed{&db};
        for (uint32_t i{0}; i < 50; ++i) seed.AddCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(i + 1)}), 0}, RandomCoin(m_rng, 1), false);
        seed.SetBestBlock(uint256::ONE);
        seed.Flush();
    }
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    for (uint32_t i{0}; i < 50; ++i) BOOST_CHECK(store.HaveCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(i + 1)}), 0}));
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 50U);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    for (int h{2}; h <= 11; ++h) {
        store.SetChangeHeight(h);
        store.AddCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(100 + h)}), 0}, RandomCoin(m_rng, h), false);
    }
    const size_t before{store.DynamicMemoryUsage()};
    store.EvictClean(0);
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 10U);
    BOOST_CHECK(store.DynamicMemoryUsage() < before);
    BOOST_CHECK_EQUAL(*OldestDirty(store), 2);
    const size_t per_height{DirtyBytesAtOrBelow(store, 2)};
    BOOST_CHECK(per_height > 0);
    BOOST_CHECK_EQUAL(store.BoundForBytes(per_height, 11), 2);
    BOOST_CHECK_EQUAL(store.BoundForBytes(per_height * 3, 11), 4);
    BOOST_CHECK_EQUAL(store.BoundForBytes(per_height * 100, 11), 11);
    auto run{WriteOut(store, 4, 11, uint256{11})};
    BOOST_CHECK_EQUAL(run->count, 3U);
    BOOST_CHECK_EQUAL(run->settled_height, 4); // height 5 stays dirty
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 7U);
    BOOST_CHECK_EQUAL(*OldestDirty(store), 5);
    store.SanityCheck();
}

//! The dirty lists are kept by span of heights; sums, bounds and the settled height are exact to the height
//! all the same, inside a span and across spans.
BOOST_AUTO_TEST_CASE(bounds_are_exact_inside_and_across_dirty_spans)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    {
        CCoinsViewCache seed{&db};
        seed.AddCoin(COutPoint{Txid::FromUint256(uint256{200}), 0}, RandomCoin(m_rng, 1), false);
        seed.SetBestBlock(uint256::ONE);
        seed.Flush();
    }
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const std::vector<int> heights{1000, 1020, 1030, 1040, 2100, 5000};
    for (size_t i{0}; i < heights.size(); ++i) {
        store.SetChangeHeight(heights[i]);
        store.AddCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(i + 1)}), 0}, RandomCoin(m_rng, heights[i]), false);
    }
    store.SanityCheck();
    BOOST_CHECK_EQUAL(*OldestDirty(store), 1000);
    const size_t each{DirtyBytesAtOrBelow(store, 1000)};
    BOOST_CHECK(each > 0);
    BOOST_CHECK_EQUAL(DirtyBytesAtOrBelow(store, 999), 0U);
    BOOST_CHECK_EQUAL(DirtyBytesAtOrBelow(store, 1029), each * 2);
    BOOST_CHECK_EQUAL(DirtyBytesAtOrBelow(store, 1030), each * 3);
    BOOST_CHECK_EQUAL(DirtyBytesAtOrBelow(store, 4999), each * 5);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 2, 5000), 1020);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 3, 5000), 1030);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 5, 5000), 2100);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 7, 5000), 5000);

    // A bound inside a span takes the heights at or below it and settles just below the next height, which
    // shares its list with what was written.
    auto run{WriteOut(store, /*bound_height=*/1030, /*head_height=*/5000, uint256{50})};
    BOOST_CHECK_EQUAL(run->count, 3U);
    BOOST_CHECK_EQUAL(run->settled_height, 1039);
    BOOST_CHECK_EQUAL(*OldestDirty(store), 1040);

    // A bound between two spans leaves the lists further up alone.
    run = WriteOut(store, /*bound_height=*/2000, /*head_height=*/5000, uint256{50});
    BOOST_CHECK_EQUAL(run->count, 1U);
    BOOST_CHECK_EQUAL(run->settled_height, 2099);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 2U);
    BOOST_CHECK_EQUAL(*OldestDirty(store), 2100);
    store.SanityCheck();
}

//! A base whose write fails once it is told to.
class FailingBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    bool m_fail{false};
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override
    {
        if (m_fail) throw std::runtime_error{"the write failed"};
        CCoinsViewBacked::WriteRun(writes, new_head, old_head, final);
    }
};

//! A run whose write fails is abandoned and is no longer in flight. What it collected counts as written in
//! the tier and is not in the database, so the tier refuses every write after it.
BOOST_AUTO_TEST_CASE(failed_run_ends_all_writes)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    FailingBase base{&db};
    CoinsStore store{&base, 256, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), 0};
    store.AddCoin(outpoint, RandomCoin(m_rng, 1), false);
    BOOST_CHECK(!store.FlushFailed());
    base.m_fail = true;
    BOOST_CHECK_THROW(store.Sync(), std::runtime_error);
    BOOST_CHECK(!store.RunInFlight());
    BOOST_CHECK(store.FlushFailed());
    base.m_fail = false;
    BOOST_CHECK_THROW(store.Flush(), std::runtime_error);
    BOOST_CHECK(!db.PeekCoin(outpoint));
    // A forced sync refuses as well: it would close the marker over what the failed run had collected.
    BOOST_CHECK_THROW(store.Sync(), std::runtime_error);
    BOOST_CHECK(!db.PeekCoin(outpoint));
}

//! Counts what is written through it: the runs, their changes, and what they come to as database batches.
class CountingBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    Mutex m_mutex;
    size_t m_runs GUARDED_BY(m_mutex){0};
    size_t m_changes GUARDED_BY(m_mutex){0};
    uint64_t m_bytes GUARDED_BY(m_mutex){0};
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        {
            LOCK(m_mutex);
            ++m_runs;
            m_changes += writes.size();
            for (const CoinWrite& write : writes) m_bytes += write.coin ? 45 + write.coin->out.scriptPubKey.size() : 38;
        }
        CCoinsViewBacked::WriteRun(writes, new_head, old_head, final);
    }
};

//! A flush fills its batches across the shards up to the batch size, a batch not being a shard's worth of
//! changes, with several workers writing; the database then holds every change and the tier is empty.
BOOST_AUTO_TEST_CASE(flush_fills_batches_across_shards)
{
    static constexpr uint64_t BATCH{4000};
    CCoinsViewDB db{MemoryDb("store"), {}};
    CountingBase base{&db};
    CoinsStore store{&base, 256, true};
    store.SetBaseWrites(BATCH, /*concurrent=*/true);
    store.SetFlushHelpers(FlushHelpers());
    store.SetBestBlock(uint256::ONE, 1);
    std::map<COutPoint, std::optional<Coin>> expected;
    std::vector<COutPoint> first;
    for (int i{0}; i < 2000; ++i) {
        const COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), 0};
        Coin coin{RandomCoin(m_rng, 1)};
        expected[outpoint] = coin;
        first.push_back(outpoint);
        store.AddCoin(outpoint, std::move(coin), false);
    }
    store.Sync(); // in the database, so that spending them owes an erase
    store.SetBestBlock(uint256{2}, 2);
    for (size_t i{0}; i < first.size(); i += 4) {
        BOOST_REQUIRE(store.SpendCoin(first[i]));
        expected[first[i]] = std::nullopt;
    }
    for (int i{0}; i < 2000; ++i) {
        const COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), 0};
        Coin coin{RandomCoin(m_rng, 2)};
        expected[outpoint] = coin;
        store.AddCoin(outpoint, std::move(coin), false);
    }
    WITH_LOCK(base.m_mutex, base.m_runs = 0; base.m_changes = 0; base.m_bytes = 0);
    store.Flush();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
    BOOST_CHECK(db.GetBestBlock() == uint256{2});
    BOOST_CHECK(db.GetHeadBlocks().empty());
    for (const auto& [outpoint, coin] : expected) {
        BOOST_CHECK_MESSAGE(Same(db.PeekCoin(outpoint), coin), "mismatch at " << outpoint.ToString());
    }
    LOCK(base.m_mutex);
    BOOST_CHECK_EQUAL(base.m_changes, 2500U);
    // Batches of whole shards, each within a shard's changes of the batch size, then what each worker
    // had left, and the run that closes the marker: far fewer and far larger than a run for each of the
    // 256 shards would be.
    BOOST_CHECK_GE(base.m_runs, base.m_bytes / (BATCH + 100));
    BOOST_CHECK_LE(base.m_runs, base.m_bytes / (BATCH - 1000) + 5);
    store.SanityCheck();
}

//! Records the range of txids of every run written through it, in the order the database has its keys in.
class RangeBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    Mutex m_mutex;
    std::vector<std::pair<uint256, uint256>> m_ranges GUARDED_BY(m_mutex);
    //! Runs whose changes were not in the descending order of the keys.
    size_t m_unordered GUARDED_BY(m_mutex){0};
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        if (!writes.empty()) {
            const auto [lo, hi]{std::ranges::minmax_element(writes, {}, [](const CoinWrite& write) { return write.outpoint.hash.ToUint256(); })};
            const bool ordered{std::ranges::is_sorted(writes, [](const CoinWrite& a, const CoinWrite& b) { return b.outpoint < a.outpoint; })};
            LOCK(m_mutex);
            m_ranges.emplace_back(lo->outpoint.hash.ToUint256(), hi->outpoint.hash.ToUint256());
            if (!ordered) ++m_unordered;
        }
        CCoinsViewBacked::WriteRun(writes, new_head, old_head, final);
    }
};

//! A flush of shards that are small against a batch writes batches of whole neighbouring shards: ranges
//! of the database's keys that do not reach into one another, from several workers.
BOOST_AUTO_TEST_CASE(flush_batches_are_disjoint_key_ranges)
{
    static constexpr uint64_t BATCH{20000};
    CCoinsViewDB db{MemoryDb("store"), {}};
    RangeBase base{&db};
    CoinsStore store{&base, 64};
    store.SetBaseWrites(BATCH, /*concurrent=*/true);
    store.SetFlushHelpers(FlushHelpers());
    store.SetBestBlock(uint256::ONE, 1);
    std::map<COutPoint, Coin> expected;
    for (int i{0}; i < 4000; ++i) {
        const COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), uint32_t(i % 3)};
        Coin coin{RandomCoin(m_rng, 1)};
        expected[outpoint] = coin;
        store.AddCoin(outpoint, std::move(coin), false);
    }
    store.Flush();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
    BOOST_CHECK(db.GetBestBlock() == uint256::ONE);
    BOOST_CHECK(db.GetHeadBlocks().empty());
    for (const auto& [outpoint, coin] : expected) {
        BOOST_CHECK_MESSAGE(Same(db.PeekCoin(outpoint), coin), "mismatch at " << outpoint.ToString());
    }
    LOCK(base.m_mutex);
    // A shard's share is a fifth of a batch or so: a batch is a few shards, and there are many batches.
    BOOST_CHECK_GE(base.m_ranges.size(), 8U);
    BOOST_CHECK_LE(base.m_ranges.size(), 64U);
    std::ranges::sort(base.m_ranges);
    for (size_t i{1}; i < base.m_ranges.size(); ++i) {
        BOOST_CHECK_MESSAGE(base.m_ranges[i - 1].second < base.m_ranges[i].first, "batches " << i - 1 << " and " << i << " overlap");
    }
}

//! A shard larger than a batch is written as one run in the order of its keys, the outputs of one
//! transaction among themselves included, and never as two runs that both cover its range: the database
//! cuts the run at its batch size, and holds every change.
BOOST_AUTO_TEST_CASE(flush_never_ends_a_batch_inside_a_shard)
{
    static constexpr uint64_t BATCH{4000};
    CCoinsViewDB db{MemoryDb("store"), {.batch_write_bytes = BATCH, .simulate_crash_ratio = 0}};
    RangeBase base{&db};
    CoinsStore store{&base, 4};
    store.SetBaseWrites(BATCH, /*concurrent=*/true);
    store.SetFlushHelpers(FlushHelpers());
    store.SetBestBlock(uint256::ONE, 1);
    std::map<COutPoint, Coin> expected;
    for (int i{0}; i < 500; ++i) {
        const Txid txid{Txid::FromUint256(m_rng.rand256())};
        for (uint32_t n{0}; n < 4; ++n) {
            Coin coin{RandomCoin(m_rng, 1)};
            expected[COutPoint{txid, n}] = coin;
            store.AddCoin(COutPoint{txid, n}, std::move(coin), false);
        }
    }
    store.Flush();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
    BOOST_CHECK(db.GetBestBlock() == uint256::ONE);
    BOOST_CHECK(db.GetHeadBlocks().empty());
    for (const auto& [outpoint, coin] : expected) {
        BOOST_CHECK_MESSAGE(Same(db.PeekCoin(outpoint), coin), "mismatch at " << outpoint.ToString());
    }
    LOCK(base.m_mutex);
    // A shard's share is six batches or so, and it is one run.
    BOOST_CHECK_EQUAL(base.m_ranges.size(), 4U);
    BOOST_CHECK_EQUAL(base.m_unordered, 0U);
    std::ranges::sort(base.m_ranges);
    for (size_t i{1}; i < base.m_ranges.size(); ++i) {
        BOOST_CHECK_MESSAGE(base.m_ranges[i - 1].second < base.m_ranges[i].first, "batches " << i - 1 << " and " << i << " overlap");
    }
}

//! A run that stops at a bound ends by recording the settled head in the database: the old head on disk is
//! the block the tier counts as written, never the head of the run before.
BOOST_AUTO_TEST_CASE(streamed_run_records_the_settled_head)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    const auto hash{[](int height) { return uint256{static_cast<uint8_t>(height)}; }};
    const auto add{[&](int height) {
        store.SetBestBlock(hash(height), height);
        store.AddCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(100 + height)}), 0}, RandomCoin(m_rng, height), false);
    }};
    for (int height{1}; height <= 4; ++height) add(height);

    auto run{store.BeginWriteOut(/*bound_height=*/2, /*head_height=*/4, hash(4), /*final=*/false)};
    BOOST_REQUIRE(run);
    store.StreamWriteOut(*run, hash);
    store.CommitWriteOut(*run);
    BOOST_CHECK_EQUAL(run->count, 2U);
    BOOST_CHECK_EQUAL(run->settled_height, 2);
    BOOST_CHECK((db.GetHeadBlocks() == std::vector<uint256>{hash(4), hash(2)}));
    BOOST_CHECK(store.WrittenHead() == hash(2));

    // The next run starts from that head and leaves its own.
    add(5);
    run = store.BeginWriteOut(/*bound_height=*/3, /*head_height=*/5, hash(5), /*final=*/false);
    BOOST_REQUIRE(run);
    BOOST_CHECK(run->old_head == hash(2));
    store.StreamWriteOut(*run, hash);
    store.CommitWriteOut(*run);
    BOOST_CHECK((db.GetHeadBlocks() == std::vector<uint256>{hash(5), hash(3)}));
    BOOST_CHECK(store.WrittenHead() == hash(3));
    store.SanityCheck();
}

//! A streamed run whose chunk holds one change, so that every change is a chunk of its own and a chunk ends
//! inside each shard that holds entries, leaves the database, the heads and the tier as a run of one chunk leaves
//! them, once it is committed. The database at a chunk boundary is not looked at.
BOOST_AUTO_TEST_CASE(streamed_run_in_chunks_of_one_change)
{
    const auto hash{[](int height) { return uint256{static_cast<uint8_t>(height)}; }};
    static constexpr int HEIGHTS{6};
    static constexpr int PER_HEIGHT{8};
    const auto outpoint{[](int height, int n) { return COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(height * 16 + n)}), 0}; }};
    std::vector<Coin> coins;
    for (int height{1}; height <= HEIGHTS; ++height) {
        for (int n{0}; n < PER_HEIGHT; ++n) coins.push_back(RandomCoin(m_rng, height));
    }
    const auto coin{[&](int height, int n) { return coins[(height - 1) * PER_HEIGHT + n]; }};
    const auto add{[&](CoinsStore& store, int height) {
        store.SetBestBlock(hash(height), height);
        for (int n{0}; n < PER_HEIGHT; ++n) store.AddCoin(outpoint(height, n), coin(height, n), false);
    }};

    CCoinsViewDB db_one{MemoryDb("one"), {}};
    CCoinsViewDB db_many{MemoryDb("many"), {}};
    CoinsStore store_one{&db_one, 4, true};
    CoinsStore store_many{&db_many, 4, true};
    // Returns the number of changes the run wrote and the number of chunks it wrote them in.
    const auto run_to{[&](CoinsStore& store, uint64_t batch_bytes) {
        // Two heights that are in the database before the run, so that spends of their coins owe an erase.
        add(store, 1);
        add(store, 2);
        store.Sync();
        add(store, 3);
        for (int n{0}; n < PER_HEIGHT / 2; ++n) BOOST_REQUIRE(store.SpendCoin(outpoint(1, n)));
        add(store, 4);
        add(store, 5);
        // A coin the database holds and one it never held, both spent above the run's bound.
        BOOST_REQUIRE(store.SpendCoin(outpoint(2, 0)));
        BOOST_REQUIRE(store.SpendCoin(outpoint(4, 0)));
        add(store, 6);
        auto run{store.BeginWriteOut(/*bound_height=*/4, /*head_height=*/6, hash(6), /*final=*/false)};
        BOOST_REQUIRE(run);
        store.StreamWriteOut(*run, hash, batch_bytes);
        store.CommitWriteOut(*run);
        store.SanityCheck();
        return std::pair{run->count, run->chunks};
    }};
    const auto [count_one, chunks_one]{run_to(store_one, DEFAULT_DB_CACHE_BATCH)};
    const auto [count_many, chunks_many]{run_to(store_many, /*batch_bytes=*/1)};
    BOOST_CHECK_GT(count_one, 1U);
    BOOST_CHECK_EQUAL(count_one, count_many);
    // The limit is kept: one chunk where it is large, several where it is one byte.
    BOOST_CHECK_EQUAL(chunks_one, 1U);
    BOOST_CHECK_GT(chunks_many, 1U);

    const auto check_equal{[&] {
        BOOST_CHECK(db_one.GetHeadBlocks() == db_many.GetHeadBlocks());
        BOOST_CHECK(db_one.GetBestBlock() == db_many.GetBestBlock());
        BOOST_CHECK(store_one.WrittenHead() == store_many.WrittenHead());
        for (int height{1}; height <= HEIGHTS; ++height) {
            for (int n{0}; n < PER_HEIGHT; ++n) {
                const auto one{db_one.GetCoin(outpoint(height, n))};
                const auto many{db_many.GetCoin(outpoint(height, n))};
                BOOST_REQUIRE_EQUAL(one.has_value(), many.has_value());
                if (one) BOOST_CHECK(one->out == many->out && one->nHeight == many->nHeight && one->fCoinBase == many->fCoinBase);
                BOOST_CHECK_EQUAL(store_one.HaveCoin(outpoint(height, n)), store_many.HaveCoin(outpoint(height, n)));
            }
        }
    }};
    check_equal();
    // The run wrote something of its heights and nothing above its settled head.
    BOOST_CHECK(db_many.GetCoin(outpoint(3, 1)).has_value());
    BOOST_CHECK(!db_many.GetCoin(outpoint(6, 1)).has_value());
    // What the bound decides: the coins of the first height spent below it are erased and the others of
    // that height are there, and a coin the database holds that is spent above the bound is there still.
    // A coin made and spent before the database held it is never written.
    for (int n{0}; n < PER_HEIGHT; ++n) BOOST_CHECK_EQUAL(db_many.GetCoin(outpoint(1, n)).has_value(), n >= PER_HEIGHT / 2);
    BOOST_CHECK(db_many.GetCoin(outpoint(2, 0)).has_value());
    BOOST_CHECK(!db_many.GetCoin(outpoint(4, 0)).has_value());

    // And the rest, written by each store the way it writes: the same again.
    store_one.Sync();
    store_many.Sync();
    check_equal();
    BOOST_CHECK(db_many.GetBestBlock() == hash(6));
    BOOST_CHECK(db_many.GetCoin(outpoint(6, 1)).has_value());
    BOOST_CHECK(!db_many.GetCoin(outpoint(2, 0)).has_value());
    BOOST_CHECK(!db_many.GetCoin(outpoint(4, 0)).has_value());
    store_many.SanityCheck();
}

//! Changes made under a block's identity are seen by appliers and not by readers until the block is
//! confirmed; unwinding makes them disappear; conflicts are told apart by height.
//! A base whose n-th write fails, counted from one; no write fails while n is zero.
class FailingAtBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    int m_calls{0};
    int m_fail_at{0};
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override
    {
        if (++m_calls == m_fail_at) throw std::runtime_error{"the write failed"};
        CCoinsViewBacked::WriteRun(writes, new_head, old_head, final);
    }
};

//! A streamed run that ends at any of its writes, as a node that stops there leaves it, leaves a database
//! the replay completes: the marker names the run's head and the head before the run, and applying the
//! blocks between the two again, as Chainstate::ReplayBlocks() does through a cache on the database, gives
//! the database of a node that never stopped. Every write of a run of one change per chunk is tried.
BOOST_AUTO_TEST_CASE(streamed_run_cut_at_any_write_is_completed_by_a_replay)
{
    const auto hash{[](int height) { return uint256{static_cast<uint8_t>(height)}; }};
    static constexpr int HEIGHTS{6};
    static constexpr int PER_HEIGHT{8};
    static constexpr int WRITTEN{2}; //!< the height the database is at before the run
    const auto outpoint{[](int height, int n) { return COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(height * 16 + n)}), 0}; }};
    std::vector<Coin> coins;
    for (int height{1}; height <= HEIGHTS; ++height) {
        for (int n{0}; n < PER_HEIGHT; ++n) coins.push_back(RandomCoin(m_rng, height));
    }
    const auto coin{[&](int height, int n) { return coins[(height - 1) * PER_HEIGHT + n]; }};
    // The changes of a block: its coins, and at two heights spends of coins the database holds and of one
    // it never held.
    const auto block{[&](auto& view, int height, bool overwrite) {
        for (int n{0}; n < PER_HEIGHT; ++n) view.AddCoin(outpoint(height, n), coin(height, n), overwrite);
        if (height == 3) {
            for (int n{0}; n < PER_HEIGHT / 2; ++n) view.SpendCoin(outpoint(1, n));
        } else if (height == 5) {
            view.SpendCoin(outpoint(2, 0));
            view.SpendCoin(outpoint(4, 0));
        }
    }};

    // The database of a node that never stopped.
    CCoinsViewDB db_whole{MemoryDb("whole"), {}};
    {
        CCoinsViewCache cache{&db_whole};
        for (int height{1}; height <= HEIGHTS; ++height) block(cache, height, /*overwrite=*/false);
        cache.SetBestBlock(hash(HEIGHTS));
        cache.Flush();
    }

    // Returns the number of writes the run made, all of them if none failed.
    const auto run_cut_at{[&](int fail_at) {
        CCoinsViewDB db{MemoryDb("cut"), {}};
        FailingAtBase base{&db};
        CoinsStore store{&base, 4, true};
        for (int height{1}; height <= HEIGHTS; ++height) {
            store.SetBestBlock(hash(height), height);
            block(store, height, /*overwrite=*/false);
            if (height == WRITTEN) store.Sync();
        }
        auto run{store.BeginWriteOut(/*bound_height=*/4, /*head_height=*/HEIGHTS, hash(HEIGHTS), /*final=*/false)};
        BOOST_REQUIRE(run);
        base.m_calls = 0;
        base.m_fail_at = fail_at;
        if (fail_at == 0) {
            store.StreamWriteOut(*run, hash, /*batch_bytes=*/1);
            store.CommitWriteOut(*run);
            return base.m_calls;
        }
        BOOST_CHECK_THROW(store.StreamWriteOut(*run, hash, /*batch_bytes=*/1), std::runtime_error);
        // The run is the caller's to abandon, as the write-out's thread does.
        store.AbandonWriteOut();
        BOOST_CHECK(!store.RunInFlight());
        BOOST_CHECK(store.FlushFailed());
        if (fail_at == 1) {
            // Nothing of the run is in the database.
            BOOST_CHECK(db.GetHeadBlocks().empty());
            BOOST_CHECK(db.GetBestBlock() == hash(WRITTEN));
        } else {
            BOOST_CHECK((db.GetHeadBlocks() == std::vector<uint256>{hash(HEIGHTS), hash(WRITTEN)}));
            BOOST_CHECK(db.GetBestBlock().IsNull());
        }

        // The replay: the blocks above the old head again, whatever the database holds of them.
        {
            CCoinsViewCache replay{&db};
            for (int height{WRITTEN + 1}; height <= HEIGHTS; ++height) block(replay, height, /*overwrite=*/true);
            replay.SetBestBlock(hash(HEIGHTS));
            replay.Flush();
        }
        BOOST_CHECK(db.GetHeadBlocks().empty());
        BOOST_CHECK(db.GetBestBlock() == hash(HEIGHTS));
        for (int height{1}; height <= HEIGHTS; ++height) {
            for (int n{0}; n < PER_HEIGHT; ++n) {
                BOOST_CHECK_MESSAGE(Same(db.PeekCoin(outpoint(height, n)), db_whole.PeekCoin(outpoint(height, n))),
                                    "cut at write " << fail_at << ": the database differs at coin " << n << " of height " << height);
            }
        }
        return base.m_calls;
    }};

    const int writes{run_cut_at(0)};
    // A chunk for each change at or below the run's bound, and the write that closes the marker.
    BOOST_REQUIRE_GT(writes, 2 * PER_HEIGHT);
    for (int fail_at{1}; fail_at <= writes; ++fail_at) BOOST_CHECK_EQUAL(run_cut_at(fail_at), fail_at);
}

//! WindowOverlay exposes outputs strictly below viewer_height, excludes intra-block spends,
//! handles BIP30 duplicate outpoints by returning the highest below viewer_height, and cleans up on RemoveBlocks/Clear.
BOOST_AUTO_TEST_CASE(window_overlay_and_intra_block_spends)
{
    WindowOverlay overlay;
    const auto make_tx{[](const std::vector<COutPoint>& ins, CAmount val, uint32_t tag) {
        CMutableTransaction tx;
        for (const COutPoint& in : ins) tx.vin.emplace_back(in);
        tx.vout.emplace_back(val, CScript() << tag);
        return MakeTransactionRef(std::move(tx));
    }};

    // Block 2: coinbase + tx1 (creates op1) + tx2 (spends op1 intra-block, creates op2).
    auto b2{std::make_shared<CBlock>()};
    const auto cb2{make_tx({COutPoint{}}, 50 * COIN, 2)};
    const auto tx1{make_tx({COutPoint{Txid::FromUint256(uint256{99}), 0}}, 10 * COIN, 21)};
    const COutPoint op1{tx1->GetHash(), 0};
    const auto tx2{make_tx({op1}, 9 * COIN, 22)};
    const COutPoint op2{tx2->GetHash(), 0};
    b2->vtx = {cb2, tx1, tx2};

    overlay.AddBlockOutputs(2, b2);
    // Intra-block spent output op1 must NEVER appear in WindowOverlay!
    BOOST_CHECK(!overlay.Lookup(3, op1).has_value());
    // Unspent outputs of block 2 are visible at viewer_height > 2, NOT at viewer_height <= 2.
    BOOST_CHECK(!overlay.Lookup(2, op2).has_value());
    const auto got2{overlay.Lookup(3, op2)};
    BOOST_REQUIRE(got2.has_value());
    BOOST_CHECK_EQUAL(got2->out.nValue, 9 * COIN);
    BOOST_CHECK_EQUAL(got2->nHeight, 2U);
    BOOST_CHECK(!got2->IsCoinBase());

    const COutPoint cb2_op{cb2->GetHash(), 0};
    const auto got_cb2{overlay.Lookup(3, cb2_op)};
    BOOST_REQUIRE(got_cb2.has_value());
    BOOST_CHECK(got_cb2->IsCoinBase());

    // Batch Fetch separates hits and misses.
    const COutPoint missing{Txid::FromUint256(uint256{88}), 0};
    std::vector<std::pair<COutPoint, Coin>> found;
    std::vector<COutPoint> misses;
    const std::vector<COutPoint> req{op1, op2, cb2_op, missing};
    overlay.Fetch(3, req, found, misses);
    BOOST_CHECK_EQUAL(found.size(), 2U);
    BOOST_CHECK_EQUAL(misses.size(), 2U);

    // BIP30 duplicate outpoint at height 4: viewer at 3 sees height 2, viewer at 5 sees height 4.
    auto b4{std::make_shared<CBlock>()};
    b4->vtx = {cb2};
    overlay.AddBlockOutputs(4, b4);
    BOOST_CHECK_EQUAL(overlay.Lookup(3, cb2_op)->nHeight, 2U);
    BOOST_CHECK_EQUAL(overlay.Lookup(5, cb2_op)->nHeight, 4U);

    // Removing block 2 leaves block 4's duplicate intact.
    const std::array<int, 1> rem2{2};
    overlay.RemoveBlocks(rem2);
    BOOST_CHECK(!overlay.Lookup(3, cb2_op).has_value());
    BOOST_CHECK(!overlay.Lookup(3, op2).has_value());
    BOOST_REQUIRE(overlay.Lookup(5, cb2_op).has_value());
    BOOST_CHECK_EQUAL(overlay.Lookup(5, cb2_op)->nHeight, 4U);

    overlay.Clear();
    BOOST_CHECK(!overlay.Lookup(5, cb2_op).has_value());
}

//! CommitBatch applies confirmed ops in exact height order across shards in parallel, skips cancelled ops,
//! and leaves CoinsStore identical to a reference CCoinsViewCache.
BOOST_AUTO_TEST_CASE(commit_batch_parallel_and_cancellation)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CCoinsViewDB ref_db{MemoryDb("ref"), {}};
    CoinsStore store{&db, 16, true};
    CCoinsViewCache ref{&ref_db};
    store.SetBestBlock(uint256{1}, 1);
    ref.SetBestBlock(uint256{1});

    const auto pool{FlushHelpers()};
    // Seed 400 coins (triggers parallel shard commit since > COMMIT_OPS_PER_HELPER).
    std::vector<CoinsStore::CommitOp> ops;
    ops.reserve(600);
    for (uint32_t i{0}; i < 400; ++i) {
        const COutPoint op{Txid::FromUint256(uint256{static_cast<uint8_t>(i & 0xff)}), i};
        const Coin coin{RandomCoin(m_rng, 2)};
        ref.AddCoin(op, Coin{coin}, false);
        ops.push_back({.outpoint = op, .coin = Coin{coin}, .key_hash = CoinsStore::KeyHashOf(op), .shard = store.ShardOf(op), .height = 2});
    }
    // Add 50 cancelled ops that must be ignored.
    for (uint32_t i{500}; i < 550; ++i) {
        const COutPoint op{Txid::FromUint256(uint256{static_cast<uint8_t>(i & 0xff)}), i};
        ops.push_back({.outpoint = op, .coin = RandomCoin(m_rng, 2), .key_hash = CoinsStore::KeyHashOf(op), .shard = store.ShardOf(op), .height = 2, .cancelled = true});
    }
    store.CommitBatch(ops, uint256{2}, 2, pool.get());
    BOOST_CHECK_EQUAL(store.ConfirmedHeight(), 2);
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 400U);
    store.SanityCheck();

    // Spend 300 of them and create 200 new ones in a second parallel batch.
    std::vector<CoinsStore::CommitOp> ops2;
    for (uint32_t i{0}; i < 300; ++i) {
        const COutPoint op{Txid::FromUint256(uint256{static_cast<uint8_t>(i & 0xff)}), i};
        BOOST_REQUIRE(ref.SpendCoin(op));
        ops2.push_back({.outpoint = op, .coin = Coin{}, .key_hash = CoinsStore::KeyHashOf(op), .shard = store.ShardOf(op), .height = 3});
    }
    for (uint32_t i{600}; i < 800; ++i) {
        const COutPoint op{Txid::FromUint256(uint256{static_cast<uint8_t>(i & 0xff)}), i};
        const Coin coin{RandomCoin(m_rng, 3)};
        ref.AddCoin(op, Coin{coin}, false);
        ops2.push_back({.outpoint = op, .coin = Coin{coin}, .key_hash = CoinsStore::KeyHashOf(op), .shard = store.ShardOf(op), .height = 3});
    }
    store.CommitBatch(ops2, uint256{3}, 3, pool.get());
    BOOST_CHECK_EQUAL(store.ConfirmedHeight(), 3);
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 300U);
    for (uint32_t i{0}; i < 800; ++i) {
        const COutPoint op{Txid::FromUint256(uint256{static_cast<uint8_t>(i & 0xff)}), i};
        BOOST_CHECK(Same(store.PeekCoin(op), ref.PeekCoin(op)));
    }
    store.SanityCheck();
}

//! The tier's byte count stays exact for coins whose scripts are on the heap when emptied.
BOOST_AUTO_TEST_CASE(emptied_entries_count_no_script_bytes)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const auto big{[](int height) { return Coin{CTxOut{CAmount{1000}, CScript{} << std::vector<unsigned char>(100, 0x42)}, height, /*fCoinBaseIn=*/false}; }};
    const COutPoint x{Txid::FromUint256(uint256{10}), 0};
    store.AddCoin(x, big(1), false);
    store.Sync();
    store.SetChangeHeight(2);
    BOOST_CHECK(store.SpendCoin(x));
    BOOST_CHECK(!store.HaveCoin(x));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);
    store.SanityCheck();
    store.Sync();
    BOOST_CHECK(!db.HaveCoin(x));
    store.SanityCheck();
}

//! Blocks the database holds are disconnected and connected again, past the written head:
//! the next write-out run is promoted to final (CS-9).
BOOST_AUTO_TEST_CASE(run_after_a_disconnect_below_the_written_head_is_final)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    RunBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const COutPoint x{Txid::FromUint256(uint256{10}), 0};
    store.SetBestBlock(uint256{2}, 2);
    store.AddCoin(x, RandomCoin(m_rng, 2), false);
    store.SetBestBlock(uint256{3}, 3);
    store.Sync();
    BOOST_CHECK(db.HaveCoin(x));

    // Blocks 3 and 2 are disconnected: the erase of x is a change of the fork height.
    store.SetChangeHeight(1);
    BOOST_CHECK(store.SpendCoin(x));
    store.SetBestBlock(uint256{1});
    store.SetTierHeight(1);
    // Reconnect via CommitBatch.
    CommitChanges(store, {{.outpoint = x, .coin = RandomCoin(m_rng, 2), .height = 2, .possible_overwrite = false}}, uint256{2}, 2);
    for (uint8_t height{3}; height <= 5; ++height) store.SetBestBlock(uint256{height}, height);

    auto run{WriteOut(store, /*bound_height=*/1, /*head_height=*/5, uint256{5})};
    BOOST_CHECK(run->final);
    for (const auto& write : base.m_writes) BOOST_CHECK(write.outpoint != x || write.coin.has_value());
    BOOST_CHECK(db.HaveCoin(x));
    BOOST_CHECK_EQUAL(store.WrittenHeight(), 5);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    store.SanityCheck();

    // Subsequent run stops at its bound again.
    store.SetBestBlock(uint256{6}, 6);
    store.AddCoin(COutPoint{Txid::FromUint256(uint256{11}), 0}, RandomCoin(m_rng, 6), false);
    store.SetBestBlock(uint256{7}, 7);
    store.AddCoin(COutPoint{Txid::FromUint256(uint256{12}), 0}, RandomCoin(m_rng, 7), false);
    store.SetBestBlock(uint256{8}, 8);
    run = WriteOut(store, /*bound_height=*/6, /*head_height=*/8, uint256{8});
    BOOST_CHECK(!run->final);
    BOOST_CHECK_EQUAL(run->count, 1U);
    BOOST_CHECK_EQUAL(store.WrittenHeight(), 6);
    store.SanityCheck();
}

//! EmplaceCoinInternalDANGER must register dirty counts and dirty spans so Flush/WriteOut persist the coin (CS-1).
BOOST_AUTO_TEST_CASE(emplace_coin_internal_danger_updates_dirty_and_height)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const COutPoint op{Txid::FromUint256(uint256{42}), 0};
    store.EmplaceCoinInternalDANGER(op, RandomCoin(m_rng, 1));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 1U);
    store.SanityCheck();
    store.Flush();
    BOOST_CHECK(db.HaveCoin(op));
    store.SanityCheck();
}

//! BlockApplyView validates read-only and commits at Confirm(); Reset() leaves CoinsStore untouched.
BOOST_AUTO_TEST_CASE(block_apply_view_applies_at_confirm)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint old_coin{Txid::FromUint256(uint256{10}), 0};
    store.AddCoin(old_coin, RandomCoin(m_rng, 1), false);
    util::NotNull pool{std::make_shared<ThreadPool>("test")};
    BlockApplyView view{store, pool, /*deterministic=*/true};

    const COutPoint a{Txid::FromUint256(uint256{20}), 0};
    const COutPoint b{Txid::FromUint256(uint256{21}), 0};
    view.PrepareBlock(2);
    BOOST_CHECK(view.InUse());
    BOOST_CHECK(view.HaveCoin(old_coin));
    Coin spent;
    BOOST_CHECK(view.SpendCoin(old_coin, &spent));
    BOOST_CHECK(!spent.IsSpent());
    view.AddCoin(a, RandomCoin(m_rng, 2), false);
    view.AddCoin(b, RandomCoin(m_rng, 2), false);
    BOOST_CHECK(view.HaveCoin(a));
    BOOST_CHECK(view.SpendCoin(a));
    BOOST_CHECK(!view.HaveCoin(a));
    view.SetBestBlock(uint256{2});
    // Readers see nothing before Confirm().
    BOOST_CHECK(store.HaveCoin(old_coin));
    BOOST_CHECK(!store.HaveCoin(b));
    view.Confirm(uint256{2});
    BOOST_CHECK(!view.InUse());
    BOOST_CHECK(!store.HaveCoin(old_coin));
    BOOST_CHECK(store.HaveCoin(b));
    BOOST_CHECK(!store.HaveCoinInCache(a));
    BOOST_CHECK(store.GetBestBlock() == uint256{2});
    BOOST_CHECK_EQUAL(store.GetChangeHeight(), 2);
    store.SanityCheck();

    // Reset() leaves the store untouched.
    view.PrepareBlock(3);
    const COutPoint c{Txid::FromUint256(uint256{22}), 0};
    view.AddCoin(c, RandomCoin(m_rng, 3), false);
    BOOST_CHECK(view.SpendCoin(b));
    view.Reset();
    BOOST_CHECK(!view.InUse());
    BOOST_CHECK(store.HaveCoin(b));
    BOOST_CHECK(!store.HaveCoinInCache(c));
    store.SanityCheck();
}

//! A base whose first read of one outpoint is overtaken: `overtake` runs after the coin was read and before
//! it is returned. PeekCoin rechecks shard.map when shard.settles advances (CS-8).
class OvertakenBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    COutPoint m_watched;
    mutable std::function<void()> m_overtake;
    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override
    {
        auto coin{CCoinsViewBacked::GetCoin(outpoint)};
        if (outpoint == m_watched && m_overtake) std::exchange(m_overtake, nullptr)();
        return coin;
    }
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override
    {
        auto coin{CCoinsViewBacked::PeekCoin(outpoint)};
        if (outpoint == m_watched && m_overtake) std::exchange(m_overtake, nullptr)();
        return coin;
    }
};

BOOST_AUTO_TEST_CASE(peek_coin_rechecks_on_concurrent_settle)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    OvertakenBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint coin{Txid::FromUint256(uint256{10}), 0};
    store.AddCoin(coin, RandomCoin(m_rng, 1), false);
    store.Sync();
    store.EvictClean(0);
    BOOST_REQUIRE_EQUAL(store.GetCacheSize(), 0U);

    base.m_watched = coin;
    base.m_overtake = [&] {
        store.SetBestBlock(uint256{2}, 2);
        BOOST_REQUIRE(store.SpendCoin(coin));
        store.Sync();
        BOOST_CHECK(!db.PeekCoin(coin).has_value());
    };
    // Even though the first base read saw the old coin, PeekCoin notices shard.settles changed and re-reads!
    BOOST_CHECK(!store.PeekCoin(coin).has_value());
    BOOST_CHECK(!base.m_overtake);
    store.SanityCheck();
}

//! The map nodes one thread frees, more than a thread keeps, serve another thread's insertions, and the
//! coins that land in them are whole.
BOOST_AUTO_TEST_CASE(freed_nodes_serve_another_thread)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    constexpr uint32_t COUNT{5000};
    const auto outpoint{[](uint8_t round, uint32_t i) { return COutPoint{Txid::FromUint256(uint256{round}), i}; }};
    const auto add{[&](uint8_t round) {
        for (uint32_t i{0}; i < COUNT; ++i) {
            store.AddCoin(outpoint(round, i), Coin{CTxOut{CAmount{i} + round, CScript{} << OP_TRUE}, 1, false}, false);
        }
    }};
    std::thread{add, uint8_t{1}}.join();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), COUNT);
    store.SetChangeHeight(2);
    uint32_t spent{0};
    std::thread{[&] {
        for (uint32_t i{0}; i < COUNT; ++i) spent += store.SpendCoin(outpoint(1, i));
    }}.join();
    BOOST_CHECK_EQUAL(spent, COUNT);
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
    std::thread{add, uint8_t{2}}.join();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), COUNT);
    for (uint32_t i{0}; i < COUNT; ++i) {
        BOOST_CHECK(!store.PeekCoin(outpoint(1, i)));
        const std::optional<Coin> coin{store.PeekCoin(outpoint(2, i))};
        BOOST_REQUIRE(coin);
        BOOST_CHECK_EQUAL(coin->out.nValue, CAmount{i} + 2);
    }
    // Many churn rounds of fresh add+spend keep memory bounded and Flush() reclaims 100% of table memory.
    const size_t after_first_round{store.DynamicMemoryUsage()};
    for (uint8_t round{3}; round < 23; ++round) {
        std::thread{[&] {
            for (uint32_t i{0}; i < COUNT; ++i) store.SpendCoin(outpoint(static_cast<uint8_t>(round - 1), i));
        }}.join();
        BOOST_REQUIRE_EQUAL(store.GetCacheSize(), 0U);
        std::thread{add, round}.join();
    }
    BOOST_CHECK_EQUAL(store.GetCacheSize(), COUNT);
    BOOST_CHECK_LE(store.DynamicMemoryUsage(), after_first_round * 2);
    store.Flush();
    BOOST_CHECK_EQUAL(store.DynamicMemoryUsage(), 0U);
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
    store.SanityCheck();
}

//! A BIP30 re-create over an owed erase keeps the erase's height; a bounded write-out below the re-create
//! height writes an erase (not the future coin!) so replay from the bound stays consistent (CS-5).
BOOST_AUTO_TEST_CASE(disconnect_keeps_the_owed_erase_height)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const COutPoint x{Txid::FromUint256(uint256{30}), 0};
    store.AddCoin(x, RandomCoin(m_rng, 1), false);
    store.Sync();
    BOOST_REQUIRE(db.HaveCoin(x));
    store.SetBestBlock(uint256{2}, 2);
    BOOST_REQUIRE(store.SpendCoin(x)); // erase owed at height 2
    store.AddCoin(COutPoint{Txid::FromUint256(uint256{31}), 0}, RandomCoin(m_rng, 2), false);
    store.SetBestBlock(uint256{3}, 3);
    // BIP30 re-create at height 4 over owed erase from height 2.
    CommitChanges(store, {{.outpoint = x, .coin = RandomCoin(m_rng, 4), .height = 4, .possible_overwrite = true}}, uint256{4}, 4);

    // Bounded write-out at height 2 must NOT write height-4's coin to DB; it erases x (and leaves height-4 coin dirty in tier).
    WriteOut(store, /*bound_height=*/2, /*head_height=*/4, uint256{4});
    BOOST_CHECK(!db.HaveCoin(x));
    BOOST_CHECK(store.HaveCoin(x));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);
    store.SanityCheck();

    // Syncing to head 4 writes the height-4 coin.
    store.Sync();
    BOOST_REQUIRE(db.PeekCoin(x).has_value());
    BOOST_CHECK_EQUAL(db.PeekCoin(x)->nHeight, 4U);
    store.SanityCheck();
}

//! A StoredCoin gives back the coin it was made from, byte for byte, whatever the script's length: inline up to
//! StoredCoin::INLINE bytes, on the heap beyond; copies, moves and assignments keep it, and it reports heap
//! memory only for a script on the heap.
BOOST_AUTO_TEST_CASE(stored_coin_round_trip)
{
    const auto make_coin{[](size_t script_size, CAmount value, uint32_t height, bool coinbase) {
        CScript script;
        for (size_t i{0}; i < script_size; ++i) script.push_back(static_cast<unsigned char>(i * 7 + script_size));
        return Coin{CTxOut{value, script}, static_cast<int>(height), coinbase};
    }};
    const auto same{[](const Coin& a, const Coin& b) {
        return a.out == b.out && a.nHeight == b.nHeight && a.fCoinBase == b.fCoinBase && a.IsSpent() == b.IsSpent();
    }};
    std::vector<size_t> sizes;
    for (size_t size{0}; size <= 40; ++size) sizes.push_back(size);
    sizes.push_back(255);
    sizes.push_back(256);
    sizes.push_back(10000);
    for (const size_t size : sizes) {
        for (const bool coinbase : {false, true}) {
            const Coin coin{make_coin(size, 50 * COIN + static_cast<CAmount>(size), (1u << 31) - 1 - static_cast<uint32_t>(size), coinbase)};
            const StoredCoin stored{coin};
            BOOST_CHECK(same(stored.ToCoin(), coin));
            BOOST_CHECK(!stored.IsSpent());
            BOOST_CHECK_EQUAL(stored.nHeight, coin.nHeight);
            BOOST_CHECK_EQUAL(stored.fCoinBase, coin.fCoinBase);
            BOOST_CHECK_EQUAL(stored.DynamicMemoryUsage() == 0, size <= StoredCoin::INLINE);
            // Copies and moves.
            StoredCoin copy{stored};
            BOOST_CHECK(same(copy.ToCoin(), coin));
            StoredCoin moved{std::move(copy)};
            BOOST_CHECK(same(moved.ToCoin(), coin));
            BOOST_CHECK(copy.IsSpent()); // NOLINT(bugprone-use-after-move): the moved-from state is a spent coin
            // Assignments over a coin with a script of the other kind, inline over heap and heap over inline.
            StoredCoin other{make_coin(size <= StoredCoin::INLINE ? 100 : 10, 1, 1, false)};
            other = stored;
            BOOST_CHECK(same(other.ToCoin(), coin));
            other = make_coin(size <= StoredCoin::INLINE ? 100 : 10, 2, 2, true);
            BOOST_CHECK(same(other.ToCoin(), make_coin(size <= StoredCoin::INLINE ? 100 : 10, 2, 2, true)));
            other = std::move(moved);
            BOOST_CHECK(same(other.ToCoin(), coin));
            other.Clear();
            BOOST_CHECK(other.IsSpent());
            BOOST_CHECK(same(other.ToCoin(), Coin{}));
            BOOST_CHECK_EQUAL(other.DynamicMemoryUsage(), 0U);
        }
    }
    // A spent coin, and the default.
    BOOST_CHECK(StoredCoin{}.IsSpent());
    BOOST_CHECK(same(StoredCoin{Coin{}}.ToCoin(), Coin{}));
    // Every standard output script is inline.
    for (const size_t standard : {size_t{22}, size_t{23}, size_t{25}, size_t{34}, size_t{35}}) {
        BOOST_CHECK_EQUAL(StoredCoin{make_coin(standard, 1, 1, false)}.DynamicMemoryUsage(), 0U);
    }
}

//! A node's links hold its number, not its address (CoinsStore::NodeAllocator::Number()): with more entries
//! than one region of nodes holds, numbers of several regions are in the chains and on the lists, and every
//! entry is still found, spent and listed where it belongs. About 80 MiB of nodes.
BOOST_AUTO_TEST_CASE(node_numbers_span_regions)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    static constexpr uint32_t COINS{700'000};
    const auto outpoint{[](uint32_t i) { return COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(i % 251)}), i}; }};
    for (uint32_t i{0}; i < COINS; ++i) {
        const int height{static_cast<int>(1 + i / 100'000)};
        if (i % 100'000 == 0) store.SetBestBlock(uint256{static_cast<uint8_t>(height)}, height);
        store.AddCoin(outpoint(i), Coin{CTxOut{CAmount{i + 1}, CScript() << OP_TRUE}, height, false}, false);
    }
    BOOST_CHECK_EQUAL(store.GetCacheSize(), COINS);
    store.SanityCheck();
    // Spends at a later height move entries between the lists of their heights.
    store.SetBestBlock(uint256{100}, 100);
    store.SetChangeHeight(100);
    for (uint32_t i{0}; i < COINS; i += 3) BOOST_REQUIRE(store.SpendCoin(outpoint(i)));
    store.SanityCheck();
    for (uint32_t i{0}; i < COINS; ++i) {
        const Coin& coin{store.AccessCoin(outpoint(i))};
        if (i % 3 == 0) {
            BOOST_REQUIRE(coin.IsSpent());
        } else {
            BOOST_REQUIRE_EQUAL(coin.out.nValue, CAmount{i + 1});
        }
    }
    // Written out and evicted, every node goes back.
    store.Sync();
    store.SanityCheck();
}

BOOST_AUTO_TEST_SUITE_END()
