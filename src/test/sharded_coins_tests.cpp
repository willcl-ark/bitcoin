// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <coins_sharded.h>
#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/threadpool.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

namespace {

struct LevelDBOutPointLess {
    bool operator()(const COutPoint& a, const COutPoint& b) const noexcept
    {
        return CompareOutPointsLevelDBOrder(a, b);
    }
};

/** Recording CCoinsView backend capturing emitted BatchWriteEntries order and contents. */
class RecordingCoinsView final : public CCoinsView
{
public:
    std::map<COutPoint, Coin, LevelDBOutPointLess> db;
    uint256 best_block;
    std::vector<std::pair<COutPoint, bool>> last_emitted; // (outpoint, is_erase)
    size_t write_calls{0};

    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override
    {
        auto it = db.find(outpoint);
        if (it == db.end()) return std::nullopt;
        return it->second;
    }

    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override
    {
        return GetCoin(outpoint);
    }

    bool HaveCoin(const COutPoint& outpoint) const override
    {
        return db.contains(outpoint);
    }

    uint256 GetBestBlock() const override { return best_block; }
    std::vector<uint256> GetHeadBlocks() const override { return {}; }
    size_t EstimateSize() const override { return db.size(); }

    void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) override
    {
        BatchWriteEntries(cursor.GetDirtyCount(), cursor.GetTotalCount(), block_hash, [&](CoinsWriteSink& sink) {
            for (auto it = cursor.Begin(); it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) {
                if (!it->second.IsDirty()) continue;
                if (it->second.coin.IsSpent()) {
                    sink.Erase(it->first);
                } else {
                    sink.Write(it->first, it->second.coin);
                }
            }
        });
    }

    void BatchWriteEntries(
        size_t,
        size_t,
        const uint256& block_hash,
        const std::function<void(CoinsWriteSink&)>& emit_entries) override
    {
        ++write_calls;
        last_emitted.clear();
        struct Sink final : CoinsWriteSink {
            RecordingCoinsView& self;
            explicit Sink(RecordingCoinsView& s) : self{s} {}
            void Write(const COutPoint& outpoint, const Coin& coin) override
            {
                self.last_emitted.emplace_back(outpoint, false);
                self.db[outpoint] = coin;
            }
            void Erase(const COutPoint& outpoint) override
            {
                self.last_emitted.emplace_back(outpoint, true);
                self.db.erase(outpoint);
            }
        } sink{*this};
        emit_entries(sink);
        best_block = block_hash;
    }
};

Coin MakeTestCoin(CAmount value, int height, bool coinbase = false, size_t script_len = 25)
{
    CTxOut out;
    out.nValue = value;
    out.scriptPubKey.assign(script_len, OP_TRUE);
    return Coin{std::move(out), height, coinbase};
}

COutPoint MakeShardedOutPoint(uint8_t shard_byte, uint64_t id, uint32_t vout)
{
    uint256 h;
    h.SetNull();
    h.data()[0] = shard_byte;
    for (int i = 0; i < 8; ++i) {
        h.data()[1 + i] = static_cast<uint8_t>((id >> (i * 8)) & 0xFF);
    }
    return COutPoint{Txid::FromUint256(h), vout};
}

