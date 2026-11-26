// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinsstore.h>
#include <crypto/common.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <util/threadpool.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace {

CScript MakeScript(size_t script_len)
{
    std::vector<unsigned char> bytes(script_len, static_cast<unsigned char>(OP_TRUE));
    return CScript(bytes.begin(), bytes.end());
}

Coin MakeCoin(CAmount amount, int height, bool coinbase = false, size_t script_len = 25)
{
    return Coin{CTxOut{amount, MakeScript(script_len)}, height, coinbase};
}

bool CoinsEqual(const Coin& a, const Coin& b)
{
    return a.out == b.out && a.nHeight == b.nHeight && a.fCoinBase == b.fCoinBase;
}

bool OptCoinsEqual(const std::optional<Coin>& a, const std::optional<Coin>& b)
{
    if (a.has_value() != b.has_value()) return false;
    return !a || CoinsEqual(*a, *b);
}

bool MapsEqual(const std::map<COutPoint, Coin>& a, const std::map<COutPoint, Coin>& b)
{
    if (a.size() != b.size()) return false;
    auto ita{a.begin()};
    auto itb{b.begin()};
    for (; ita != a.end(); ++ita, ++itb) {
        if (ita->first != itb->first || !CoinsEqual(ita->second, itb->second)) return false;
    }
    return true;
}

class RecordingView final : public CCoinsView
{
public:
    struct RecordedWrite {
        COutPoint outpoint;
        std::optional<Coin> coin;
    };

    std::map<COutPoint, Coin> coins;
    uint256 best_block;
    std::vector<std::vector<RecordedWrite>> sorted_chunks;

    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override
    {
        if (const auto it{coins.find(outpoint)}; it != coins.end() && !it->second.IsSpent()) {
            return it->second;
        }
        return std::nullopt;
    }
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override { return GetCoin(outpoint); }
    bool HaveCoin(const COutPoint& outpoint) const override { return GetCoin(outpoint).has_value(); }
    uint256 GetBestBlock() const override { return best_block; }
    std::vector<uint256> GetHeadBlocks() const override { return {}; }
    void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) override
    {
        for (auto it{cursor.Begin()}; it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) {
            if (!it->second.IsDirty()) continue;
            if (it->second.coin.IsSpent()) {
                coins.erase(it->first);
            } else {
                coins[it->first] = it->second.coin;
            }
        }
        best_block = block_hash;
    }
    void BatchWriteSorted(const SortedBatchProducer& produce, const uint256& block_hash, size_t, size_t) override
    {
        sorted_chunks.clear();
        produce([&](std::span<const SortedCoinWrite> chunk) {
            if (chunk.empty()) return;
            auto& recorded{sorted_chunks.emplace_back()};
            recorded.reserve(chunk.size());
            for (const SortedCoinWrite& w : chunk) {
                if (!w.out || w.out->IsNull()) {
                    recorded.push_back({*w.outpoint, std::nullopt});
                    coins.erase(*w.outpoint);
                } else {
                    Coin c{*w.out, static_cast<int>(w.code >> 1), (w.code & 1) != 0};
                    recorded.push_back({*w.outpoint, c});
                    coins[*w.outpoint] = std::move(c);
                }
            }
        });
        best_block = block_hash;
    }
    size_t EstimateSize() const override { return coins.size(); }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(coinsstore_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(matches_coinsviewcache_and_sorted_flush)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    RecordingView base_store;
    RecordingView base_ref;
    CoinsStore store(&base_store, /*shards=*/16, /*deterministic=*/true);
    CCoinsViewCache ref(&base_ref, /*deterministic=*/true);

    std::vector<COutPoint> keys;
    for (int i{0}; i < 400; ++i) {
        keys.emplace_back(Txid::FromUint256(rng.rand256()), static_cast<uint32_t>(rng.randrange(4)));
    }

    for (int step{0}; step < 1200; ++step) {
        const COutPoint& op{keys[rng.randrange(keys.size())]};
        const uint32_t action{static_cast<uint32_t>(rng.randrange(5))};
        if (action == 0) {
            Coin c{MakeCoin(100 + step, static_cast<uint32_t>(step + 1), /*coinbase=*/(step % 17 == 0), 25 + (step % 50))};
            Coin c_ref{c};
            if (!ref.HaveCoin(op)) {
                store.AddCoin(op, std::move(c), /*possible_overwrite=*/false);
                ref.AddCoin(op, std::move(c_ref), /*possible_overwrite=*/false);
            }
        } else if (action == 1) {
            Coin spent_store, spent_ref;
            const bool ok_store{store.SpendCoin(op, &spent_store)};
            const bool ok_ref{ref.SpendCoin(op, &spent_ref)};
            BOOST_CHECK_EQUAL(ok_store, ok_ref);
            if (ok_store) BOOST_CHECK(CoinsEqual(spent_store, spent_ref));
        } else if (action == 2) {
            BOOST_CHECK(OptCoinsEqual(store.GetCoin(op), ref.GetCoin(op)));
            BOOST_CHECK_EQUAL(store.HaveCoinInCache(op), ref.HaveCoinInCache(op));
        } else if (action == 3) {
            store.Uncache(op);
            ref.Uncache(op);
        } else if (step % 100 == 0) {
            const uint256 h{rng.rand256()};
            store.SetBestBlock(h);
            ref.SetBestBlock(h);
            if (step % 200 == 0) {
                store.Sync();
                ref.Sync();
            } else {
                store.Flush();
                ref.Flush();
            }
            store.SanityCheck();
            BOOST_CHECK(MapsEqual(base_store.coins, base_ref.coins));
            BOOST_CHECK(base_store.best_block == base_ref.best_block);
            // Verify chunks are sorted descending and strictly disjoint across shards.
            std::optional<uint64_t> prev_prefix;
            for (const auto& chunk : base_store.sorted_chunks) {
                for (size_t i{1}; i < chunk.size(); ++i) {
                    const uint64_t pa{ReadBE64(chunk[i - 1].outpoint.hash.ToUint256().data())};
                    const uint64_t pb{ReadBE64(chunk[i].outpoint.hash.ToUint256().data())};
                    BOOST_CHECK(pa > pb || (pa == pb && chunk[i].outpoint < chunk[i - 1].outpoint));
                }
                if (prev_prefix) {
                    const uint64_t first_prefix{ReadBE64(chunk.front().outpoint.hash.ToUint256().data())};
                    BOOST_CHECK_LE(first_prefix, *prev_prefix);
                }
                prev_prefix = ReadBE64(chunk.back().outpoint.hash.ToUint256().data());
            }
        }
    }

    const uint256 final_hash{rng.rand256()};
    store.SetBestBlock(final_hash);
    ref.SetBestBlock(final_hash);
    store.Flush();
    ref.Flush();
    store.SanityCheck();
    BOOST_CHECK(MapsEqual(base_store.coins, base_ref.coins));
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
    BOOST_CHECK_EQUAL(store.DynamicMemoryUsage(), 0U);
}

