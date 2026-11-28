// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sync.h>
#include <test/util/coins.h>
#include <test/util/random.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(validation_flush_tests, TestingSetup)

//! Verify that Chainstate::GetCoinsCacheSizeState() switches from OK→LARGE→CRITICAL
//! at the expected utilization thresholds, first with *no* mempool head-room,
//! then with additional mempool head-room.
BOOST_AUTO_TEST_CASE(getcoinscachesizestate)
{
    constexpr uint64_t MAX_COINS_BYTES{8_MiB};
    constexpr uint64_t MAX_MEMPOOL_BYTES{4_MiB};
    constexpr uint64_t MAX_ATTEMPTS{50'000};
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};

    LOCK(::cs_main);
    BOOST_REQUIRE(chainstate.ResizeCoinsCaches(MAX_COINS_BYTES, /*coinsdb_size=*/1_MiB));
    ShardedCoinsView& view{chainstate.CoinsTip()};

    // Sanity: an empty cache should be ≲ 1 chunk (~ 256 KiB).
    BOOST_CHECK_LT(view.DynamicMemoryUsage() / (256 * 1024.0), 1.1);

    // Run the same growth-path twice: first with 0 head-room, then with extra head-room
    for (uint64_t max_mempool_size_bytes : {uint64_t{0}, MAX_MEMPOOL_BYTES}) {
        const int64_t full_cap{int64_t(MAX_COINS_BYTES + max_mempool_size_bytes)};
        const int64_t large_cap{LargeCoinsCacheThreshold(full_cap)};

        // OK → LARGE
        auto state{chainstate.GetCoinsCacheSizeState(MAX_COINS_BYTES, max_mempool_size_bytes)};
        for (uint64_t i{0}; i < MAX_ATTEMPTS && int64_t(view.DynamicMemoryUsage()) <= large_cap; ++i) {
            BOOST_CHECK_EQUAL(state, CoinsCacheSizeState::OK);
            AddTestCoin(m_rng, view);
            state = chainstate.GetCoinsCacheSizeState(MAX_COINS_BYTES, max_mempool_size_bytes);
        }

        // LARGE → CRITICAL
        for (uint64_t i{0}; i < MAX_ATTEMPTS && int64_t(view.DynamicMemoryUsage()) <= full_cap; ++i) {
            BOOST_CHECK_EQUAL(state, CoinsCacheSizeState::LARGE);
            AddTestCoin(m_rng, view);
            state = chainstate.GetCoinsCacheSizeState(MAX_COINS_BYTES, max_mempool_size_bytes);
        }
        BOOST_CHECK_EQUAL(state, CoinsCacheSizeState::CRITICAL);
    }

    // Unused mempool space permits many more coins.
    for (int i{0}; i < 1'000; ++i) {
        AddTestCoin(m_rng, view);
        BOOST_CHECK_EQUAL(chainstate.GetCoinsCacheSizeState(), CoinsCacheSizeState::OK);
    }

    // CRITICAL → OK via Flush
    BOOST_CHECK_EQUAL(chainstate.GetCoinsCacheSizeState(MAX_COINS_BYTES, /*max_mempool_size_bytes=*/0), CoinsCacheSizeState::CRITICAL);
    BOOST_CHECK_EQUAL(chainstate.GetCoinsCacheSizeState(), CoinsCacheSizeState::OK);
    view.SetBestBlock(m_rng.rand256());
    view.Flush();
    BOOST_CHECK_EQUAL(chainstate.GetCoinsCacheSizeState(MAX_COINS_BYTES, /*max_mempool_size_bytes=*/0), CoinsCacheSizeState::OK);
}