std::array<std::vector<ShardedCoinsView::BatchCommitOp>, ShardedCoinsView::NUM_SHARDS>
ToShardOps(const std::vector<ShardedCoinsView::BatchCommitOp>& ops)
{
    std::array<std::vector<ShardedCoinsView::BatchCommitOp>, ShardedCoinsView::NUM_SHARDS> sharded;
    for (const auto& op : ops) {
        sharded[ShardedCoinsView::ShardIndex(op.outpoint)].push_back(op);
    }
    return sharded;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(sharded_coins_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(sharded_sorted_flush_order)
{
    RecordingCoinsView base;
    // Pre-seed a few DB entries across shards to test tombstone erasures in sorted order too.
    const COutPoint db_spend_1 = MakeShardedOutPoint(0x02, 10, 0);
    const COutPoint db_spend_2 = MakeShardedOutPoint(0x80, 20, 128);
    const COutPoint db_spend_3 = MakeShardedOutPoint(0xFE, 30, 16512);
    base.db[db_spend_1] = MakeTestCoin(100, 1);
    base.db[db_spend_2] = MakeTestCoin(200, 2);
    base.db[db_spend_3] = MakeTestCoin(300, 3);

    ShardedCoinsView cache{&base, /*deterministic=*/true};

    // Add entries across shards and VARINT boundaries (n < 128, n >= 128, n >= 16512).
    const std::array<uint32_t, 5> vouts{0, 1, 127, 128, 16512};
    for (uint32_t i = 0; i < 1000; ++i) {
        const uint8_t shard = static_cast<uint8_t>((i * 73) & 0xFF);
        for (uint32_t v : vouts) {
            cache.AddCoin(MakeShardedOutPoint(shard, i, v), MakeTestCoin(1000 + i + v, 10), /*possible_overwrite=*/false);
        }
    }
    BOOST_CHECK(cache.SpendCoin(db_spend_1));
    BOOST_CHECK(cache.SpendCoin(db_spend_2));
    BOOST_CHECK(cache.SpendCoin(db_spend_3));
    cache.SanityCheck();

    // Flush with a worker pool (>= 4096 dirty entries across >= 4 shards exercises parallel sort).
    ThreadPool pool{"test_flush"};
    pool.Start(4);
    const uint256 tip_hash = m_rng.rand256();
    cache.SetBestBlock(tip_hash);
    cache.Flush(/*reallocate_cache=*/true, &pool);
    pool.Stop();

    BOOST_CHECK(base.best_block == tip_hash);
    BOOST_CHECK_EQUAL(cache.GetCacheSize(), 0U);
    BOOST_CHECK_EQUAL(cache.GetDirtyCount(), 0U);
    BOOST_CHECK_EQUAL(cache.DynamicMemoryUsage(), 0U);
    BOOST_CHECK_EQUAL(base.last_emitted.size(), 5003U);

    // Every emitted key must be strictly ascending in exact LevelDB serialization order.
    for (size_t i = 1; i < base.last_emitted.size(); ++i) {
        BOOST_CHECK(CompareOutPointsLevelDBOrder(base.last_emitted[i - 1].first, base.last_emitted[i].first));
    }
    BOOST_CHECK(!base.HaveCoin(db_spend_1));
    BOOST_CHECK(!base.HaveCoin(db_spend_2));
    BOOST_CHECK(!base.HaveCoin(db_spend_3));
    cache.SanityCheck();
}

BOOST_AUTO_TEST_CASE(sharded_lookup_statuses_and_batch)
{
    RecordingCoinsView base;
    const COutPoint op_db_only = MakeShardedOutPoint(0x10, 1, 0);
    const COutPoint op_live_clean = MakeShardedOutPoint(0x20, 2, 0);
    const COutPoint op_live_fresh = MakeShardedOutPoint(0x30, 3, 0);
    const COutPoint op_tombstone = MakeShardedOutPoint(0x40, 4, 0);
    const COutPoint op_absent = MakeShardedOutPoint(0x50, 5, 0);

    base.db[op_db_only] = MakeTestCoin(111, 5);
    base.db[op_live_clean] = MakeTestCoin(222, 6);
    base.db[op_tombstone] = MakeTestCoin(444, 7);

    ShardedCoinsView cache{&base, /*deterministic=*/true};

    // PeekCoin must not populate cache.
    BOOST_CHECK(cache.PeekCoin(op_db_only).has_value());
    BOOST_CHECK(!cache.HaveCoinInCache(op_db_only));
    BOOST_CHECK(cache.PeekCacheStatus(op_db_only) == ShardedCoinsView::CacheLookupStatus::ABSENT);

    // GetCoin populates cache as clean LIVE.
    auto fetched = cache.GetCoin(op_live_clean);
    BOOST_REQUIRE(fetched.has_value());
    BOOST_CHECK_EQUAL(fetched->out.nValue, 222);
    BOOST_CHECK(cache.HaveCoinInCache(op_live_clean));
    BOOST_CHECK(cache.PeekCacheStatus(op_live_clean) == ShardedCoinsView::CacheLookupStatus::LIVE);

    // Add fresh coin.
    cache.AddCoin(op_live_fresh, MakeTestCoin(333, 8), /*possible_overwrite=*/false);
    BOOST_CHECK(cache.PeekCacheStatus(op_live_fresh) == ShardedCoinsView::CacheLookupStatus::LIVE);

    // Spend non-fresh DB coin -> compact TOMBSTONE.
    Coin moved;
    BOOST_CHECK(cache.SpendCoin(op_tombstone, &moved));
    BOOST_CHECK_EQUAL(moved.out.nValue, 444);
    BOOST_CHECK(!cache.HaveCoin(op_tombstone));
    BOOST_CHECK(!cache.HaveCoinInCache(op_tombstone));
    BOOST_CHECK(cache.PeekCacheStatus(op_tombstone) == ShardedCoinsView::CacheLookupStatus::TOMBSTONE);

    // Batch lookup must distinguish LIVE, TOMBSTONE, and ABSENT accurately.
    const std::array<COutPoint, 5> ops{op_db_only, op_live_clean, op_live_fresh, op_tombstone, op_absent};
    std::array<ShardedCoinsView::BatchLookupResult, 5> results;
    cache.LookupBatch(ops, results);

    BOOST_CHECK(results[0].status == ShardedCoinsView::CacheLookupStatus::ABSENT);
    BOOST_CHECK(results[1].status == ShardedCoinsView::CacheLookupStatus::LIVE);
    BOOST_CHECK_EQUAL(results[1].coin.out.nValue, 222);
    BOOST_CHECK(results[2].status == ShardedCoinsView::CacheLookupStatus::LIVE);
    BOOST_CHECK_EQUAL(results[2].coin.out.nValue, 333);
    BOOST_CHECK(results[3].status == ShardedCoinsView::CacheLookupStatus::TOMBSTONE);
    BOOST_CHECK(results[4].status == ShardedCoinsView::CacheLookupStatus::ABSENT);

    // Uncache clean live coin increments flush epoch and returns to ABSENT.
    const uint64_t epoch_before = cache.FlushEpoch();
    cache.Uncache(op_live_clean);
    BOOST_CHECK_GT(cache.FlushEpoch(), epoch_before);
    BOOST_CHECK(cache.PeekCacheStatus(op_live_clean) == ShardedCoinsView::CacheLookupStatus::ABSENT);
    cache.SanityCheck();
}

BOOST_AUTO_TEST_CASE(sharded_o_dirty_sync_and_tombstone_cleanup)
{
    RecordingCoinsView base;
    ShardedCoinsView cache{&base, /*deterministic=*/true};

    // Populate 500 clean live coins in base + cache.
    std::vector<COutPoint> clean_ops;
    clean_ops.reserve(500);
    for (uint32_t i = 0; i < 500; ++i) {
        const COutPoint op = MakeShardedOutPoint(static_cast<uint8_t>(i & 0xFF), i, 0);
        base.db[op] = MakeTestCoin(1000 + i, 1);
        clean_ops.push_back(op);
        BOOST_CHECK(cache.GetCoin(op).has_value());
    }
    BOOST_CHECK_EQUAL(cache.GetDirtyCount(), 0U);
    BOOST_CHECK_EQUAL(cache.GetCacheSize(), 500U);

    // Mutate only 5 coins: 3 spends (tombstones) and 2 new coins.
    for (size_t i = 0; i < 3; ++i) {
        BOOST_CHECK(cache.SpendCoin(clean_ops[i]));
    }
    const COutPoint new_op_1 = MakeShardedOutPoint(0xAA, 9991, 0);
    const COutPoint new_op_2 = MakeShardedOutPoint(0xBB, 9992, 0);
    cache.AddCoin(new_op_1, MakeTestCoin(777, 2), /*possible_overwrite=*/false);
    cache.AddCoin(new_op_2, MakeTestCoin(888, 2), /*possible_overwrite=*/false);
    BOOST_CHECK_EQUAL(cache.GetDirtyCount(), 5U);

    const uint64_t epoch_before = cache.FlushEpoch();
    const uint256 sync_hash = m_rng.rand256();
    cache.SetBestBlock(sync_hash);
    cache.Sync();

    // Sync() must emit ONLY the 5 dirty entries, keep 499 clean live coins cached, and erase flushed tombstones.
    BOOST_CHECK_EQUAL(base.last_emitted.size(), 5U);
    BOOST_CHECK(base.best_block == sync_hash);
    BOOST_CHECK_EQUAL(cache.GetDirtyCount(), 0U);
    BOOST_CHECK_EQUAL(cache.GetCacheSize(), 499U);
    BOOST_CHECK_GT(cache.FlushEpoch(), epoch_before);

    for (size_t i = 0; i < 3; ++i) {
        BOOST_CHECK(cache.PeekCacheStatus(clean_ops[i]) == ShardedCoinsView::CacheLookupStatus::ABSENT);
        BOOST_CHECK(!base.HaveCoin(clean_ops[i]));
    }
    for (size_t i = 3; i < 500; ++i) {
        BOOST_CHECK(cache.PeekCacheStatus(clean_ops[i]) == ShardedCoinsView::CacheLookupStatus::LIVE);
    }
    BOOST_CHECK(cache.PeekCacheStatus(new_op_1) == ShardedCoinsView::CacheLookupStatus::LIVE);
    BOOST_CHECK(cache.PeekCacheStatus(new_op_2) == ShardedCoinsView::CacheLookupStatus::LIVE);
    cache.SanityCheck();
}

BOOST_AUTO_TEST_CASE(sharded_commit_batch_and_rollback)
{
    RecordingCoinsView base;
    const COutPoint db_coin = MakeShardedOutPoint(0x11, 1, 0);
    base.db[db_coin] = MakeTestCoin(5000, 1);

    ShardedCoinsView cache{&base, /*deterministic=*/true};
    const uint64_t epoch = cache.FlushEpoch();

    // Happy path: 3 contiguous blocks with inter-block dependency + DB spend.
    const COutPoint coin_b0 = MakeShardedOutPoint(0x22, 10, 0);
    const COutPoint coin_b1 = MakeShardedOutPoint(0x33, 20, 0);
    const COutPoint coin_b2 = MakeShardedOutPoint(0x44, 30, 0);
    const uint256 h0 = m_rng.rand256();
    const uint256 h1 = m_rng.rand256();
    const uint256 h2 = m_rng.rand256();

    std::vector<ShardedCoinsView::BatchCommitBlockMeta> meta{
        {h0, epoch, /*height=*/2, /*enforce_bip30=*/false},
        {h1, epoch, /*height=*/3, /*enforce_bip30=*/false},
        {h2, epoch, /*height=*/4, /*enforce_bip30=*/false},
    };
    std::array<std::vector<ShardedCoinsView::BatchCommitOp>, ShardedCoinsView::NUM_SHARDS> ops;
    ops[ShardedCoinsView::ShardIndex(coin_b0)].push_back({coin_b0, MakeTestCoin(1000, 2), 0, ShardedCoinsView::BatchCommitOp::Kind::CREATE, false, false});
    ops[ShardedCoinsView::ShardIndex(coin_b0)].push_back({coin_b0, Coin{}, 1, ShardedCoinsView::BatchCommitOp::Kind::SPEND, false, false});
    ops[ShardedCoinsView::ShardIndex(coin_b1)].push_back({coin_b1, MakeTestCoin(900, 3), 1, ShardedCoinsView::BatchCommitOp::Kind::CREATE, false, false});
    ops[ShardedCoinsView::ShardIndex(db_coin)].push_back({db_coin, Coin{}, 2, ShardedCoinsView::BatchCommitOp::Kind::SPEND, true, false});
    ops[ShardedCoinsView::ShardIndex(coin_b2)].push_back({coin_b2, MakeTestCoin(4900, 4), 2, ShardedCoinsView::BatchCommitOp::Kind::CREATE, false, false});

    BOOST_CHECK_EQUAL(cache.CommitBlockBatch(meta, ops), 3U);
    BOOST_CHECK(cache.GetBestBlock() == h2);
    BOOST_CHECK(!cache.HaveCoin(coin_b0)); // created & spent within batch -> erased as FRESH
    BOOST_CHECK(cache.PeekCacheStatus(coin_b0) == ShardedCoinsView::CacheLookupStatus::ABSENT);
    BOOST_CHECK(cache.HaveCoin(coin_b1));
    BOOST_CHECK(cache.HaveCoin(coin_b2));
    BOOST_CHECK(cache.PeekCacheStatus(db_coin) == ShardedCoinsView::CacheLookupStatus::TOMBSTONE);
    cache.SanityCheck();

    // Conflict path: 3 blocks where block 0 & 1 are valid, block 2 double-spends coin_b1 AND writes to another shard.
    const COutPoint coin_c0 = MakeShardedOutPoint(0x55, 40, 0);
    const COutPoint coin_c1 = MakeShardedOutPoint(0x66, 50, 0);
    const COutPoint coin_c2_other_shard = MakeShardedOutPoint(0x77, 60, 0);
    const uint256 hc0 = m_rng.rand256();
    const uint256 hc1 = m_rng.rand256();
    const uint256 hc2 = m_rng.rand256();

    std::vector<ShardedCoinsView::BatchCommitBlockMeta> conflict_meta{
        {hc0, cache.FlushEpoch(), /*height=*/5, /*enforce_bip30=*/false},
        {hc1, cache.FlushEpoch(), /*height=*/6, /*enforce_bip30=*/false},
        {hc2, cache.FlushEpoch(), /*height=*/7, /*enforce_bip30=*/false},
    };
    std::array<std::vector<ShardedCoinsView::BatchCommitOp>, ShardedCoinsView::NUM_SHARDS> conflict_ops;
    // Block 0 creates coin_c0
    conflict_ops[ShardedCoinsView::ShardIndex(coin_c0)].push_back({coin_c0, MakeTestCoin(500, 5), 0, ShardedCoinsView::BatchCommitOp::Kind::CREATE, false, false});
    // Block 1 spends coin_b1 (valid) and creates coin_c1
    conflict_ops[ShardedCoinsView::ShardIndex(coin_b1)].push_back({coin_b1, Coin{}, 1, ShardedCoinsView::BatchCommitOp::Kind::SPEND, false, false});
    conflict_ops[ShardedCoinsView::ShardIndex(coin_c1)].push_back({coin_c1, MakeTestCoin(800, 6), 1, ShardedCoinsView::BatchCommitOp::Kind::CREATE, false, false});
    // Block 2 tries to spend coin_b1 AGAIN (conflict!) AND creates coin_c2_other_shard (must be rolled back!)
    conflict_ops[ShardedCoinsView::ShardIndex(coin_b1)].push_back({coin_b1, Coin{}, 2, ShardedCoinsView::BatchCommitOp::Kind::SPEND, false, false});
    conflict_ops[ShardedCoinsView::ShardIndex(coin_c2_other_shard)].push_back({coin_c2_other_shard, MakeTestCoin(700, 7), 2, ShardedCoinsView::BatchCommitOp::Kind::CREATE, false, false});

    BOOST_CHECK_EQUAL(cache.CommitBlockBatch(conflict_meta, conflict_ops), 2U);
    BOOST_CHECK(cache.GetBestBlock() == hc1);
    BOOST_CHECK(cache.HaveCoin(coin_c0));
    BOOST_CHECK(!cache.HaveCoin(coin_b1));
    BOOST_CHECK(cache.HaveCoin(coin_c1));
    BOOST_CHECK(!cache.HaveCoin(coin_c2_other_shard));
    BOOST_CHECK(cache.PeekCacheStatus(coin_c2_other_shard) == ShardedCoinsView::CacheLookupStatus::ABSENT);
    cache.SanityCheck();
}

BOOST_AUTO_TEST_CASE(sharded_memory_accounting)
{
    RecordingCoinsView base;
    ShardedCoinsView cache{&base, /*deterministic=*/true};
    BOOST_CHECK_EQUAL(cache.DynamicMemoryUsage(), 0U);

    // Large scripts (> 36 bytes prevector inline size) in one shard exercise dynamic script byte accounting.
    std::vector<COutPoint> ops;
    for (uint32_t i = 0; i < 200; ++i) {
        const COutPoint op = MakeShardedOutPoint(0x10, i, 0);
        base.db[op] = MakeTestCoin(1000, 1, false, 200);
        ops.push_back(op);
        BOOST_CHECK(cache.GetCoin(op).has_value());
    }
    const size_t mem_with_large_live = cache.DynamicMemoryUsage();
    BOOST_CHECK_GT(mem_with_large_live, 200U * 200U);

    // Spending non-fresh coins releases dynamic script allocations and stores compact tombstones.
    for (const auto& op : ops) {
        BOOST_CHECK(cache.SpendCoin(op));
    }
    const size_t mem_with_tombs = cache.DynamicMemoryUsage();
    BOOST_CHECK_LT(mem_with_tombs, mem_with_large_live);
    cache.SanityCheck();

    // Compare pure tombstone cache vs pure live cache across 1000 coins in one shard:
    // compact tombstones should be significantly smaller than full LiveEntry storage.
    ShardedCoinsView live_only{&base, /*deterministic=*/true};
    ShardedCoinsView tomb_only{&base, /*deterministic=*/true};
    for (uint32_t i = 0; i < 1000; ++i) {
        const COutPoint op = MakeShardedOutPoint(0x20, i, 0);
        base.db[op] = MakeTestCoin(1000 + i, 1, false, 25);
        live_only.AddCoin(op, MakeTestCoin(1000 + i, 1, false, 25), /*possible_overwrite=*/false);
        BOOST_CHECK(tomb_only.SpendCoin(op));
    }
    BOOST_CHECK_LT(tomb_only.DynamicMemoryUsage() * 2, live_only.DynamicMemoryUsage() * 3);
    live_only.SanityCheck();
    tomb_only.SanityCheck();

    cache.SetBestBlock(m_rng.rand256());
    cache.Flush(/*reallocate_cache=*/true);
    BOOST_CHECK_EQUAL(cache.DynamicMemoryUsage(), 0U);
    cache.SanityCheck();
}

BOOST_AUTO_TEST_CASE(sharded_multithreaded_read_stress)
{
    RecordingCoinsView base;
    ShardedCoinsView cache{&base, /*deterministic=*/true};

    constexpr size_t NUM_KEYS = 2048;
    std::vector<COutPoint> keys;
    keys.reserve(NUM_KEYS);
    for (uint32_t i = 0; i < NUM_KEYS; ++i) {
        const COutPoint op = MakeShardedOutPoint(static_cast<uint8_t>(i & 0xFF), i, i % 5);
        keys.push_back(op);
        if (i % 3 == 0) {
            cache.AddCoin(op, MakeTestCoin(100 + i, 5), /*possible_overwrite=*/false);
        } else if (i % 3 == 1) {
            base.db[op] = MakeTestCoin(200 + i, 2);
            BOOST_CHECK(cache.SpendCoin(op)); // tombstone
        }
        // i % 3 == 2 stays ABSENT
    }
    cache.SanityCheck();

    std::atomic<bool> failed{false};
    constexpr int NUM_THREADS = 8;
    std::vector<std::thread> readers;
    readers.reserve(NUM_THREADS);
    for (int t = 0; t < NUM_THREADS; ++t) {
        readers.emplace_back([&, t]() {
            std::vector<ShardedCoinsView::BatchLookupResult> batch_res(NUM_KEYS);
            for (int iter = 0; iter < 25; ++iter) {
                cache.LookupBatch(keys, batch_res);
                for (size_t i = 0; i < NUM_KEYS; ++i) {
                    const auto expected = (i % 3 == 0) ? ShardedCoinsView::CacheLookupStatus::LIVE :
                                          (i % 3 == 1) ? ShardedCoinsView::CacheLookupStatus::TOMBSTONE :
                                                         ShardedCoinsView::CacheLookupStatus::ABSENT;
                    if (batch_res[i].status != expected) {
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                    if (expected == ShardedCoinsView::CacheLookupStatus::LIVE &&
                        batch_res[i].coin.out.nValue != static_cast<CAmount>(100 + i)) {
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
                const size_t probe_idx = static_cast<size_t>((t * 97 + iter * 13) % NUM_KEYS);
                const bool have_cached = cache.HaveCoinInCache(keys[probe_idx]);
                if (have_cached != (probe_idx % 3 == 0)) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (auto& th : readers) th.join();

    BOOST_CHECK(!failed.load());
    cache.SanityCheck();
}

BOOST_AUTO_TEST_CASE(sharded_stored_coin_packing_and_roundtrip)
{
    static_assert(ShardedCoinsView::LIVE_ENTRY_BYTES == 80);
    static_assert(ShardedCoinsView::TOMBSTONE_ENTRY_BYTES == 40);
    static_assert(ShardedCoinsView::INDEX_SLOT_BYTES == 8);

    auto make_script_bytes = [](std::initializer_list<uint8_t> prefix, size_t payload_len, uint8_t seed, std::initializer_list<uint8_t> suffix = {}) {
        std::vector<unsigned char> buf(prefix.begin(), prefix.end());
        for (size_t i = 0; i < payload_len; ++i) {
            buf.push_back(static_cast<unsigned char>((seed * 31 + i * 17) & 0xFF));
        }
        buf.insert(buf.end(), suffix.begin(), suffix.end());
        return CScript(buf.begin(), buf.end());
    };

    struct Case {
        CAmount amount;
        uint32_t height;
        bool coinbase;
        CScript script;
        bool expect_heap;
    };

    std::vector<Case> cases;
    // Empty and inline raw scripts (0..32 bytes: empty, 1 B, P2WPKH 22 B, P2SH 23 B, P2PKH 25 B, 32 B)
    cases.push_back({0, 0, false, CScript{}, false});
    cases.push_back({1, 1, true, make_script_bytes({OP_TRUE}, 0, 1), false});
    cases.push_back({MAX_MONEY, 100, false, make_script_bytes({OP_0, 20}, 20, 2), false});
    cases.push_back({50000, 900000, false, make_script_bytes({OP_HASH160, 20}, 20, 3, {OP_EQUAL}), false});
    cases.push_back({123456789, 12345678, true, make_script_bytes({OP_DUP, OP_HASH160, 20}, 20, 4, {OP_EQUALVERIFY, OP_CHECKSIG}), false});
    cases.push_back({(CAmount{1} << 52) - 2, ShardedCoinsView::MAX_HEIGHT, true, make_script_bytes({OP_TRUE}, 31, 5), false});

    // Compact 32-byte payload standard scripts (zero heap allocation!)
    cases.push_back({1000, 500000, false, make_script_bytes({OP_0, 32}, 32, 6), false});                           // P2WSH (34 B)
    cases.push_back({2000, 709632, false, make_script_bytes({OP_1, 32}, 32, 7), false});                           // P2TR (34 B)
    cases.push_back({3000, 200000, false, make_script_bytes({33, 0x02}, 32, 8, {OP_CHECKSIG}), false});            // P2PK even (35 B)
    cases.push_back({4000, 200001, true, make_script_bytes({33, 0x03}, 32, 9, {OP_CHECKSIG}), false});             // P2PK odd (35 B)

    // Near-miss and large scripts that must fall back to HEAP and round-trip byte-for-byte
    cases.push_back({5000, 10, false, make_script_bytes({OP_TRUE}, 32, 10), true});                                // 33 B
    cases.push_back({6000, 11, false, make_script_bytes({OP_2, 32}, 32, 11), true});                               // 34 B witness v2
    cases.push_back({7000, 12, false, make_script_bytes({OP_0, 31}, 32, 12), true});                               // 34 B wrong push len
    cases.push_back({8000, 13, false, make_script_bytes({33, 0x04}, 32, 13, {OP_CHECKSIG}), true});                // 35 B wrong key prefix
    cases.push_back({9000, 14, false, make_script_bytes({33, 0x02}, 32, 14, {OP_CHECKSIGVERIFY}), true});          // 35 B wrong trailing op
    cases.push_back({10000, 15, false, make_script_bytes({32, 0x02}, 32, 15, {OP_CHECKSIG}), true});               // 35 B wrong push len
    cases.push_back({11000, 16, false, make_script_bytes({65, 0x04}, 64, 16, {OP_CHECKSIG}), true});               // 67 B uncompressed P2PK
    cases.push_back({12000, 17, false, make_script_bytes({OP_TRUE}, 500, 17), true});                              // 501 B large script

    RecordingCoinsView base;
    ShardedCoinsView cache{&base, /*deterministic=*/true};

    // Place all test coins in one shard and add extra padding to exceed prefetch lookahead distances.
    std::vector<COutPoint> ops;
    std::vector<Coin> expected_coins;
    for (size_t i = 0; i < cases.size(); ++i) {
        const COutPoint op = MakeShardedOutPoint(0x33, i, 0);
        Coin coin{CTxOut{cases[i].amount, cases[i].script}, static_cast<int>(cases[i].height), cases[i].coinbase};
        ops.push_back(op);
        expected_coins.push_back(coin);
        cache.AddCoin(op, Coin{coin}, /*possible_overwrite=*/false);
    }
    for (size_t i = cases.size(); i < 48; ++i) {
        const COutPoint op = MakeShardedOutPoint(0x33, i, 0);
        Coin coin{CTxOut{static_cast<CAmount>(100 + i), make_script_bytes({OP_1, 32}, 32, static_cast<uint8_t>(i))}, 800000, false};
        ops.push_back(op);
        expected_coins.push_back(coin);
        cache.AddCoin(op, Coin{coin}, /*possible_overwrite=*/false);
    }

    auto coins_equal = [](const Coin& a, const Coin& b) {
        return a.out == b.out && a.nHeight == b.nHeight && a.fCoinBase == b.fCoinBase;
    };

    // Verify GetCoin, PeekCoin, and single-shard LookupBatch (with prefetch pipeline) round-trip every field.
    std::vector<ShardedCoinsView::BatchLookupResult> batch(ops.size());
    cache.LookupBatch(ops, batch);
    for (size_t i = 0; i < ops.size(); ++i) {
        BOOST_REQUIRE(batch[i].status == ShardedCoinsView::CacheLookupStatus::LIVE);
        BOOST_CHECK(coins_equal(batch[i].coin, expected_coins[i]));

        auto peeked = cache.PeekCoin(ops[i]);
        BOOST_REQUIRE(peeked.has_value());
        BOOST_CHECK(coins_equal(*peeked, expected_coins[i]));
    }
    cache.SanityCheck();

    // Verify compact scripts (P2WSH, P2TR, P2PK compressed) incur ZERO heap allocation,
    // matching inline P2PKH memory usage byte-for-byte, while near-miss scripts charge heap usage.
    ShardedCoinsView p2pkh_cache{&base, /*deterministic=*/true};
    ShardedCoinsView compact_cache{&base, /*deterministic=*/true};
    ShardedCoinsView nearmiss_cache{&base, /*deterministic=*/true};
    const CScript s_p2pkh = make_script_bytes({OP_DUP, OP_HASH160, 20}, 20, 21, {OP_EQUALVERIFY, OP_CHECKSIG});
    const CScript s_p2wsh = make_script_bytes({OP_0, 32}, 32, 22);
    const CScript s_p2tr = make_script_bytes({OP_1, 32}, 32, 23);
    const CScript s_p2pk_even = make_script_bytes({33, 0x02}, 32, 24, {OP_CHECKSIG});
    const CScript s_p2pk_odd = make_script_bytes({33, 0x03}, 32, 25, {OP_CHECKSIG});
    const CScript s_nearmiss = make_script_bytes({OP_2, 32}, 32, 26);
    const std::array<CScript, 4> compact_scripts{s_p2wsh, s_p2tr, s_p2pk_even, s_p2pk_odd};
    for (uint32_t i = 0; i < 1000; ++i) {
        const COutPoint op = MakeShardedOutPoint(0x44, i, 0);
        p2pkh_cache.AddCoin(op, Coin{CTxOut{1000, s_p2pkh}, 800000, false}, false);
        compact_cache.AddCoin(op, Coin{CTxOut{1000, compact_scripts[i % 4]}, 800000, false}, false);
        nearmiss_cache.AddCoin(op, Coin{CTxOut{1000, s_nearmiss}, 800000, false}, false);
    }
    BOOST_CHECK_EQUAL(compact_cache.DynamicMemoryUsage(), p2pkh_cache.DynamicMemoryUsage());
    BOOST_CHECK_GT(nearmiss_cache.DynamicMemoryUsage(), compact_cache.DynamicMemoryUsage());
    p2pkh_cache.SanityCheck();
    compact_cache.SanityCheck();
    nearmiss_cache.SanityCheck();

    // Verify Flush streams every packed coin identically into the backing store.
    const uint256 tip = m_rng.rand256();
    cache.SetBestBlock(tip);
    cache.Flush(/*reallocate_cache=*/true);
    for (size_t i = 0; i < ops.size(); ++i) {
        auto db_coin = base.GetCoin(ops[i]);
        BOOST_REQUIRE(db_coin.has_value());
        BOOST_CHECK(coins_equal(*db_coin, expected_coins[i]));
    }

    // Verify CommitBlockBatch rollback preserves both compact and HEAP StoredCoins without leaks or corruption.
    const COutPoint heap_op = MakeShardedOutPoint(0x55, 1, 0);
    const COutPoint compact_op = MakeShardedOutPoint(0x55, 2, 0);
    const COutPoint missing_op = MakeShardedOutPoint(0x55, 999, 0);
    const Coin orig_heap{CTxOut{77777, make_script_bytes({OP_TRUE}, 200, 42)}, 50, false};
    const Coin orig_compact{CTxOut{88888, s_p2tr}, 51, true};
    cache.AddCoin(heap_op, Coin{orig_heap}, false);
    cache.AddCoin(compact_op, Coin{orig_compact}, false);
    const uint64_t epoch = cache.FlushEpoch();

    std::vector<ShardedCoinsView::BatchCommitBlockMeta> meta{{m_rng.rand256(), epoch, /*height=*/52, /*enforce_bip30=*/false}};
    std::array<std::vector<ShardedCoinsView::BatchCommitOp>, ShardedCoinsView::NUM_SHARDS> batch_ops;
    batch_ops[0x55].push_back({heap_op, Coin{}, 0, ShardedCoinsView::BatchCommitOp::Kind::SPEND, false, false});
    batch_ops[0x55].push_back({compact_op, Coin{}, 0, ShardedCoinsView::BatchCommitOp::Kind::SPEND, false, false});
    // Trigger conflict in same block so both spends must roll back via StoredCoin move restoration.
    batch_ops[0x55].push_back({missing_op, Coin{}, 0, ShardedCoinsView::BatchCommitOp::Kind::SPEND, false, false});
    BOOST_CHECK_EQUAL(cache.CommitBlockBatch(meta, batch_ops), 0U);

    auto restored_heap = cache.PeekCoin(heap_op);
    auto restored_compact = cache.PeekCoin(compact_op);
    BOOST_REQUIRE(restored_heap.has_value());
    BOOST_REQUIRE(restored_compact.has_value());
    BOOST_CHECK(coins_equal(*restored_heap, orig_heap));
    BOOST_CHECK(coins_equal(*restored_compact, orig_compact));
    cache.SanityCheck();
}

BOOST_AUTO_TEST_CASE(sharded_block_commit_parallel_and_full_rollback)
{
    RecordingCoinsView base;
    ShardedCoinsView cache{&base, /*deterministic=*/true};
    ThreadPool pool{"testcommit"};
    pool.Start(4);

    auto build_sharded_commit = [](const uint256& block_hash,
                                   uint64_t epoch,
                                   uint32_t height,
                                   const std::vector<ShardedCoinsView::BlockSpendOp>& raw_spends,
                                   const std::vector<ShardedCoinsView::BlockCreateOp>& raw_creates,
                                   std::vector<ShardedCoinsView::BlockSpendOp>& sorted_spends,
                                   std::vector<ShardedCoinsView::BlockCreateOp>& sorted_creates,
                                   std::array<uint32_t, ShardedCoinsView::NUM_SHARDS + 1>& sp_off,
                                   std::array<uint32_t, ShardedCoinsView::NUM_SHARDS + 1>& cr_off) {
        std::array<uint32_t, ShardedCoinsView::NUM_SHARDS> sp_cnt{}, cr_cnt{};
        for (const auto& s : raw_spends) ++sp_cnt[ShardedCoinsView::ShardIndex(s.outpoint)];
        for (const auto& c : raw_creates) ++cr_cnt[ShardedCoinsView::ShardIndex(COutPoint{c.tx->GetHash(), c.vout_idx})];
        sp_off[0] = 0;
        cr_off[0] = 0;
        for (size_t s = 0; s < ShardedCoinsView::NUM_SHARDS; ++s) {
            sp_off[s + 1] = sp_off[s] + sp_cnt[s];
            cr_off[s + 1] = cr_off[s] + cr_cnt[s];
        }
        sorted_spends.resize(raw_spends.size());
        sorted_creates.resize(raw_creates.size());
        auto sp_pos = sp_off;
        auto cr_pos = cr_off;
        for (const auto& s : raw_spends) sorted_spends[sp_pos[ShardedCoinsView::ShardIndex(s.outpoint)]++] = s;
        for (const auto& c : raw_creates) sorted_creates[cr_pos[ShardedCoinsView::ShardIndex(COutPoint{c.tx->GetHash(), c.vout_idx})]++] = c;
        return ShardedCoinsView::ShardedBlockCommit{
            .meta = {block_hash, epoch, height, /*enforce_bip30=*/false},
            .spends = sorted_spends,
            .creates = sorted_creates,
            .spend_offsets = &sp_off,
            .create_offsets = &cr_off,
        };
    };

    // Part 1: Production zero-copy ShardedBlockCommit path with >= 8 shards and >= 4096 ops (triggers parallel helpers).
    std::vector<CTransactionRef> txs_b0;
    txs_b0.reserve(256);
    std::vector<ShardedCoinsView::BlockCreateOp> b0_creates_raw;
    const CScript p2tr_script = CScript() << OP_1 << std::vector<unsigned char>(32, 0xAB);
    for (uint32_t i = 0; i < 256; ++i) {
        CMutableTransaction mtx;
        mtx.nLockTime = i + 1;
        mtx.vout.reserve(20);
        for (uint32_t o = 0; o < 20; ++o) {
            mtx.vout.emplace_back(1000 + i * 20 + o, p2tr_script);
        }
        auto ref = MakeTransactionRef(std::move(mtx));
        for (uint32_t o = 0; o < 20; ++o) {
            b0_creates_raw.push_back(ShardedCoinsView::BlockCreateOp{ref.get(), o, /*is_coinbase=*/0u, /*possible_overwrite=*/0u});
        }
        txs_b0.push_back(std::move(ref));
    }

    // Also seed DB-backed coins, dirty-tombstone coins, and overwriteable coins for Part 2 rollback coverage.
    CMutableTransaction overwrite_tx;
    overwrite_tx.nLockTime = 999999;
    overwrite_tx.vout.emplace_back(50000, p2tr_script);
    const CTransactionRef overwrite_tx_ref = MakeTransactionRef(overwrite_tx);
    const COutPoint op_overwrite{overwrite_tx_ref->GetHash(), 0};
    const Coin orig_overwrite_coin{CTxOut{12345, CScript() << OP_TRUE}, 10, true};
    cache.AddCoin(op_overwrite, Coin{orig_overwrite_coin}, /*possible_overwrite=*/false);

    CMutableTransaction tomb_revive_tx;
    tomb_revive_tx.nLockTime = 888888;
    tomb_revive_tx.vout.emplace_back(60000, p2tr_script);
    const CTransactionRef tomb_revive_ref = MakeTransactionRef(tomb_revive_tx);
    const COutPoint op_tomb_revive{tomb_revive_ref->GetHash(), 0};
    base.db[op_tomb_revive] = MakeTestCoin(11111, 5);
    BOOST_REQUIRE(cache.SpendCoin(op_tomb_revive)); // now dirty tombstone

    const COutPoint op_db_only = MakeShardedOutPoint(0x11, 77, 0);
    base.db[op_db_only] = MakeTestCoin(22222, 6); // absent from cache, in DB

    const COutPoint op_cached_nonfresh = MakeShardedOutPoint(0x22, 88, 0);
    base.db[op_cached_nonfresh] = MakeTestCoin(33333, 7);
    BOOST_REQUIRE(cache.GetCoin(op_cached_nonfresh).has_value()); // clean non-fresh live in cache

    const uint256 b0_hash = m_rng.rand256();
    std::vector<ShardedCoinsView::BlockSpendOp> b0_sp;
    std::vector<ShardedCoinsView::BlockCreateOp> b0_cr;
    std::array<uint32_t, ShardedCoinsView::NUM_SHARDS + 1> b0_sp_off{}, b0_cr_off{};
    auto b0_commit = build_sharded_commit(b0_hash, cache.FlushEpoch(), /*height=*/100u, {}, b0_creates_raw, b0_sp, b0_cr, b0_sp_off, b0_cr_off);

    // Block 1 (5,120+ ops across >100 shards): exercises all 5 RestoreKind undo paths + possible_overwrite,
    // then intentionally triggers a missing-spend conflict so block 1 must fully roll back while block 0 commits!
    std::vector<ShardedCoinsView::BlockSpendOp> b1_spends_raw;
    std::vector<ShardedCoinsView::BlockCreateOp> b1_creates_raw;
    // 1. Spend fresh coins created in block 0 (rollback = RESTORE_LIVE)
    for (size_t i = 0; i < 128; ++i) {
        for (uint32_t o = 0; o < 16; ++o) {
            b1_spends_raw.push_back({COutPoint{txs_b0[i]->GetHash(), o}, /*expected_height=*/100u, /*expected_coinbase=*/0u, /*from_db=*/false});
        }
    }
    // 2. Spend clean non-fresh cached coin (becomes dirty tombstone; rollback = RESTORE_LIVE_FROM_TOMB)
    b1_spends_raw.push_back({op_cached_nonfresh, /*expected_height=*/7u, /*expected_coinbase=*/0u, /*from_db=*/false});
    // 3. Spend DB-only coin (creates dirty tombstone; rollback = ERASE_TOMB)
    b1_spends_raw.push_back({op_db_only, /*expected_height=*/6u, /*expected_coinbase=*/0u, /*from_db=*/true});
    // 4. Overwrite tombstone with create (rollback = RESTORE_TOMB_FROM_LIVE)
    b1_creates_raw.push_back(ShardedCoinsView::BlockCreateOp{tomb_revive_ref.get(), 0u, /*is_coinbase=*/0u, /*possible_overwrite=*/0u});
    // 5. Overwrite existing live coin with possible_overwrite=true (rollback = RESTORE_LIVE with original coin)
    b1_creates_raw.push_back(ShardedCoinsView::BlockCreateOp{overwrite_tx_ref.get(), 0u, /*is_coinbase=*/1u, /*possible_overwrite=*/1u});
    // 6. Create fresh coins in block 1 (rollback = ERASE_LIVE)
    std::vector<CTransactionRef> txs_b1;
    for (uint32_t i = 0; i < 128; ++i) {
        CMutableTransaction mtx;
        mtx.nLockTime = 50000 + i;
        for (uint32_t o = 0; o < 16; ++o) mtx.vout.emplace_back(777 + o, p2tr_script);
        auto ref = MakeTransactionRef(std::move(mtx));
        for (uint32_t o = 0; o < 16; ++o) {
            b1_creates_raw.push_back(ShardedCoinsView::BlockCreateOp{ref.get(), o, /*is_coinbase=*/0u, /*possible_overwrite=*/0u});
        }
        txs_b1.push_back(std::move(ref));
    }
    // 7. Conflicting spend of an absent non-DB outpoint triggers block 1 rollback!
    b1_spends_raw.push_back({MakeShardedOutPoint(0xEE, 99999, 0), /*expected_height=*/1u, /*expected_coinbase=*/0u, /*from_db=*/false});

    const uint256 b1_hash = m_rng.rand256();
    std::vector<ShardedCoinsView::BlockSpendOp> b1_sp;
    std::vector<ShardedCoinsView::BlockCreateOp> b1_cr;
    std::array<uint32_t, ShardedCoinsView::NUM_SHARDS + 1> b1_sp_off{}, b1_cr_off{};
    auto b1_commit = build_sharded_commit(b1_hash, cache.FlushEpoch(), /*height=*/101u, b1_spends_raw, b1_creates_raw, b1_sp, b1_cr, b1_sp_off, b1_cr_off);

    const uint64_t epoch_before_commit = cache.FlushEpoch();
    std::array<ShardedCoinsView::ShardedBlockCommit, 2> commits{b0_commit, b1_commit};
    BOOST_CHECK_EQUAL(cache.CommitBlockBatch(commits, &pool), 1U);
    BOOST_CHECK(cache.GetBestBlock() == b0_hash);
    BOOST_CHECK_GT(cache.FlushEpoch(), epoch_before_commit);

    // Block 0 creates must all be live (block 1's spends of them rolled back).
    for (size_t i = 0; i < 128; ++i) {
        auto c = cache.PeekCoin(COutPoint{txs_b0[i]->GetHash(), 0});
        BOOST_REQUIRE(c.has_value());
        BOOST_CHECK_EQUAL(c->out.nValue, static_cast<CAmount>(1000 + i * 20));
        BOOST_CHECK_EQUAL(c->nHeight, 100U);
    }
    // Block 1 creates must all be erased.
    for (size_t i = 0; i < 128; ++i) {
        BOOST_CHECK(!cache.HaveCoinInCache(COutPoint{txs_b1[i]->GetHash(), 0}));
    }
    // Verify every RestoreKind rollback state:
    BOOST_CHECK(cache.PeekCacheStatus(op_cached_nonfresh) == ShardedCoinsView::CacheLookupStatus::LIVE);
    BOOST_CHECK_EQUAL(cache.PeekCoin(op_cached_nonfresh)->out.nValue, 33333);
    BOOST_CHECK(cache.PeekCacheStatus(op_db_only) == ShardedCoinsView::CacheLookupStatus::ABSENT);
    BOOST_CHECK(cache.PeekCacheStatus(op_tomb_revive) == ShardedCoinsView::CacheLookupStatus::TOMBSTONE);
    auto restored_ov = cache.PeekCoin(op_overwrite);
    BOOST_REQUIRE(restored_ov.has_value());
    BOOST_CHECK_EQUAL(restored_ov->out.nValue, orig_overwrite_coin.out.nValue);
    BOOST_CHECK_EQUAL(restored_ov->nHeight, orig_overwrite_coin.nHeight);
    cache.SanityCheck();
    pool.Stop();
}

BOOST_AUTO_TEST_CASE(sharded_randomized_differential_model_check)
{
    RecordingCoinsView base_sharded;
    RecordingCoinsView base_ref;
    ShardedCoinsView sharded{&base_sharded, /*deterministic=*/true};
    CCoinsViewCache ref{&base_ref, /*deterministic=*/true};

    // Key universe spanning 16 shards and multiple vouts per txid.
    std::vector<COutPoint> universe;
    universe.reserve(64);
    for (uint32_t i = 0; i < 64; ++i) {
        universe.push_back(MakeShardedOutPoint(static_cast<uint8_t>((i * 17) & 0xFF), i / 2, i % 3));
    }

    auto make_random_coin = [&](int height) -> Coin {
        const CAmount val = static_cast<CAmount>(m_rng.randrange(1'000'000));
        const bool cb = m_rng.randbool();
        const int kind = static_cast<int>(m_rng.randrange(5));
        CScript script;
        switch (kind) {
        case 0: // inline small script
            script = CScript() << OP_TRUE;
            break;
        case 1: // P2WPKH (22B)
            script = CScript() << OP_0 << m_rng.randbytes(20);
            break;
        case 2: // P2TR (34B)
            script = CScript() << OP_1 << m_rng.randbytes(32);
            break;
        case 3: // heap script (>32B)
            script = CScript() << m_rng.randbytes(48) << OP_DROP << OP_TRUE;
            break;
        default: // unspendable OP_RETURN (must be ignored by AddCoin)
            script = CScript() << OP_RETURN << m_rng.randbytes(8);
            break;
        }
        return Coin{CTxOut{val, std::move(script)}, height, cb};
    };

    auto check_full_equivalence = [&]() {
        BOOST_CHECK(sharded.GetBestBlock() == ref.GetBestBlock());
        for (const auto& op : universe) {
            const auto a = sharded.PeekCoin(op);
            const auto b = ref.PeekCoin(op);
            BOOST_REQUIRE_EQUAL(a.has_value(), b.has_value());
            if (a) {
                BOOST_CHECK(a->out == b->out);
                BOOST_CHECK_EQUAL(a->nHeight, b->nHeight);
                BOOST_CHECK_EQUAL(a->fCoinBase, b->fCoinBase);
            }
            BOOST_CHECK_EQUAL(sharded.HaveCoin(op), ref.HaveCoin(op));
        }
        sharded.SanityCheck();
        ref.SanityCheck();
    };

    auto check_base_db_equivalence = [&]() {
        BOOST_CHECK(base_sharded.best_block == base_ref.best_block);
        BOOST_REQUIRE_EQUAL(base_sharded.db.size(), base_ref.db.size());
        for (const auto& [op, ref_coin] : base_ref.db) {
            auto it = base_sharded.db.find(op);
            BOOST_REQUIRE(it != base_sharded.db.end());
            BOOST_CHECK(it->second.out == ref_coin.out);
            BOOST_CHECK_EQUAL(it->second.nHeight, ref_coin.nHeight);
            BOOST_CHECK_EQUAL(it->second.fCoinBase, ref_coin.fCoinBase);
        }
    };

    int height = 1;
    uint256 tip = m_rng.rand256();
    sharded.SetBestBlock(tip);
    ref.SetBestBlock(tip);

    for (int step = 0; step < 1500; ++step) {
        const int action = static_cast<int>(m_rng.randrange(100));
        const COutPoint& op = universe[m_rng.randrange(universe.size())];

        if (action < 28) {
            // AddCoin (respecting overwrite rules)
            const bool exists = ref.HaveCoin(op);
            const bool possible_overwrite = exists || m_rng.randbool();
            const Coin c = make_random_coin(height);
            sharded.AddCoin(op, Coin{c}, possible_overwrite);
            ref.AddCoin(op, Coin{c}, possible_overwrite);
        } else if (action < 52) {
            // SpendCoin
            const bool had_coin = ref.HaveCoin(op);
            Coin moved_s, moved_r;
            const bool rs = sharded.SpendCoin(op, &moved_s);
            const bool rr = ref.SpendCoin(op, &moved_r);
            BOOST_CHECK_EQUAL(rs, had_coin);
            if (had_coin) {
                BOOST_REQUIRE(rr);
                BOOST_CHECK(moved_s.out == moved_r.out);
                BOOST_CHECK_EQUAL(moved_s.nHeight, moved_r.nHeight);
                BOOST_CHECK_EQUAL(moved_s.fCoinBase, moved_r.fCoinBase);
            } else {
                // CCoinsViewCache::SpendCoin returns true with a spent Coin on an unflushed tombstone.
                BOOST_CHECK(!rr || moved_r.IsSpent());
            }
        } else if (action < 62) {
            // Caching GetCoin
            const auto cs = sharded.GetCoin(op);
            const auto cr = ref.GetCoin(op);
            BOOST_REQUIRE_EQUAL(cs.has_value(), cr.has_value());
            if (cs) BOOST_CHECK(cs->out == cr->out);
        } else if (action < 70) {
            // Uncache
            sharded.Uncache(op);
            ref.Uncache(op);
        } else if (action < 78) {
            // Stacked overlay BatchWrite via CCoinsViewCache::Flush()
            CCoinsViewCache over_s{&sharded};
            CCoinsViewCache over_r{&ref};
            const COutPoint& op2 = universe[m_rng.randrange(universe.size())];
            if (over_r.HaveCoin(op2)) {
                BOOST_CHECK_EQUAL(over_s.SpendCoin(op2), over_r.SpendCoin(op2));
            } else {
                const Coin c = make_random_coin(height);
                over_s.AddCoin(op2, Coin{c}, /*possible_overwrite=*/false);
                over_r.AddCoin(op2, Coin{c}, /*possible_overwrite=*/false);
            }
            tip = m_rng.rand256();
            over_s.SetBestBlock(tip);
            over_r.SetBestBlock(tip);
            over_s.Flush();
            over_r.Flush();
        } else if (action < 84) {
            // Streaming BatchWriteEntries() directly into both caches
            const Coin c = make_random_coin(height);
            const bool do_write = !c.out.scriptPubKey.IsUnspendable() && m_rng.randbool();
            tip = m_rng.rand256();
            auto emit = [&](CoinsWriteSink& sink) {
                if (do_write) {
                    sink.Write(op, c);
                } else {
                    sink.Erase(op);
                }
            };
            sharded.BatchWriteEntries(1, 1, tip, emit);
            ref.BatchWriteEntries(1, 1, tip, emit);
        } else if (action < 92) {
            // CommitBlockBatch (2 blocks, second optionally conflicts and rolls back)
            const uint256 b0 = m_rng.rand256();
            const uint256 b1 = m_rng.rand256();
            const uint64_t epoch = sharded.FlushEpoch();
            std::vector<ShardedCoinsView::BatchCommitOp> ops;
            std::vector<std::pair<COutPoint, std::optional<Coin>>> b0_expected;

            // Block 0: 1 valid spend (if available) + 1 valid create (if absent and spendable)
            const COutPoint& sp_op = universe[m_rng.randrange(universe.size())];
            if (ref.HaveCoin(sp_op)) {
                const bool from_db = !sharded.HaveCoinInCache(sp_op);
                ops.push_back({sp_op, Coin{}, 0, ShardedCoinsView::BatchCommitOp::Kind::SPEND, from_db, false});
                b0_expected.emplace_back(sp_op, std::nullopt);
            }
            const COutPoint& cr_op = universe[m_rng.randrange(universe.size())];
            if (cr_op != sp_op && !ref.HaveCoin(cr_op)) {
                Coin c = make_random_coin(height);
                if (!c.out.scriptPubKey.IsUnspendable()) {
                    ops.push_back({cr_op, c, 0, ShardedCoinsView::BatchCommitOp::Kind::CREATE, false, false});
                    b0_expected.emplace_back(cr_op, c);
                }
            }
            // Block 1: intentionally inject a conflict on half the runs so rollback is continuously stress-tested.
            const bool force_conflict = m_rng.randbool();
            std::vector<ShardedCoinsView::BatchCommitOp> b1_ops;
            if (force_conflict) {
                // Create a coin in block 1 and then spend an outpoint outside the universe (guaranteed missing).
                const COutPoint absent = MakeShardedOutPoint(0xFE, 999999, 0);
                ops.push_back({absent, Coin{}, 1, ShardedCoinsView::BatchCommitOp::Kind::SPEND, false, false});
            }
            std::vector<ShardedCoinsView::BatchCommitBlockMeta> blocks{
                {b0, epoch, static_cast<uint32_t>(height), /*enforce_bip30=*/false},
                {b1, epoch, static_cast<uint32_t>(height + 1), /*enforce_bip30=*/false},
            };
            auto shard_ops = ToShardOps(ops);
            const size_t committed = sharded.CommitBlockBatch(blocks, shard_ops);
            BOOST_CHECK_EQUAL(committed, force_conflict ? 1U : 2U);
            // Apply committed blocks to ref.
            for (const auto& [k, maybe_c] : b0_expected) {
                if (maybe_c) {
                    ref.AddCoin(k, Coin{*maybe_c}, /*possible_overwrite=*/false);
                } else {
                    BOOST_REQUIRE(ref.SpendCoin(k));
                }
            }
            tip = force_conflict ? b0 : b1;
            ref.SetBestBlock(tip);
            ++height;
        } else if (action < 96) {
            // Sync
            sharded.Sync();
            ref.Sync();
            check_base_db_equivalence();
        } else {
            // Flush (alternating reallocate_cache)
            const uint64_t ep_before = sharded.FlushEpoch();
            const bool realloc_cache = m_rng.randbool();
            sharded.Flush(realloc_cache);
            ref.Flush(realloc_cache);
            BOOST_CHECK_EQUAL(sharded.FlushEpoch(), ep_before + 1);
            check_base_db_equivalence();
        }

        check_full_equivalence();
    }

    check_full_equivalence();
    sharded.Flush();
    ref.Flush();
    check_base_db_equivalence();
    check_full_equivalence();
}

BOOST_AUTO_TEST_CASE(sharded_commit_bip30_and_stale_flush_epoch_conflicts)
{
    RecordingCoinsView base;
    ShardedCoinsView cache{&base, /*deterministic=*/true};

    const COutPoint op_cached = MakeShardedOutPoint(0x10, 1, 0);
    const COutPoint op_db = MakeShardedOutPoint(0x20, 2, 0);
    const COutPoint op_fresh = MakeShardedOutPoint(0x30, 3, 0);

    const Coin c_orig = MakeTestCoin(1000, 10, true);
    const Coin c_db = MakeTestCoin(2000, 11, true);
    base.db[op_db] = c_db;
    cache.AddCoin(op_cached, Coin{c_orig}, /*possible_overwrite=*/false);
    const uint256 tip0 = m_rng.rand256();
    cache.SetBestBlock(tip0);

    // 1. Stale FlushEpoch with enforce_bip30=true must reject block 0 immediately and leave cache untouched.
    {
        const uint256 b_stale = m_rng.rand256();
        std::vector<ShardedCoinsView::BatchCommitBlockMeta> blocks{
            {b_stale, cache.FlushEpoch() + 1, /*height=*/12, /*enforce_bip30=*/true},
        };
        std::vector<ShardedCoinsView::BatchCommitOp> ops{
            {op_fresh, MakeTestCoin(500, 12), 0, ShardedCoinsView::BatchCommitOp::Kind::CREATE, false, false},
        };
        auto shard_ops = ToShardOps(ops);
        BOOST_CHECK_EQUAL(cache.CommitBlockBatch(blocks, shard_ops), 0U);
        BOOST_CHECK(cache.GetBestBlock() == tip0);
        BOOST_CHECK(!cache.HaveCoinInCache(op_fresh));
    }

    // 2. Active BIP30 collision against an unspent cached coin rejects block and rolls back earlier creates.
    {
        const uint256 b_dup = m_rng.rand256();
        std::vector<ShardedCoinsView::BatchCommitBlockMeta> blocks{
            {b_dup, cache.FlushEpoch(), /*height=*/12, /*enforce_bip30=*/true},
        };
        std::vector<ShardedCoinsView::BatchCommitOp> ops{
            {op_fresh, MakeTestCoin(500, 12), 0, ShardedCoinsView::BatchCommitOp::Kind::CREATE, false, false},
            {op_cached, MakeTestCoin(999, 12, true), 0, ShardedCoinsView::BatchCommitOp::Kind::CREATE, false, /*possible_overwrite=*/false},
        };
        auto shard_ops = ToShardOps(ops);
        BOOST_CHECK_EQUAL(cache.CommitBlockBatch(blocks, shard_ops), 0U);
        BOOST_CHECK(cache.GetBestBlock() == tip0);
        BOOST_CHECK(!cache.HaveCoinInCache(op_fresh));
        BOOST_CHECK_EQUAL(cache.PeekCoin(op_cached)->out.nValue, 1000);
    }

    // 3. Same collision with enforce_bip30=false and possible_overwrite=true succeeds (historical BIP30 repeat).
    {
        const uint256 b_ok = m_rng.rand256();
        std::vector<ShardedCoinsView::BatchCommitBlockMeta> blocks{
            {b_ok, cache.FlushEpoch(), /*height=*/12, /*enforce_bip30=*/false},
        };
        std::vector<ShardedCoinsView::BatchCommitOp> ops{
            {op_cached, MakeTestCoin(999, 12, true), 0, ShardedCoinsView::BatchCommitOp::Kind::CREATE, false, /*possible_overwrite=*/true},
        };
        auto shard_ops = ToShardOps(ops);
        BOOST_CHECK_EQUAL(cache.CommitBlockBatch(blocks, shard_ops), 1U);
        BOOST_CHECK(cache.GetBestBlock() == b_ok);
        BOOST_CHECK_EQUAL(cache.PeekCoin(op_cached)->out.nValue, 999);
    }

    cache.SanityCheck();
}

BOOST_AUTO_TEST_CASE(sharded_concurrent_readers_during_commits_and_flushes)
{
    struct ThreadSafeCoinsView final : public CCoinsView {
        mutable std::shared_mutex mtx;
        RecordingCoinsView inner;
        std::optional<Coin> GetCoin(const COutPoint& op) const override
        {
            std::shared_lock lock{mtx};
            return inner.GetCoin(op);
        }
        std::optional<Coin> PeekCoin(const COutPoint& op) const override
        {
            std::shared_lock lock{mtx};
            return inner.PeekCoin(op);
        }
        bool HaveCoin(const COutPoint& op) const override
        {
            std::shared_lock lock{mtx};
            return inner.HaveCoin(op);
        }
        uint256 GetBestBlock() const override
        {
            std::shared_lock lock{mtx};
            return inner.GetBestBlock();
        }
        std::vector<uint256> GetHeadBlocks() const override { return {}; }
        size_t EstimateSize() const override
        {
            std::shared_lock lock{mtx};
            return inner.EstimateSize();
        }
        void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) override
        {
            std::unique_lock lock{mtx};
            inner.BatchWrite(cursor, block_hash);
        }
        void BatchWriteEntries(size_t d, size_t t, const uint256& bh, const std::function<void(CoinsWriteSink&)>& emit) override
        {
            std::unique_lock lock{mtx};
            inner.BatchWriteEntries(d, t, bh, emit);
        }
    } base;

    ShardedCoinsView cache{&base, /*deterministic=*/true};
    ThreadPool pool{"conc"};
    pool.Start(4);

    std::vector<COutPoint> ops;
    ops.reserve(128);
    for (uint32_t i = 0; i < 128; ++i) {
        ops.push_back(MakeShardedOutPoint(static_cast<uint8_t>((i * 2) & 0xFF), i, 0));
    }

    std::atomic<bool> stop{false};
    std::atomic<size_t> reader_ops{0};
    std::atomic<size_t> bad_values{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&, r] {
            std::vector<ShardedCoinsView::BatchLookupResult> results(ops.size());
            std::vector<ShardedCoinsView::CacheLookupStatus> statuses(ops.size());
            while (!stop.load(std::memory_order_acquire)) {
                cache.LookupBatch(ops, results);
                cache.PeekCacheStatusBatch(ops, statuses);
                for (size_t i = r; i < ops.size(); i += 4) {
                    auto c = cache.PeekCoin(ops[i]);
                    if (c && c->out.nValue < 1000) bad_values.fetch_add(1, std::memory_order_relaxed);
                    (void)cache.HaveCoinInCache(ops[i]);
                    (void)cache.PeekCacheStatus(ops[i]);
                }
                (void)cache.FlushEpoch();
                reader_ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (int round = 0; round < 30; ++round) {
        std::vector<ShardedCoinsView::BatchCommitOp> batch_ops;
        for (size_t i = 0; i < ops.size(); ++i) {
            if (round % 2 == 0) {
                batch_ops.push_back({ops[i], MakeTestCoin(1000 + round * 10 + static_cast<CAmount>(i), round + 1), 0, ShardedCoinsView::BatchCommitOp::Kind::CREATE, false, true});
            } else {
                const bool from_db = !cache.HaveCoinInCache(ops[i]);
                batch_ops.push_back({ops[i], Coin{}, 0, ShardedCoinsView::BatchCommitOp::Kind::SPEND, from_db, false});
            }
        }
        std::vector<ShardedCoinsView::BatchCommitBlockMeta> meta{{m_rng.rand256(), cache.FlushEpoch(), static_cast<uint32_t>(round + 1), /*enforce_bip30=*/false}};
        auto shard_ops = ToShardOps(batch_ops);
        BOOST_CHECK_EQUAL(cache.CommitBlockBatch(meta, shard_ops, &pool), 1U);
        if (round % 5 == 0) {
            cache.Sync(&pool);
        } else if (round % 7 == 0) {
            cache.Flush(/*reallocate_cache=*/false, &pool);
        }
    }

    stop.store(true, std::memory_order_release);
    for (auto& th : readers) th.join();
    BOOST_CHECK_GT(reader_ops.load(), 0U);
    BOOST_CHECK_EQUAL(bad_values.load(), 0U);
    cache.SanityCheck();
    pool.Stop();
}

BOOST_AUTO_TEST_CASE(sharded_max_31bit_height_roundtrip)
{
    static_assert(ShardedCoinsView::MAX_HEIGHT == (uint32_t{1} << 31) - 1);
    constexpr uint32_t MAX_H = ShardedCoinsView::MAX_HEIGHT;

    RecordingCoinsView base;
    ShardedCoinsView cache{&base, /*deterministic=*/true};

    // Direct AddCoin / PeekCoin / GetCoin / LookupBatch / Flush roundtrip at MAX_HEIGHT.
    const COutPoint op_cb = MakeShardedOutPoint(0x12, 1, 0);
    const COutPoint op_std = MakeShardedOutPoint(0x34, 2, 0);
    cache.AddCoin(op_cb, MakeTestCoin(MAX_MONEY, static_cast<int>(MAX_H), /*coinbase=*/true), /*possible_overwrite=*/false);
    cache.AddCoin(op_std, MakeTestCoin(123456, static_cast<int>(MAX_H), /*coinbase=*/false), /*possible_overwrite=*/false);

    auto c1 = cache.PeekCoin(op_cb);
    auto c2 = cache.PeekCoin(op_std);
    BOOST_REQUIRE(c1.has_value());
    BOOST_REQUIRE(c2.has_value());
    BOOST_CHECK_EQUAL(c1->nHeight, MAX_H);
    BOOST_CHECK(c1->IsCoinBase());
    BOOST_CHECK_EQUAL(c1->out.nValue, MAX_MONEY);
    BOOST_CHECK_EQUAL(c2->nHeight, MAX_H);
    BOOST_CHECK(!c2->IsCoinBase());
    BOOST_CHECK_EQUAL(c2->out.nValue, 123456);

    const std::array<COutPoint, 2> lookup_ops{op_cb, op_std};
    std::array<ShardedCoinsView::BatchLookupResult, 2> lookup_res;
    cache.LookupBatch(lookup_ops, lookup_res);
    BOOST_CHECK(lookup_res[0].status == ShardedCoinsView::CacheLookupStatus::LIVE);
    BOOST_CHECK_EQUAL(lookup_res[0].coin.nHeight, MAX_H);
    BOOST_CHECK(lookup_res[0].coin.IsCoinBase());
    BOOST_CHECK(lookup_res[1].status == ShardedCoinsView::CacheLookupStatus::LIVE);
    BOOST_CHECK_EQUAL(lookup_res[1].coin.nHeight, MAX_H);
    BOOST_CHECK(!lookup_res[1].coin.IsCoinBase());

    cache.SetBestBlock(m_rng.rand256());
    cache.Flush(/*reallocate_cache=*/true);
    auto db_cb = base.GetCoin(op_cb);
    BOOST_REQUIRE(db_cb.has_value());
    BOOST_CHECK_EQUAL(db_cb->nHeight, MAX_H);
    BOOST_CHECK(db_cb->IsCoinBase());

    // Zero-copy CommitBlockBatch create & spend at MAX_HEIGHT.
    CMutableTransaction mtx;
    mtx.nLockTime = 42;
    mtx.vout.emplace_back(50000, CScript() << OP_1 << std::vector<unsigned char>(32, 0x5A));
    const CTransactionRef tx_ref = MakeTransactionRef(std::move(mtx));
    const COutPoint op_batch{tx_ref->GetHash(), 0};

    std::vector<ShardedCoinsView::BlockCreateOp> creates{
        {tx_ref.get(), 0u, /*is_coinbase=*/1u, /*possible_overwrite=*/0u},
    };
    std::array<uint32_t, ShardedCoinsView::NUM_SHARDS + 1> sp_off{}, cr_off{};
    const size_t cr_shard = ShardedCoinsView::ShardIndex(op_batch);
    for (size_t s = cr_shard + 1; s <= ShardedCoinsView::NUM_SHARDS; ++s) cr_off[s] = 1;

    ShardedCoinsView::ShardedBlockCommit create_blk{
        .meta = {m_rng.rand256(), cache.FlushEpoch(), MAX_H, /*enforce_bip30=*/false},
        .spends = {},
        .creates = creates,
        .spend_offsets = &sp_off,
        .create_offsets = &cr_off,
    };
    BOOST_CHECK_EQUAL(cache.CommitBlockBatch(std::span{&create_blk, 1}), 1U);
    auto created = cache.PeekCoin(op_batch);
    BOOST_REQUIRE(created.has_value());
    BOOST_CHECK_EQUAL(created->nHeight, MAX_H);
    BOOST_CHECK(created->IsCoinBase());

    std::vector<ShardedCoinsView::BlockSpendOp> spends{
        {op_batch, /*expected_height=*/MAX_H, /*expected_coinbase=*/1u, /*from_db=*/false},
    };
    std::array<uint32_t, ShardedCoinsView::NUM_SHARDS + 1> sp2_off{}, cr2_off{};
    for (size_t s = cr_shard + 1; s <= ShardedCoinsView::NUM_SHARDS; ++s) sp2_off[s] = 1;
    ShardedCoinsView::ShardedBlockCommit spend_blk{
        .meta = {m_rng.rand256(), cache.FlushEpoch(), MAX_H, /*enforce_bip30=*/false},
        .spends = spends,
        .creates = {},
        .spend_offsets = &sp2_off,
        .create_offsets = &cr2_off,
    };
    BOOST_CHECK_EQUAL(cache.CommitBlockBatch(std::span{&spend_blk, 1}), 1U);
    BOOST_CHECK(!cache.HaveCoin(op_batch));
    cache.SanityCheck();
}

BOOST_AUTO_TEST_CASE(sharded_commit_rejects_spend_metadata_mismatch)
{
    RecordingCoinsView base;
    ShardedCoinsView cache{&base, /*deterministic=*/true};

    const uint256 tip0 = m_rng.rand256();
    cache.SetBestBlock(tip0);

    auto run_single_block = [&](uint64_t epoch,
                                uint32_t height,
                                const std::vector<ShardedCoinsView::BlockSpendOp>& raw_spends,
                                const std::vector<ShardedCoinsView::BlockCreateOp>& raw_creates) -> size_t {
        std::array<uint32_t, ShardedCoinsView::NUM_SHARDS> sp_cnt{}, cr_cnt{};
        for (const auto& s : raw_spends) ++sp_cnt[ShardedCoinsView::ShardIndex(s.outpoint)];
        for (const auto& c : raw_creates) ++cr_cnt[ShardedCoinsView::ShardIndex(COutPoint{c.tx->GetHash(), c.vout_idx})];
        std::array<uint32_t, ShardedCoinsView::NUM_SHARDS + 1> sp_off{}, cr_off{};
        for (size_t s = 0; s < ShardedCoinsView::NUM_SHARDS; ++s) {
            sp_off[s + 1] = sp_off[s] + sp_cnt[s];
            cr_off[s + 1] = cr_off[s] + cr_cnt[s];
        }
        std::vector<ShardedCoinsView::BlockSpendOp> sorted_sp(raw_spends.size());
        std::vector<ShardedCoinsView::BlockCreateOp> sorted_cr(raw_creates.size());
        auto sp_pos = sp_off;
        auto cr_pos = cr_off;
        for (const auto& s : raw_spends) sorted_sp[sp_pos[ShardedCoinsView::ShardIndex(s.outpoint)]++] = s;
        for (const auto& c : raw_creates) sorted_cr[cr_pos[ShardedCoinsView::ShardIndex(COutPoint{c.tx->GetHash(), c.vout_idx})]++] = c;
        ShardedCoinsView::ShardedBlockCommit blk{
            .meta = {m_rng.rand256(), epoch, height, /*enforce_bip30=*/false},
            .spends = sorted_sp,
            .creates = sorted_cr,
            .spend_offsets = &sp_off,
            .create_offsets = &cr_off,
        };
        return cache.CommitBlockBatch(std::span{&blk, 1});
    };

    // 1. In-cache live coin: mismatched height or coinbase must reject and roll back sibling creates.
    const COutPoint op_cached = MakeShardedOutPoint(0x21, 1, 0);
    cache.AddCoin(op_cached, MakeTestCoin(10000, 42, /*coinbase=*/true), /*possible_overwrite=*/false);

    CMutableTransaction sibling_mtx;
    sibling_mtx.nLockTime = 7;
    sibling_mtx.vout.emplace_back(5000, CScript() << OP_TRUE);
    const CTransactionRef sibling_tx = MakeTransactionRef(std::move(sibling_mtx));
    const COutPoint op_sibling{sibling_tx->GetHash(), 0};

    BOOST_CHECK_EQUAL(run_single_block(cache.FlushEpoch(), 200,
                                       {{op_cached, /*expected_height=*/41u, /*expected_coinbase=*/1u, /*from_db=*/false}},
                                       {{sibling_tx.get(), 0u, 0u, 0u}}),
                      0U);
    BOOST_CHECK(cache.HaveCoinInCache(op_cached));
    BOOST_CHECK(!cache.HaveCoinInCache(op_sibling));
    BOOST_CHECK(cache.GetBestBlock() == tip0);

    BOOST_CHECK_EQUAL(run_single_block(cache.FlushEpoch(), 200,
                                       {{op_cached, /*expected_height=*/42u, /*expected_coinbase=*/0u, /*from_db=*/false}},
                                       {{sibling_tx.get(), 0u, 0u, 0u}}),
                      0U);
    BOOST_CHECK(cache.HaveCoinInCache(op_cached));
    BOOST_CHECK(!cache.HaveCoinInCache(op_sibling));

    BOOST_CHECK_EQUAL(run_single_block(cache.FlushEpoch(), 200,
                                       {{op_cached, /*expected_height=*/42u, /*expected_coinbase=*/1u, /*from_db=*/false}},
                                       {}),
                      1U);
    BOOST_CHECK(!cache.HaveCoin(op_cached));

    // 2. DB-only coin with stale flush epoch (!epoch_matches -> base->PeekCoin check).
    const COutPoint op_db = MakeShardedOutPoint(0x61, 2, 0);
    base.db[op_db] = MakeTestCoin(20000, 77, /*coinbase=*/false);
    const uint64_t stale_epoch = cache.FlushEpoch() + 1;

    BOOST_CHECK_EQUAL(run_single_block(stale_epoch, 201,
                                       {{op_db, /*expected_height=*/76u, /*expected_coinbase=*/0u, /*from_db=*/true}},
                                       {}),
                      0U);
    BOOST_CHECK(cache.PeekCacheStatus(op_db) == ShardedCoinsView::CacheLookupStatus::ABSENT);

    BOOST_CHECK_EQUAL(run_single_block(stale_epoch, 201,
                                       {{op_db, /*expected_height=*/77u, /*expected_coinbase=*/1u, /*from_db=*/true}},
                                       {}),
                      0U);
    BOOST_CHECK(cache.PeekCacheStatus(op_db) == ShardedCoinsView::CacheLookupStatus::ABSENT);

    BOOST_CHECK_EQUAL(run_single_block(stale_epoch, 201,
                                       {{op_db, /*expected_height=*/77u, /*expected_coinbase=*/0u, /*from_db=*/true}},
                                       {}),
                      1U);
    BOOST_CHECK(cache.PeekCacheStatus(op_db) == ShardedCoinsView::CacheLookupStatus::TOMBSTONE);

    // 3. Legacy BatchCommitOp overload with populated op.coin also verifies height & coinbase.
    const COutPoint op_legacy = MakeShardedOutPoint(0x81, 3, 0);
    cache.AddCoin(op_legacy, MakeTestCoin(30000, 55, /*coinbase=*/false), /*possible_overwrite=*/false);
    {
        std::vector<ShardedCoinsView::BatchCommitBlockMeta> meta{{m_rng.rand256(), cache.FlushEpoch(), 202, false}};
        auto shard_ops = ToShardOps({{op_legacy, MakeTestCoin(30000, 54, false), 0, ShardedCoinsView::BatchCommitOp::Kind::SPEND, false, false}});
        BOOST_CHECK_EQUAL(cache.CommitBlockBatch(meta, shard_ops), 0U);
        BOOST_CHECK(cache.HaveCoinInCache(op_legacy));
    }
    {
        std::vector<ShardedCoinsView::BatchCommitBlockMeta> meta{{m_rng.rand256(), cache.FlushEpoch(), 202, false}};
        auto shard_ops = ToShardOps({{op_legacy, MakeTestCoin(30000, 55, false), 0, ShardedCoinsView::BatchCommitOp::Kind::SPEND, false, false}});
        BOOST_CHECK_EQUAL(cache.CommitBlockBatch(meta, shard_ops), 1U);
        BOOST_CHECK(!cache.HaveCoin(op_legacy));
    }

    cache.SanityCheck();
}

BOOST_AUTO_TEST_CASE(sharded_batchwrite_exception_preserves_accounting)
{
    RecordingCoinsView base;
    ShardedCoinsView cache{&base, /*deterministic=*/true};

    const COutPoint op_conflict = MakeShardedOutPoint(0x42, 100, 0);
    const COutPoint op_other_shard = MakeShardedOutPoint(0x10, 1, 0);
    cache.AddCoin(op_conflict, MakeTestCoin(1000, 1, false, 200), /*possible_overwrite=*/false);
    cache.AddCoin(op_other_shard, MakeTestCoin(2000, 1, false, 200), /*possible_overwrite=*/false);
    cache.SanityCheck();

    CCoinsViewCache overlay{&cache};
    // Add coins in shard 0x42 both before and after op_conflict so shard 0x42 mutates
    // live_count, dirty_count, and script_dynamic_bytes before hitting the FRESH violation.
    for (uint64_t id = 1; id <= 5; ++id) {
        overlay.AddCoin(MakeShardedOutPoint(0x42, id, 0), MakeTestCoin(500 + static_cast<CAmount>(id), 2, false, 200), /*possible_overwrite=*/false);
    }
    // Adding op_conflict with possible_overwrite=false marks it FRESH in overlay even though
    // it already exists live in parent cache, causing ShardedCoinsView::BatchWrite to throw.
    overlay.AddCoin(op_conflict, MakeTestCoin(9999, 2, false, 200), /*possible_overwrite=*/false);
    for (uint64_t id = 200; id <= 205; ++id) {
        overlay.AddCoin(MakeShardedOutPoint(0x42, id, 0), MakeTestCoin(500 + static_cast<CAmount>(id), 2, false, 200), /*possible_overwrite=*/false);
    }
    overlay.AddCoin(MakeShardedOutPoint(0x10, 2, 0), MakeTestCoin(777, 2, false, 200), /*possible_overwrite=*/false);
    overlay.SetBestBlock(m_rng.rand256());

    BOOST_CHECK_THROW(overlay.Flush(), std::logic_error);

    // SanityCheck asserts GetCacheSize(), GetDirtyCount(), DynamicMemoryUsage(),
    // and every shard's reported_* counters match actual shard state.
    BOOST_CHECK_GT(cache.GetCacheSize(), 2U);
    BOOST_CHECK_GT(cache.GetDirtyCount(), 2U);
    BOOST_CHECK_GT(cache.DynamicMemoryUsage(), 0U);
    cache.SanityCheck();
}

BOOST_AUTO_TEST_CASE(sharded_flush_partial_retains_newest_and_compacts)
{
    RecordingCoinsView base;
    ShardedCoinsView cache{&base, /*deterministic=*/true};
    ThreadPool pool{"sharded_partial"};
    pool.Start(4);

    // Seed an existing DB coin so we also exercise tombstones during FlushPartial.
    const COutPoint db_spent_op = MakeShardedOutPoint(0x05, 999999, 0);
    base.db[db_spent_op] = MakeTestCoin(12345, 1, /*coinbase=*/false);

    // Populate 64 shards with 200 coins each across heights 1..200 (12,800 live entries).
    std::vector<COutPoint> oldest_ops;
    std::vector<COutPoint> newest_ops;
    for (uint8_t s = 0; s < 64; ++s) {
        for (uint32_t h = 1; h <= 200; ++h) {
            const COutPoint op = MakeShardedOutPoint(s, h, 0);
            cache.AddCoin(op, MakeTestCoin(1000 + static_cast<CAmount>(h), static_cast<int>(h), /*coinbase=*/false),
                          /*possible_overwrite=*/false);
            if (h <= 20) oldest_ops.push_back(op);
            if (h >= 181) newest_ops.push_back(op);
        }
    }
    BOOST_REQUIRE(cache.SpendCoin(db_spent_op));
    const uint256 tip1 = m_rng.rand256();
    cache.SetBestBlock(tip1);
    cache.SanityCheck();

    const size_t usage_before = cache.DynamicMemoryUsage();
    const uint64_t epoch_before = cache.FlushEpoch();
    const size_t target_bytes = usage_before / 2;

    cache.FlushPartial(target_bytes, &pool);

    // Dirty changes and tombstones must be persisted in globally sorted order.
    BOOST_CHECK(base.best_block == tip1);
    BOOST_CHECK(!base.HaveCoin(db_spent_op));
    BOOST_CHECK_EQUAL(base.db.size(), 64U * 200U);
    BOOST_CHECK(std::is_sorted(base.last_emitted.begin(), base.last_emitted.end(),
                               [](const auto& a, const auto& b) {
                                   return CompareOutPointsLevelDBOrder(a.first, b.first);
                               }));

    // Memory usage must shrink to <= target_bytes, all entries clean, epoch bumped.
    BOOST_CHECK_LE(cache.DynamicMemoryUsage(), target_bytes);
    BOOST_CHECK_LT(cache.DynamicMemoryUsage(), usage_before);
    BOOST_CHECK_EQUAL(cache.GetDirtyCount(), 0U);
    BOOST_CHECK_GT(cache.GetCacheSize(), 0U);
    BOOST_CHECK_LT(cache.GetCacheSize(), 64U * 200U);
    BOOST_CHECK_EQUAL(cache.FlushEpoch(), epoch_before + 1);
    cache.SanityCheck();

    // Oldest coins must be evicted (still in DB); newest coins must remain cached.
    for (const COutPoint& op : oldest_ops) {
        BOOST_CHECK(!cache.HaveCoinInCache(op));
        BOOST_CHECK(base.HaveCoin(op));
        BOOST_CHECK(cache.PeekCoin(op).has_value());
    }
    for (const COutPoint& op : newest_ops) {
        BOOST_CHECK(cache.HaveCoinInCache(op));
        BOOST_CHECK(base.HaveCoin(op));
    }

    // Spending a retained clean coin creates a dirty tombstone and flushes cleanly.
    const COutPoint retained_spend = newest_ops.front();
    BOOST_REQUIRE(cache.SpendCoin(retained_spend));
    BOOST_CHECK(cache.PeekCacheStatus(retained_spend) == ShardedCoinsView::CacheLookupStatus::TOMBSTONE);
    const uint256 tip2 = m_rng.rand256();
    cache.SetBestBlock(tip2);

    // Huge target with no eviction still removes the tombstone and bumps FlushEpoch.
    const uint64_t epoch_before_tomb_only = cache.FlushEpoch();
    cache.FlushPartial(size_t{1} << 30, &pool);
    BOOST_CHECK(!base.HaveCoin(retained_spend));
    BOOST_CHECK(!cache.HaveCoin(retained_spend));
    BOOST_CHECK_EQUAL(cache.FlushEpoch(), epoch_before_tomb_only + 1);
    cache.SanityCheck();

    // Fast path: huge target with no tombstones and already under budget keeps all entries
    // and does not bump FlushEpoch.
    const COutPoint clean_keep = MakeShardedOutPoint(0x01, 5000, 0);
    cache.AddCoin(clean_keep, MakeTestCoin(7777, 250), /*possible_overwrite=*/false);
    const size_t size_before_fast = cache.GetCacheSize();
    const uint64_t epoch_before_fast = cache.FlushEpoch();
    cache.FlushPartial(size_t{1} << 30, &pool);
    BOOST_CHECK_EQUAL(cache.GetCacheSize(), size_before_fast);
    BOOST_CHECK(cache.HaveCoinInCache(clean_keep));
    BOOST_CHECK_EQUAL(cache.GetDirtyCount(), 0U);
    BOOST_CHECK_EQUAL(cache.FlushEpoch(), epoch_before_fast);
    cache.SanityCheck();

    // FlushPartial(0) behaves like full Flush(/*reallocate_cache=*/true).
    const uint64_t epoch_before_zero = cache.FlushEpoch();
    cache.FlushPartial(0, &pool);
    BOOST_CHECK_EQUAL(cache.GetCacheSize(), 0U);
    BOOST_CHECK_EQUAL(cache.DynamicMemoryUsage(), 0U);
    BOOST_CHECK_EQUAL(cache.FlushEpoch(), epoch_before_zero + 1);
    cache.SanityCheck();

    // Heap-script trimming path: large scripts pushing a single shard over budget trim oldest first.
    const COutPoint old_big = MakeShardedOutPoint(0x77, 1, 0);
    const COutPoint new_big = MakeShardedOutPoint(0x77, 2, 0);
    cache.AddCoin(old_big, MakeTestCoin(1000, 10, false, /*script_len=*/8192), /*possible_overwrite=*/false);
    cache.AddCoin(new_big, MakeTestCoin(2000, 20, false, /*script_len=*/8192), /*possible_overwrite=*/false);
    const size_t two_big_usage = cache.DynamicMemoryUsage();
    const size_t one_big_budget = two_big_usage - 4096;
    cache.FlushPartial(one_big_budget, &pool);
    BOOST_CHECK_LE(cache.DynamicMemoryUsage(), one_big_budget);
    BOOST_CHECK(!cache.HaveCoinInCache(old_big));
    BOOST_CHECK(cache.HaveCoinInCache(new_big));
    BOOST_CHECK(base.HaveCoin(old_big));
    BOOST_CHECK(base.HaveCoin(new_big));
    cache.SanityCheck();

    pool.Stop();
}

BOOST_AUTO_TEST_CASE(sharded_flush_partial_failure_preserves_cache)
{
    struct FailingCoinsView final : public CCoinsView {
        std::optional<Coin> GetCoin(const COutPoint&) const override { return std::nullopt; }
        std::optional<Coin> PeekCoin(const COutPoint&) const override { return std::nullopt; }
        bool HaveCoin(const COutPoint&) const override { return false; }
        uint256 GetBestBlock() const override { return {}; }
        std::vector<uint256> GetHeadBlocks() const override { return {}; }
        void BatchWrite(CoinsViewCacheCursor&, const uint256&) override { throw std::runtime_error("disk full"); }
        void BatchWriteEntries(size_t, size_t, const uint256&, const std::function<void(CoinsWriteSink&)>&) override
        {
            throw std::runtime_error("disk full");
        }
        size_t EstimateSize() const override { return 0; }
    };

    FailingCoinsView failing_base;
    ShardedCoinsView cache{&failing_base, /*deterministic=*/true};
    const COutPoint op = MakeShardedOutPoint(0x33, 1, 0);
    cache.AddCoin(op, MakeTestCoin(5000, 42), /*possible_overwrite=*/false);
    cache.SetBestBlock(m_rng.rand256());

    const uint64_t epoch_before = cache.FlushEpoch();
    const size_t dirty_before = cache.GetDirtyCount();
    BOOST_CHECK_THROW(cache.FlushPartial(1024), std::runtime_error);
    BOOST_CHECK_EQUAL(cache.FlushEpoch(), epoch_before);
    BOOST_CHECK_EQUAL(cache.GetDirtyCount(), dirty_before);
    BOOST_CHECK(cache.HaveCoinInCache(op));
    cache.SanityCheck();
}

BOOST_AUTO_TEST_SUITE_END()