BOOST_AUTO_TEST_CASE(batch_write_and_sync_tombstone_shrinking)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    RecordingView base;
    CoinsStore store(&base, /*shards=*/8, /*deterministic=*/true);

    // Populate across multiple live chunks per shard (> 256 entries per shard).
    std::vector<COutPoint> keys;
    keys.reserve(3000);
    for (int i{0}; i < 3000; ++i) {
        keys.emplace_back(Txid::FromUint256(rng.rand256()), 0);
        store.AddCoin(keys.back(), MakeCoin(1000 + i, 10 + (i % 50), false, 34), /*possible_overwrite=*/false);
    }
    store.SetBestBlock(rng.rand256());
    store.Sync();
    store.SanityCheck();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 3000U);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);

    // Spend a small subset via CCoinsViewCache::Flush (tests shard-batched BatchWrite)
    // and verify O(dirty) Sync removes tombstones and keeps all remaining clean coins intact.
    {
        CCoinsViewCache overlay{&store, /*deterministic=*/true};
        for (size_t i{0}; i < 20; ++i) {
            BOOST_CHECK(overlay.SpendCoin(keys[i]));
        }
        // Re-add one spent coin (tests tombstone -> live non-FRESH transition).
        overlay.AddCoin(keys[0], MakeCoin(9999, 99, false, 25), /*possible_overwrite=*/false);
        // Add a brand-new coin via overlay.
        const COutPoint extra{Txid::FromUint256(rng.rand256()), 1};
        overlay.AddCoin(extra, MakeCoin(777, 100, false, 60), /*possible_overwrite=*/false);
        overlay.SetBestBlock(rng.rand256());
        overlay.Flush(/*reallocate_cache=*/false);
        store.SanityCheck();

        // Spend keys[1] is a tombstone: FetchInto must report FETCH_SPENT so callers skip base DB.
        std::vector<COutPoint> probe{keys[0], keys[1], extra, COutPoint{Txid::FromUint256(rng.rand256()), 0}};
        std::vector<uint32_t> which{0, 1, 2, 3};
        std::vector<Coin> out_coins(4);
        std::vector<uint8_t> out_state;
        store.FetchInto(probe, which, out_coins, out_state);
        BOOST_CHECK_EQUAL(out_state[0], CoinsStore::FETCH_FOUND);
        BOOST_CHECK_EQUAL(out_coins[0].out.nValue, 9999);
        BOOST_CHECK_EQUAL(out_state[1], CoinsStore::FETCH_SPENT);
        BOOST_CHECK_EQUAL(out_state[2], CoinsStore::FETCH_FOUND);
        BOOST_CHECK_EQUAL(out_coins[2].out.nValue, 777);
        BOOST_CHECK_EQUAL(out_state[3], CoinsStore::FETCH_ABSENT);

        // MayHave must report 0 for spent tombstone (keys[1]) even though base still holds it!
        std::vector<uint8_t> may;
        store.MayHave(probe, may);
        BOOST_CHECK_EQUAL(may[0], 1);
        BOOST_CHECK_EQUAL(may[1], 0);
        BOOST_CHECK_EQUAL(may[2], 1);
        BOOST_CHECK_EQUAL(may[3], 0);
    }

    const size_t bytes_with_tombs{store.DynamicMemoryUsage()};
    store.Sync();
    store.SanityCheck();
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 3000U - 19U + 1U);
    BOOST_CHECK_LE(store.DynamicMemoryUsage(), bytes_with_tombs);
    BOOST_CHECK(!base.HaveCoin(keys[1]));
    BOOST_CHECK(base.HaveCoin(keys[0]));
    BOOST_CHECK_EQUAL(base.GetCoin(keys[0])->out.nValue, 9999);

    // Also verify CCoinsViewCache::Sync() (non-erasing cursor) above CoinsStore works cleanly.
    {
        CCoinsViewCache overlay{&store, /*deterministic=*/true};
        BOOST_CHECK(overlay.SpendCoin(keys[25]));
        overlay.AddCoin(COutPoint{Txid::FromUint256(rng.rand256()), 2}, MakeCoin(555, 101), false);
        overlay.SetBestBlock(rng.rand256());
        overlay.Sync();
        store.SanityCheck();
        BOOST_CHECK(!store.HaveCoin(keys[25]));
    }
}

