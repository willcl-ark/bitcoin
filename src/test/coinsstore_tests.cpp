// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <block_apply_view.h>
#include <coinsstore.h>
#include <crypto/common.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <util/threadpool.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
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
    std::map<COutPoint, Coin> coins;
    uint256 best_block;
    std::vector<std::vector<SortedCoinWrite>> sorted_chunks;

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
            sorted_chunks.emplace_back(chunk.begin(), chunk.end());
            for (const auto& [outpoint, maybe_coin] : chunk) {
                if (!maybe_coin || maybe_coin->IsSpent()) {
                    coins.erase(outpoint);
                } else {
                    coins[outpoint] = *maybe_coin;
                }
            }
        });
        best_block = block_hash;
    }
    size_t EstimateSize() const override { return coins.size(); }
};

class SimpleWindowSource final : public WindowCoinSource
{
public:
    std::map<COutPoint, std::pair<int, Coin>> entries;

    bool FindCoin(const COutPoint& outpoint, int viewer_height, std::optional<Coin>& coin) const override
    {
        coin.reset();
        const auto it{entries.find(outpoint)};
        if (it == entries.end() || it->second.first >= viewer_height) return false;
        coin = it->second.second;
        return true;
    }
    void FindCoins(std::vector<COutPoint>& outpoints, int viewer_height, const std::function<void(const COutPoint&, Coin&&)>& found) const override
    {
        std::vector<COutPoint> remaining;
        for (const COutPoint& outpoint : outpoints) {
            std::optional<Coin> coin;
            if (FindCoin(outpoint, viewer_height, coin)) {
                if (coin) found(outpoint, std::move(*coin));
            } else {
                remaining.push_back(outpoint);
            }
        }
        outpoints = std::move(remaining);
    }
    void NamedIds(std::span<const CTransactionRef> txs, int viewer_height, std::vector<uint8_t>& named) const override
    {
        named.assign(txs.size(), 0);
        for (size_t i{0}; i < txs.size(); ++i) {
            for (const auto& [outpoint, entry] : entries) {
                if (outpoint.hash == txs[i]->GetHash() && entry.first < viewer_height) {
                    named[i] = 1;
                    break;
                }
            }
        }
    }
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
    for (int i{0}; i < 300; ++i) {
        keys.emplace_back(Txid::FromUint256(rng.rand256()), static_cast<uint32_t>(rng.randrange(4)));
    }

    for (int step{0}; step < 500; ++step) {
        const COutPoint& op{keys[rng.randrange(keys.size())]};
        const uint32_t action{static_cast<uint32_t>(rng.randrange(5))};
        if (action == 0) {
            Coin c{MakeCoin(100 + step, static_cast<uint32_t>(step + 1), /*coinbase=*/false, 25 + (step % 40))};
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
                    const uint64_t pa{ReadBE64(chunk[i - 1].first.hash.ToUint256().data())};
                    const uint64_t pb{ReadBE64(chunk[i].first.hash.ToUint256().data())};
                    BOOST_CHECK(pa > pb || (pa == pb && chunk[i].first < chunk[i - 1].first));
                }
                if (prev_prefix) {
                    const uint64_t first_prefix{ReadBE64(chunk.front().first.hash.ToUint256().data())};
                    BOOST_CHECK_LE(first_prefix, *prev_prefix);
                }
                prev_prefix = ReadBE64(chunk.back().first.hash.ToUint256().data());
            }
        }
    }

    const uint256 final_hash{rng.rand256()};
    store.SetBestBlock(final_hash);
    ref.SetBestBlock(final_hash);
    store.Flush();
    ref.Flush();
    store.SanityCheck();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    BOOST_CHECK(MapsEqual(base_store.coins, base_ref.coins));
}

