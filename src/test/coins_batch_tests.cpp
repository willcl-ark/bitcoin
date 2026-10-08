// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <coins_batch.h>
#include <primitives/block.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <util/byte_units.h>
#include <util/threadpool.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

class BatchTestView : public CoinsViewEmpty
{
public:
    std::map<COutPoint, Coin> coins;
    mutable std::atomic<size_t> reads{0};
    bool fail_reads{false};

    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override { return PeekCoin(outpoint); }
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override
    {
        ++reads;
        if (fail_reads) {
            throw std::runtime_error("coin read failed");
        }
        const auto it{coins.find(outpoint)};
        if (it == coins.end() || it->second.IsSpent()) {
            return std::nullopt;
        }
        return it->second;
    }
};

class BatchTestCache : public CCoinsViewCache
{
public:
    explicit BatchTestCache(CCoinsView& base) : CCoinsViewCache{&base, true} {}
    const CCoinsCacheEntry* Find(const COutPoint& outpoint) const
    {
        const auto& storage{GetStorage(outpoint)};
        const auto it{storage.coins.find(outpoint)};
        return it == storage.coins.end() ? nullptr : &it->second;
    }
};

CTransactionRef Coinbase(int tag, CScript script = CScript{} << OP_TRUE)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].scriptSig = CScript{} << tag;
    tx.vout.emplace_back(1000, script);
    return MakeTransactionRef(std::move(tx));
}

CTransactionRef Spend(std::vector<COutPoint> inputs, int tag)
{
    CMutableTransaction tx;
    tx.nLockTime = tag;
    for (const auto& outpoint : inputs) {
        tx.vin.emplace_back(outpoint);
    }
    tx.vout.emplace_back(100, CScript{} << OP_TRUE);
    return MakeTransactionRef(std::move(tx));
}

CoinsViewBatch::BlockDescriptor Block(int height, std::vector<CTransactionRef> txs, bool bip30 = false)
{
    auto block{std::make_shared<CBlock>()};
    block->vtx = std::move(txs);
    return {std::move(block), height, bip30};
}

std::vector<CBlockUndo> ApplySerial(CCoinsViewCache& cache, const std::vector<CoinsViewBatch::BlockDescriptor>& blocks)
{
    std::vector<CBlockUndo> undo(blocks.size());
    for (size_t b{0}; b < blocks.size(); ++b) {
        for (const auto& tx : blocks[b].block->vtx) {
            if (!tx->IsCoinBase()) {
                auto& txundo{undo[b].vtxundo.emplace_back()};
                txundo.vprevout.resize(tx->vin.size());
                for (size_t i{0}; i < tx->vin.size(); ++i) {
                    const bool spent{cache.SpendCoin(tx->vin[i].prevout, &txundo.vprevout[i])};
                    BOOST_REQUIRE(spent);
                }
            }
            AddCoins(cache, *tx, blocks[b].height);
        }
    }
    return undo;
}

void CheckCoin(const Coin& actual, const Coin& expected)
{
    BOOST_CHECK(actual.out == expected.out);
    BOOST_CHECK_EQUAL(actual.nHeight, expected.nHeight);
    BOOST_CHECK_EQUAL(actual.fCoinBase, expected.fCoinBase);
}