BOOST_AUTO_TEST_CASE(parallel_flush_matches_serial_and_leveldb)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    DBParams db_params{
        .path = {},
        .cache_bytes = 1 << 20,
        .memory_only = true,
    };
    CCoinsViewDB db(db_params, CoinsViewOptions{});
    CoinsStore store(&db, /*shards=*/32, /*deterministic=*/true);

    // Exceed PARALLEL_FLUSH_MIN_DIRTY (4096) to exercise double-buffered parallel shard sorting.
    std::map<COutPoint, Coin> expected;
    std::vector<COutPoint> ops;
    ops.reserve(6000);
    for (int i{0}; i < 6000; ++i) {
        const COutPoint op{Txid::FromUint256(rng.rand256()), static_cast<uint32_t>(i & 3)};
        Coin c{MakeCoin(1000 + i, 1 + (i % 100000), (i % 100 == 0), 25 + (i % 40))};
        expected[op] = c;
        store.AddCoin(op, std::move(c), /*possible_overwrite=*/false);
        ops.push_back(op);
    }
    const uint256 block1{rng.rand256()};
    store.SetBestBlock(block1);

    ThreadPool pool{"flush_test"};
    pool.Start(4);
    store.Flush(/*reallocate_cache=*/true, &pool);
    store.SanityCheck();
    BOOST_CHECK(db.GetBestBlock() == block1);
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);

    // Spend 2000 from base and add 3000 new coins, then Sync in parallel.
    for (size_t i{0}; i < 2000; ++i) {
        BOOST_CHECK(store.SpendCoin(ops[i]));
        expected.erase(ops[i]);
    }
    for (int i{0}; i < 3000; ++i) {
        const COutPoint op{Txid::FromUint256(rng.rand256()), 0};
        Coin c{MakeCoin(5000 + i, 500 + i, false, 34)};
        expected[op] = c;
        store.AddCoin(op, std::move(c), false);
    }
    const uint256 block2{rng.rand256()};
    store.SetBestBlock(block2);
    store.Sync(&pool);
    pool.Stop();
    store.SanityCheck();
    BOOST_CHECK(db.GetBestBlock() == block2);

    // Verify full LevelDB contents match expected bit-for-bit.
    std::map<COutPoint, Coin> actual;
    auto cursor{db.Cursor()};
    while (cursor->Valid()) {
        COutPoint k;
        Coin v;
        BOOST_REQUIRE(cursor->GetKey(k));
        BOOST_REQUIRE(cursor->GetValue(v));
        actual[k] = std::move(v);
        cursor->Next();
    }
    BOOST_CHECK(MapsEqual(actual, expected));
}