BOOST_AUTO_TEST_CASE(apply_confirmed_conflicts_overwrites_and_misses)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    RecordingView base;
    const COutPoint op_base{Txid::FromUint256(rng.rand256()), 0};
    const COutPoint op_mem{Txid::FromUint256(rng.rand256()), 0};
    const COutPoint op_create{Txid::FromUint256(rng.rand256()), 0};
    base.coins[op_base] = MakeCoin(500, 10, /*coinbase=*/false);
    base.best_block = rng.rand256();

    CoinsStore store(&base, /*shards=*/8, /*deterministic=*/true);
    store.AddCoin(op_mem, MakeCoin(1000, 20, /*coinbase=*/true), /*possible_overwrite=*/false);

    // Spend missing-from-cache base coin with wrong known_coin height must report conflict.
    {
        Coin wrong_known;
        wrong_known.nHeight = 99;
        wrong_known.fCoinBase = false;
        CoinsStore::ConfirmedReport report;
        std::vector<CoinsStore::Change> changes{
            {.outpoint = &op_base, .coin = &wrong_known, .known_coin = true},
        };
        BOOST_CHECK_EQUAL(store.ApplyConfirmed(std::move(changes), &report), 1U);
        BOOST_REQUIRE_EQUAL(report.conflicted.size(), 1U);
        BOOST_CHECK_EQUAL(report.conflicted[0], &op_base);
        BOOST_CHECK(store.HaveCoin(op_base));
    }

    // Valid spend of base coin + valid create + disallowed overwrite of op_mem.
    {
        store.Uncache(op_base);
        BOOST_CHECK(!store.HaveCoinInCache(op_base));

        Coin spend_base;
        spend_base.nHeight = 10;
        spend_base.fCoinBase = false;
        Coin new_coin{MakeCoin(700, 21)};
        Coin bad_overwrite{MakeCoin(800, 21)};

        CoinsStore::ConfirmedReport report;
        std::vector<CoinsStore::Change> changes{
            {.outpoint = &op_base, .coin = &spend_base, .known_coin = true},
            {.outpoint = &op_create, .coin = &new_coin},
            {.outpoint = &op_mem, .coin = &bad_overwrite, .possible_overwrite = false},
        };
        BOOST_CHECK_EQUAL(store.ApplyConfirmed(std::move(changes), &report), 1U);
        BOOST_REQUIRE_EQUAL(report.conflicted.size(), 1U);
        BOOST_CHECK_EQUAL(report.conflicted[0], &op_mem);
        BOOST_CHECK(!store.HaveCoin(op_base));
        BOOST_CHECK(store.HaveCoin(op_create));
        BOOST_CHECK_EQUAL(store.GetCoin(op_mem)->out.nValue, 1000);
    }

    // Allowed overwrite records previous coin in report.overwritten.
    {
        Coin good_overwrite{MakeCoin(900, 22, /*coinbase=*/true)};
        CoinsStore::ConfirmedReport report;
        std::vector<CoinsStore::Change> changes{
            {.outpoint = &op_mem, .coin = &good_overwrite, .possible_overwrite = true},
        };
        BOOST_CHECK_EQUAL(store.ApplyConfirmed(std::move(changes), &report), 0U);
        BOOST_REQUIRE_EQUAL(report.overwritten.size(), 1U);
        BOOST_CHECK_EQUAL(report.overwritten[0].second.out.nValue, 1000);
        BOOST_CHECK_EQUAL(store.GetCoin(op_mem)->out.nValue, 900);
    }

    store.SanityCheck();
}

BOOST_AUTO_TEST_CASE(block_apply_view_overlay_prefetch_and_commit_alone)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    RecordingView base;
    const COutPoint op_db{Txid::FromUint256(rng.rand256()), 0};
    const COutPoint op_win{Txid::FromUint256(rng.rand256()), 0};
    base.coins[op_db] = MakeCoin(2000, 5);
    base.best_block = rng.rand256();

    CoinsStore store(&base, /*shards=*/8, /*deterministic=*/true);
    auto pool{util::MakeNotNullShared<ThreadPool>("test_apply")};
    BlockApplyView view(store, pool, /*deterministic=*/true);

    SimpleWindowSource window;
    window.entries.emplace(op_win, std::make_pair(6, MakeCoin(3000, 6)));

    // Build a block spending op_db and op_win.
    CBlock block;
    CMutableTransaction cb;
    cb.vout.emplace_back(5000, MakeScript(25));
    block.vtx.push_back(MakeTransactionRef(cb));
    CMutableTransaction tx;
    tx.vin.emplace_back(op_db);
    tx.vin.emplace_back(op_win);
    tx.vout.emplace_back(4500, MakeScript(25));
    block.vtx.push_back(MakeTransactionRef(tx));

    view.SetHeight(7);
    view.SetOverlay(&window, 7);
    view.SetKeepBaseReads(true);
    view.Prefetch(block);

    BOOST_CHECK(view.HaveCoinInCache(op_win));
    BOOST_CHECK_EQUAL(view.AccessCoin(op_win).out.nValue, 3000);
    BOOST_CHECK_EQUAL(view.AccessCoin(op_db).out.nValue, 2000);
    // KeepBaseReads populated the store cleanly for op_db.
    BOOST_CHECK(store.HaveCoinInCache(op_db));
    view.Release();

    // Test CommitAlone on a single-input block spending op_db.
    CBlock alone_block;
    alone_block.vtx.push_back(MakeTransactionRef(cb));
    CMutableTransaction tx2;
    tx2.vin.emplace_back(op_db);
    tx2.vout.emplace_back(1900, MakeScript(25));
    alone_block.vtx.push_back(MakeTransactionRef(tx2));

    store.Uncache(op_db);
    BOOST_CHECK(!store.HaveCoinInCache(op_db));

    view.SetHeight(8);
    CBlockUndo undo;
    undo.vtxundo.emplace_back();
    Coin spent;
    BOOST_REQUIRE(view.SpendCoin(op_db, &spent));
    undo.vtxundo[0].vprevout.push_back(spent);
    AddCoins(view, *alone_block.vtx[0], 8);
    AddCoins(view, *alone_block.vtx[1], 8);
    const uint256 new_best{rng.rand256()};
    view.SetBestBlock(new_best);

    view.CommitAlone(alone_block, undo);
    view.ConfirmCommitted();

    BOOST_CHECK(!store.HaveCoin(op_db));
    BOOST_CHECK(store.HaveCoin(COutPoint{alone_block.vtx[1]->GetHash(), 0}));
    BOOST_CHECK(store.GetBestBlock() == new_best);
    store.SanityCheck();
}

BOOST_AUTO_TEST_SUITE_END()