void CheckCache(const BatchTestCache& actual, const BatchTestCache& expected, const std::vector<COutPoint>& keys)
{
    BOOST_CHECK_EQUAL(actual.GetCacheSize(), expected.GetCacheSize());
    BOOST_CHECK_EQUAL(actual.GetDirtyCount(), expected.GetDirtyCount());
    for (const auto& key : keys) {
        const auto* left{actual.Find(key)};
        const auto* right{expected.Find(key)};
        BOOST_REQUIRE_EQUAL(left != nullptr, right != nullptr);
        if (!left) {
            continue;
        }
        CheckCoin(left->coin, right->coin);
        BOOST_CHECK_EQUAL(left->IsFresh(), right->IsFresh());
        BOOST_CHECK_EQUAL(left->IsDirty(), right->IsDirty());
        BOOST_CHECK(!left->IsConfirmedUnknown());
    }
    actual.SanityCheck();
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(coins_batch_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(serial_equivalence_and_input_lifetimes)
{
    for (const int workers : {0, 4}) {
        for (const bool cache_fresh : {false, true}) {
            BatchTestView base;
            BatchTestCache actual{base};
            BatchTestCache expected{base};
            const auto source{Coinbase(1)};
            const COutPoint external{source->GetHash(), 0};
            const Coin external_coin{source->vout[0], 1, true};
            if (cache_fresh) {
                actual.AddCoin(external, Coin{external_coin}, false);
                expected.AddCoin(external, Coin{external_coin}, false);
            } else {
                base.coins.emplace(external, external_coin);
            }
            const auto first{Spend({external}, 1)};
            const COutPoint intermediate{first->GetHash(), 0};
            const auto second{Spend({intermediate}, 2)};
            const COutPoint final{second->GetHash(), 0};
            const auto cb1{Coinbase(2)};
            const auto cb2{Coinbase(3)};
            const std::vector<CoinsViewBatch::BlockDescriptor> blocks{Block(2, {cb1, first}), Block(3, {cb2, second})};
            ThreadPool pool{"batch"};
            if (workers) {
                pool.Start(workers);
            }
            CoinsViewBatch batch{actual, blocks, 8'000'000};
            BOOST_REQUIRE(batch.Prepare(pool) == CoinsViewBatch::PrepareResult::READY);
            BOOST_CHECK(actual.HaveCoin(external));
            BOOST_CHECK(!actual.HaveCoin(intermediate));
            BOOST_CHECK(!actual.PeekCoin(final));
            BOOST_REQUIRE_EQUAL(batch.Inputs(0, 1).size(), 1U);
            CheckCoin(*batch.Inputs(0, 1)[0], external_coin);
            CheckCoin(*batch.Inputs(1, 1)[0], Coin{first->vout[0], 2, false});
            const auto oracle{ApplySerial(expected, blocks)};
            for (size_t b{0}; b < oracle.size(); ++b) {
                DataStream actual_undo;
                DataStream expected_undo;
                actual_undo << batch.Undo(b);
                expected_undo << oracle[b];
                BOOST_CHECK_EQUAL(actual_undo.str(), expected_undo.str());
            }
            batch.Commit(source->GetHash().ToUint256());
            CheckCache(actual, expected, {external, intermediate, final, {cb1->GetHash(), 0}, {cb2->GetHash(), 0}});
        }
    }
}

BOOST_AUTO_TEST_CASE(cancel_preserves_confirmed_visibility_and_flags)
{
    BatchTestView base;
    BatchTestCache cache{base};
    const auto creation{Coinbase(11)};
    const COutPoint output{creation->GetHash(), 0};
    const Coin old{CTxOut{77, CScript{} << OP_TRUE}, 1, true};
    base.coins.emplace(output, old);
    const std::vector<CoinsViewBatch::BlockDescriptor> blocks{Block(2, {creation})};
    ThreadPool pool{"batch"};
    {
        CoinsViewBatch batch{cache, blocks, 8'000'000};
        BOOST_REQUIRE(batch.Prepare(pool) == CoinsViewBatch::PrepareResult::READY);
        BOOST_CHECK_EQUAL(base.reads.load(), 0U);
        BOOST_REQUIRE(cache.PeekCoin(output));
        CheckCoin(*cache.PeekCoin(output), old);
        BOOST_REQUIRE(cache.GetCoin(output));
        CheckCoin(*cache.GetCoin(output), old);
        BOOST_CHECK_EQUAL(cache.GetDirtyCount(), 0U);
    }
    BOOST_CHECK_EQUAL(cache.GetCacheSize(), 0U);
    CheckCoin(*cache.PeekCoin(output), old);
    cache.SanityCheck();
}

BOOST_AUTO_TEST_CASE(recreation_preserves_dirty_spent_state)
{
    for (const bool spend_creation : {false, true}) {
        BatchTestView base;
        BatchTestCache actual{base};
        BatchTestCache expected{base};
        const auto creation{Spend({{Coinbase(21)->GetHash(), 0}}, 22)};
        const COutPoint output{creation->GetHash(), 0};
        for (auto* cache : {&actual, &expected}) {
            cache->AddCoin(output, Coin{CTxOut{50, CScript{} << OP_TRUE}, 1, false}, true);
            BOOST_REQUIRE(cache->SpendCoin(output));
        }
        const auto funding{Coinbase(21)};
        base.coins.emplace(COutPoint{funding->GetHash(), 0}, Coin{funding->vout[0], 1, true});
        std::vector<CTransactionRef> txs{Coinbase(23), creation};
        if (spend_creation) {
            txs.emplace_back(Spend({output}, 24));
        }
        const std::vector<CoinsViewBatch::BlockDescriptor> blocks{Block(2, txs)};
        ThreadPool pool{"batch"};
        CoinsViewBatch batch{actual, blocks, 8'000'000};
        BOOST_REQUIRE(batch.Prepare(pool) == CoinsViewBatch::PrepareResult::READY);
        ApplySerial(expected, blocks);
        batch.Commit(funding->GetHash().ToUint256());
        std::vector<COutPoint> keys{{funding->GetHash(), 0}};
        for (const auto& tx : txs) {
            keys.emplace_back(tx->GetHash(), 0);
        }
        CheckCache(actual, expected, keys);
    }
}

BOOST_AUTO_TEST_CASE(ordered_failures_return_to_serial_path)
{
    const auto funding{Coinbase(31)};
    const COutPoint external{funding->GetHash(), 0};
    const auto producer{Spend({external}, 32)};
    const COutPoint produced{producer->GetHash(), 0};
    const std::vector<std::vector<CoinsViewBatch::BlockDescriptor>> cases{
        {Block(2, {Coinbase(33), Spend({{Coinbase(99)->GetHash(), 0}}, 34)})},
        {Block(2, {Coinbase(33), Spend({produced}, 34), producer})},
        {Block(2, {Coinbase(33), Spend({external, external}, 34)})},
        {Block(2, {Coinbase(33), producer}), Block(3, {Coinbase(35), Spend({external}, 36)})},
        {Block(2, {funding}, true)},
        {Block(2, {funding}), Block(3, {funding})},
    };
    for (const auto& blocks : cases) {
        BatchTestView base;
        base.coins.emplace(external, Coin{funding->vout[0], 1, true});
        BatchTestCache cache{base};
        ThreadPool pool{"batch"};
        pool.Start(4);
        CoinsViewBatch batch{cache, blocks, 8'000'000};
        BOOST_CHECK(batch.Prepare(pool) == CoinsViewBatch::PrepareResult::FALLBACK);
        BOOST_CHECK_EQUAL(cache.GetCacheSize(), 0U);
        BOOST_CHECK_EQUAL(cache.GetDirtyCount(), 0U);
        BOOST_REQUIRE(cache.PeekCoin(external));
        cache.SanityCheck();
    }
}

BOOST_AUTO_TEST_CASE(bip30_probes_unspendable_outputs_at_block_entry)
{
    BatchTestView base;
    BatchTestCache cache{base};
    const auto funding{Coinbase(41)};
    const COutPoint external{funding->GetHash(), 0};
    CMutableTransaction recreation;
    recreation.vin.emplace_back(external);
    recreation.vout.emplace_back(1, CScript{} << OP_RETURN);
    const auto tx{MakeTransactionRef(std::move(recreation))};
    const COutPoint conflict{tx->GetHash(), 0};
    base.coins.emplace(external, Coin{funding->vout[0], 1, true});
    base.coins.emplace(conflict, Coin{CTxOut{1, CScript{} << OP_TRUE}, 1, false});
    // The old conflicting coin is spent before its unspendable replacement.
    // Probing at transaction time would miss BIP30's block-entry conflict.
    const std::vector<CoinsViewBatch::BlockDescriptor> blocks{Block(2, {Coinbase(42), Spend({conflict}, 43), tx}, true)};
    ThreadPool pool{"batch"};
    CoinsViewBatch batch{cache, blocks, 8'000'000};
    BOOST_CHECK(batch.Prepare(pool) == CoinsViewBatch::PrepareResult::FALLBACK);
    BOOST_CHECK_EQUAL(cache.GetCacheSize(), 0U);
}

BOOST_AUTO_TEST_CASE(runtime_interrupt_and_memory_cleanup)
{
    const auto funding{Coinbase(51)};
    const COutPoint external{funding->GetHash(), 0};
    const std::vector<CoinsViewBatch::BlockDescriptor> blocks{Block(2, {Coinbase(52), Spend({external}, 53)})};
    {
        BatchTestView base;
        base.fail_reads = true;
        BatchTestCache cache{base};
        ThreadPool pool{"batch"};
        pool.Start(4);
        CoinsViewBatch batch{cache, blocks, 8'000'000};
        BOOST_CHECK_THROW(batch.Prepare(pool), std::runtime_error);
        BOOST_CHECK_EQUAL(cache.GetCacheSize(), 0U);
        cache.AddCoin(external, Coin{funding->vout[0], 1, true}, false);
    }
    {
        BatchTestView base;
        BatchTestCache cache{base};
        ThreadPool pool{"batch"};
        pool.Start(4);
        pool.Interrupt();
        CoinsViewBatch batch{cache, blocks, 8'000'000};
        BOOST_CHECK(batch.Prepare(pool) == CoinsViewBatch::PrepareResult::INTERRUPTED);
        BOOST_CHECK_EQUAL(cache.GetCacheSize(), 0U);
    }
    {
        BatchTestView base;
        BatchTestCache cache{base};
        ThreadPool pool{"batch"};
        CoinsViewBatch batch{cache, blocks, 1};
        BOOST_CHECK(batch.Prepare(pool) == CoinsViewBatch::PrepareResult::FALLBACK);
        BOOST_CHECK_EQUAL(cache.GetCacheSize(), 0U);
    }
}

BOOST_AUTO_TEST_CASE(ineligible_block_shape_falls_back)
{
    BatchTestView base;
    BatchTestCache cache{base};
    const std::vector<CoinsViewBatch::BlockDescriptor> blocks{Block(2, {Spend({{Coinbase(91)->GetHash(), 0}}, 92)})};
    ThreadPool pool{"batch"};
    CoinsViewBatch batch{cache, blocks, 8'000'000};
    BOOST_CHECK(batch.Prepare(pool) == CoinsViewBatch::PrepareResult::FALLBACK);
    BOOST_CHECK_EQUAL(cache.GetCacheSize(), 0U);
}

BOOST_AUTO_TEST_CASE(incremental_budget_cancels_large_external_material)
{
    BatchTestView base;
    BatchTestCache cache{base};
    std::vector<COutPoint> inputs;
    for (int i{0}; i < 64; ++i) {
        const COutPoint key{Coinbase(100 + i)->GetHash(), 0};
        inputs.emplace_back(key);
        CScript script;
        script.resize(MAX_SCRIPT_SIZE);
        std::fill(script.begin(), script.end(), OP_TRUE);
        base.coins.emplace(key, Coin{CTxOut{100, script}, 1, false});
    }
    const std::vector<CoinsViewBatch::BlockDescriptor> blocks{Block(2, {Coinbase(200), Spend(inputs, 201)})};
    const auto budget{CoinsViewBatch::EstimateScratch(cache, blocks)};
    ThreadPool pool{"batch"};
    pool.Start(4);
    CoinsViewBatch batch{cache, blocks, budget};
    BOOST_CHECK(batch.Prepare(pool) == CoinsViewBatch::PrepareResult::FALLBACK);
    BOOST_CHECK_EQUAL(cache.GetCacheSize(), 0U);
    BOOST_CHECK_EQUAL(cache.GetDirtyCount(), 0U);
    cache.SanityCheck();
}

BOOST_AUTO_TEST_CASE(commit_flush_and_reopen_database)
{
    const auto path{m_args.GetDataDirBase() / "coins_batch_db"};
    const auto funding{Coinbase(301)};
    const COutPoint external{funding->GetHash(), 0};
    const auto first{Spend({external}, 302)};
    const COutPoint intermediate{first->GetHash(), 0};
    const auto second{Spend({intermediate}, 303)};
    const COutPoint final{second->GetHash(), 0};
    const auto best{funding->GetHash().ToUint256()};
    {
        CCoinsViewDB base{{.path = path, .cache_bytes = 1_MiB, .wipe_data = true}, {}};
        BatchTestCache cache{base};
        cache.AddCoin(external, Coin{funding->vout[0], 1, true}, false);
        cache.SetBestBlock(best);
        cache.Flush();
        const std::vector<CoinsViewBatch::BlockDescriptor> blocks{Block(2, {Coinbase(304), first}), Block(3, {Coinbase(305), second})};
        ThreadPool pool{"batch"};
        pool.Start(4);
        CoinsViewBatch batch{cache, blocks, 8'000'000};
        BOOST_REQUIRE(batch.Prepare(pool) == CoinsViewBatch::PrepareResult::READY);
        batch.Commit(best);
        cache.Flush();
    }
    {
        CCoinsViewDB base{{.path = path, .cache_bytes = 1_MiB}, {}};
        BOOST_CHECK(!base.GetCoin(external));
        BOOST_CHECK(!base.GetCoin(intermediate));
        BOOST_REQUIRE(base.GetCoin(final));
        CheckCoin(*base.GetCoin(final), Coin{second->vout[0], 3, false});
        BOOST_CHECK(base.GetBestBlock() == best);
    }
}

BOOST_AUTO_TEST_SUITE_END()