BOOST_AUTO_TEST_CASE(coinstip_sync_and_flush_invariants)
{
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
    LOCK(::cs_main);
    ShardedCoinsView& tip{chainstate.CoinsTip()};
    CCoinsViewDB& db{chainstate.CoinsDB()};

    const COutPoint op{Txid::FromUint256(m_rng.rand256()), 0};
    const Coin coin{CTxOut{50'000, CScript() << OP_TRUE}, /*nHeightIn=*/10, /*fCoinBaseIn=*/false};
    const uint256 block_hash{m_rng.rand256()};

    tip.AddCoin(op, Coin{coin}, /*possible_overwrite=*/false);
    tip.SetBestBlock(block_hash);
    BOOST_CHECK(tip.HaveCoinInCache(op));
    BOOST_CHECK(!db.HaveCoin(op));

    // Sync writes dirty coins to CoinsDB while preserving cached entries and not bumping FlushEpoch.
    const uint64_t epoch_before_sync{tip.FlushEpoch()};
    tip.Sync();
    BOOST_CHECK_EQUAL(tip.FlushEpoch(), epoch_before_sync);
    BOOST_CHECK(tip.HaveCoinInCache(op));
    BOOST_CHECK(db.HaveCoin(op));
    BOOST_CHECK(db.GetBestBlock() == block_hash);

    // Flush clears cached entries, increments FlushEpoch, and leaves CoinsDB intact.
    tip.Flush();
    BOOST_CHECK_EQUAL(tip.FlushEpoch(), epoch_before_sync + 1);
    BOOST_CHECK(!tip.HaveCoinInCache(op));
    auto persisted = tip.PeekCoin(op);
    BOOST_REQUIRE(persisted.has_value());
    BOOST_CHECK(persisted->out == coin.out);
    BOOST_CHECK_EQUAL(persisted->nHeight, coin.nHeight);
    tip.SanityCheck();
}

BOOST_AUTO_TEST_SUITE_END()

struct SmallMempoolTestingSetup : public TestingSetup {
    SmallMempoolTestingSetup() : TestingSetup{ChainType::REGTEST, {.extra_args = {"-maxmempool=5"}}} {}
};

BOOST_FIXTURE_TEST_SUITE(validation_flush_partial_tests, SmallMempoolTestingSetup)

BOOST_AUTO_TEST_CASE(partial_eviction_on_cache_pressure)
{
    constexpr uint64_t MAX_COINS_BYTES{4_MiB};
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
    BlockValidationState state{};

    LOCK(::cs_main);
    BOOST_REQUIRE(chainstate.ResizeCoinsCaches(MAX_COINS_BYTES, /*coinsdb_size=*/1_MiB));
    ShardedCoinsView& tip{chainstate.CoinsTip()};
    CCoinsViewDB& db{chainstate.CoinsDB()};

    const int64_t total_space{chainstate.GetCoinsCacheTotalSpace(
        MAX_COINS_BYTES, m_node.mempool ? m_node.mempool->m_opts.max_size_bytes : 0)};
    BOOST_REQUIRE_GT(total_space, 0);

    // Fill all 256 shards evenly with coins of monotonically increasing height until CRITICAL.
    std::vector<COutPoint> old_ops;
    std::vector<COutPoint> new_ops;
    uint32_t round{0};
    while (chainstate.GetCoinsCacheSizeState() < CoinsCacheSizeState::CRITICAL) {
        ++round;
        for (uint32_t s = 0; s < ShardedCoinsView::NUM_SHARDS; ++s) {
            uint256 hash = m_rng.rand256();
            hash.data()[0] = static_cast<unsigned char>(s);
            const COutPoint op{Txid::FromUint256(hash), 0};
            const Coin coin{CTxOut{50'000, CScript() << OP_TRUE}, static_cast<int>(round), /*fCoinBaseIn=*/false};
            tip.AddCoin(op, Coin{coin}, /*possible_overwrite=*/false);
            if (round == 1) {
                old_ops.push_back(op);
            } else {
                if (new_ops.size() < ShardedCoinsView::NUM_SHARDS) {
                    new_ops.push_back(op);
                } else {
                    new_ops[s] = op;
                }
            }
        }
    }
    BOOST_REQUIRE_GT(round, 10U);
    const uint256 best_block{chainstate.m_chain.Tip()->GetBlockHash()};
    tip.SetBestBlock(best_block);
    const size_t usage_before{tip.DynamicMemoryUsage()};
    const size_t count_before{tip.GetCacheSize()};
    BOOST_REQUIRE_GT(usage_before, static_cast<size_t>(total_space));

    // Cache-pressure flush persists everything to CoinsDB and retains the newest ~50% of budget.
    BOOST_REQUIRE(chainstate.FlushStateToDisk(state, FlushStateMode::IF_NEEDED));
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK_EQUAL(tip.GetDirtyCount(), 0U);
    BOOST_CHECK_EQUAL(chainstate.GetCoinsCacheSizeState(), CoinsCacheSizeState::OK);
    BOOST_CHECK_LE(tip.DynamicMemoryUsage(), static_cast<size_t>(total_space) / 2);
    BOOST_CHECK_GT(tip.GetCacheSize(), 0U);
    BOOST_CHECK_LT(tip.GetCacheSize(), count_before);

    // Oldest coins should be evicted to DB; newest coins should remain hot in cache.
    for (const COutPoint& op : old_ops) {
        BOOST_CHECK(db.HaveCoin(op));
        BOOST_CHECK(!tip.HaveCoinInCache(op));
    }
    for (const COutPoint& op : new_ops) {
        BOOST_CHECK(db.HaveCoin(op));
        BOOST_CHECK(tip.HaveCoinInCache(op));
    }
    tip.SanityCheck();

    // Explicit FORCE_FLUSH must still wipe 100% of the cache.
    BOOST_REQUIRE(chainstate.FlushStateToDisk(state, FlushStateMode::FORCE_FLUSH));
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK_EQUAL(tip.GetCacheSize(), 0U);
    BOOST_CHECK_EQUAL(tip.DynamicMemoryUsage(), 0U);
    tip.SanityCheck();
}

BOOST_AUTO_TEST_SUITE_END()