BOOST_AUTO_TEST_CASE(two_phase_check_and_apply_shard)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    RecordingView base;
    CoinsStore store(&base, /*shards=*/1, /*deterministic=*/true);

    const COutPoint base_coin_op{Txid::FromUint256(rng.rand256()), 0};
    const COutPoint cached_coin_op{Txid::FromUint256(rng.rand256()), 0};
    const COutPoint created_op{Txid::FromUint256(rng.rand256()), 0};
    const COutPoint dup_cb_op{Txid::FromUint256(rng.rand256()), 0};

    base.coins[base_coin_op] = MakeCoin(500, 10, false);
    store.AddCoin(cached_coin_op, MakeCoin(600, 20, false), false);
    store.AddCoin(dup_cb_op, MakeCoin(700, 5, true), true);
    store.SetBestBlock(rng.rand256());

    const CTxOut new_out{300, MakeScript(34)};
    const CTxOut cb_overwrite_out{750, MakeScript(25)};
    constexpr uint32_t NO_CONFLICT{std::numeric_limits<uint32_t>::max()};

    // Block 0: spends base_coin_op (untrusted base check) and creates created_op.
    // Block 1: spends created_op (intra-batch create -> spend!) and overwrites dup_cb_op (BIP30).
    // Block 2: double-spends base_coin_op -> conflict at block 2!
    std::vector<CoinsStore::ShardOp> ops{
        {base_coin_op, nullptr, (10U << 1), 0, store.HashOutpoint(base_coin_op), false, /*trust_from_base=*/false},
        {created_op, &new_out, (21U << 1), 0, store.HashOutpoint(created_op), false, false},
        {created_op, nullptr, (21U << 1), 1, store.HashOutpoint(created_op), false, false},
        {dup_cb_op, &cb_overwrite_out, (22U << 1) | 1U, 1, store.HashOutpoint(dup_cb_op), /*possible_overwrite=*/true, false},
        {base_coin_op, nullptr, (10U << 1), 2, store.HashOutpoint(base_coin_op), false, false},
    };

    const uint32_t conflict{store.CheckShard(0, ops, NO_CONFLICT)};
    BOOST_CHECK_EQUAL(conflict, 2U);
    // Phase 1 must leave CoinsStore completely untouched!
    BOOST_CHECK(store.HaveCoin(base_coin_op));
    BOOST_CHECK(!store.HaveCoin(created_op));
    BOOST_CHECK_EQUAL(store.GetCoin(dup_cb_op)->out.nValue, 700);

    // Phase 2 applies only blocks 0..1 unconditionally.
    store.ApplyShardUnconditional(0, ops, /*valid_blocks=*/conflict);
    store.SanityCheck();
    BOOST_CHECK(!store.HaveCoin(base_coin_op));
    BOOST_CHECK(!store.HaveCoin(created_op));
    BOOST_CHECK_EQUAL(store.GetCoin(dup_cb_op)->out.nValue, 750);
    BOOST_CHECK_EQUAL(store.GetCoin(dup_cb_op)->nHeight, 22U);
    BOOST_CHECK(store.GetCoin(dup_cb_op)->IsCoinBase());

    // Wrong metadata on spend must report conflict in Phase 1 without mutating store.
    std::vector<CoinsStore::ShardOp> bad_meta{
        {cached_coin_op, nullptr, (99U << 1), 0, store.HashOutpoint(cached_coin_op), false, false},
    };
    BOOST_CHECK_EQUAL(store.CheckShard(0, bad_meta, NO_CONFLICT), 0U);
    BOOST_CHECK(store.HaveCoin(cached_coin_op));

    // Disallowed overwrite must report conflict in Phase 1.
    std::vector<CoinsStore::ShardOp> bad_overwrite{
        {cached_coin_op, &new_out, (25U << 1), 0, store.HashOutpoint(cached_coin_op), /*possible_overwrite=*/false, false},
    };
    BOOST_CHECK_EQUAL(store.CheckShard(0, bad_overwrite, NO_CONFLICT), 0U);
    BOOST_CHECK_EQUAL(store.GetCoin(cached_coin_op)->out.nValue, 600);
}

BOOST_AUTO_TEST_CASE(concurrent_reads_during_lookup_and_flush)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    RecordingView base;
    std::vector<COutPoint> keys;
    for (int i{0}; i < 200; ++i) {
        const COutPoint op{Txid::FromUint256(rng.rand256()), 0};
        base.coins[op] = MakeCoin(1000 + i, 50, false);
        keys.push_back(op);
    }

    CoinsStore store(&base, /*shards=*/16, /*deterministic=*/true);
    std::atomic<bool> all_found{true};
    std::vector<std::thread> threads;
    for (int t{0}; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (size_t iter{0}; iter < keys.size(); ++iter) {
                const COutPoint& op{keys[(iter + t * 37) % keys.size()]};
                if (!store.GetCoin(op).has_value()) {
                    all_found.store(false, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    BOOST_CHECK(all_found.load());
    store.SanityCheck();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 200U);
}

BOOST_AUTO_TEST_SUITE_END()
