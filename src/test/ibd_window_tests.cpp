// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <ibd_window.h>
#include <kernel/disconnected_transactions.h>
#include <node/blockstorage.h>
#include <node/kernel_notifications.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/signingprovider.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/check.h>
#include <validation.h>

#include <atomic>
#include <memory>
#include <vector>

#include <boost/test/unit_test.hpp>

namespace {

/** Construct a valid regtest block on top of `prev` without advancing the active chain tip. */
CBlock BuildBlockOnIndex(
    const ChainstateManager& chainman,
    const CBlockIndex* prev,
    const std::vector<CMutableTransaction>& txns,
    const CScript& coinbase_script)
{
    Assert(prev != nullptr);
    const Consensus::Params& consensus = chainman.GetConsensus();
    const int height = prev->nHeight + 1;

    CBlock block;
    block.nVersion = 0x20000000;
    block.hashPrevBlock = prev->GetBlockHash();
    block.nTime = prev->GetBlockTime() + 1;
    block.nBits = GetNextWorkRequired(prev, &block, consensus);
    block.nNonce = 0;

    CMutableTransaction cb;
    cb.vin.resize(1);
    cb.vin[0].prevout.SetNull();
    cb.vin[0].scriptSig = CScript() << height << OP_0;
    cb.vout.resize(1);
    cb.vout[0].nValue = GetBlockSubsidy(height, consensus);
    cb.vout[0].scriptPubKey = coinbase_script;

    block.vtx.reserve(1 + txns.size());
    block.vtx.push_back(MakeTransactionRef(std::move(cb)));
    for (const auto& tx : txns) {
        block.vtx.push_back(MakeTransactionRef(tx));
    }

    chainman.GenerateCoinbaseCommitment(block, prev);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    while (!CheckProofOfWork(block.GetHash(), block.nBits, consensus)) {
        ++block.nNonce;
    }
    return block;
}

/** Store a block via off-cs_main StoreNewBlock without activating it yet. */
CBlockIndex* StoreBlockForParallelIBD(ChainstateManager& chainman, const CBlock& block)
{
    auto pblock = std::make_shared<const CBlock>(block);
    BlockValidationState state;
    CBlockIndex* pindex = nullptr;
    bool new_block = false;
    BOOST_REQUIRE(chainman.StoreNewBlock(pblock, /*force_processing=*/true, /*min_pow_checked=*/true, state, &pindex, &new_block));
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(new_block);
    return pindex;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(ibd_window_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(ibd_window_intra_window_dependencies_and_self_spends)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& chainstate = chainman.ActiveChainstate();
    const CScript coinbase_script = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    const CScript anyone_script = CScript() << OP_TRUE;

    // Build 4 contiguous blocks above height 100 BEFORE activating them:
    // - Block 101: tx1 spends mature coinbase #1 -> U1; tx2 spends U1 (intra-block self-spend!) -> U1_child
    // - Block 102: tx3 spends U1_child (intra-window dependency on uncommitted Block 101!) -> U2
    // - Block 103: tx4 spends U2 -> U3
    // - Block 104: tx5 spends U3 -> U4
    CTransactionRef cb1 = m_coinbase_txns[0];
    auto [tx1, fee1] = CreateValidTransaction(
        {cb1}, {COutPoint{cb1->GetHash(), 0}}, /*input_height=*/1,
        {coinbaseKey}, {CTxOut{49 * COIN, anyone_script}},
        /*feerate=*/std::nullopt, /*fee_output=*/std::nullopt);
    CTransactionRef tx1_ref = MakeTransactionRef(tx1);
    const COutPoint u1{tx1_ref->GetHash(), 0};

    // Intra-block self-spend spending U1 (OP_TRUE script requires empty scriptSig).
    CMutableTransaction tx2;
    tx2.vin.emplace_back(u1);
    tx2.vout.emplace_back(48 * COIN, anyone_script);
    CTransactionRef tx2_ref = MakeTransactionRef(tx2);
    const COutPoint u1_child{tx2_ref->GetHash(), 0};

    CMutableTransaction tx3;
    tx3.vin.emplace_back(u1_child);
    tx3.vout.emplace_back(47 * COIN, anyone_script);
    CTransactionRef tx3_ref = MakeTransactionRef(tx3);
    const COutPoint u2{tx3_ref->GetHash(), 0};

    CMutableTransaction tx4;
    tx4.vin.emplace_back(u2);
    tx4.vout.emplace_back(46 * COIN, anyone_script);
    CTransactionRef tx4_ref = MakeTransactionRef(tx4);
    const COutPoint u3{tx4_ref->GetHash(), 0};

    CMutableTransaction tx5;
    tx5.vin.emplace_back(u3);
    tx5.vout.emplace_back(45 * COIN, anyone_script);
    CTransactionRef tx5_ref = MakeTransactionRef(tx5);
    const COutPoint u4{tx5_ref->GetHash(), 0};

    CBlockIndex* prev = WITH_LOCK(::cs_main, return chainstate.m_chain.Tip());
    BOOST_REQUIRE_EQUAL(prev->nHeight, 100);

    CBlock blk101 = BuildBlockOnIndex(chainman, prev, {tx1, tx2}, coinbase_script);
    CBlockIndex* idx101 = StoreBlockForParallelIBD(chainman, blk101);

    CBlock blk102 = BuildBlockOnIndex(chainman, idx101, {tx3}, coinbase_script);
    CBlockIndex* idx102 = StoreBlockForParallelIBD(chainman, blk102);

    CBlock blk103 = BuildBlockOnIndex(chainman, idx102, {tx4}, coinbase_script);
    CBlockIndex* idx103 = StoreBlockForParallelIBD(chainman, blk103);

    CBlock blk104 = BuildBlockOnIndex(chainman, idx103, {tx5}, coinbase_script);
    StoreBlockForParallelIBD(chainman, blk104);

    // Ensure IBD is active so parallel IBD window runs (on 3+ core hosts) or exercises the path cleanly.
    chainman.m_cached_is_ibd.store(true, std::memory_order_relaxed);
    // Keep tip old enough that UpdateIBDStatus stays in IBD during activation.
    SetMockTime(std::chrono::seconds{blk104.GetBlockTime() + 48 * 3600});

    BlockValidationState state;
    BOOST_REQUIRE(chainstate.ActivateBestChain(state));

    {
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(chainstate.m_chain.Height(), 104);
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(u1));
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(u1_child));
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(u2));
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(u3));
        auto coin_u4 = chainstate.CoinsTip().GetCoin(u4);
        BOOST_REQUIRE(coin_u4.has_value());
        BOOST_CHECK_EQUAL(coin_u4->out.nValue, 45 * COIN);
        BOOST_CHECK_EQUAL(coin_u4->nHeight, 104);
        chainstate.CoinsTip().SanityCheck();
    }

    // Verify undo data round-trips identically by disconnecting blocks 104..101.
    {
        LOCK(::cs_main);
        LOCK(chainstate.MempoolMutex());
        for (int h = 104; h >= 101; --h) {
            BlockValidationState disc_state;
            BOOST_REQUIRE(chainstate.DisconnectTip(disc_state, /*disconnectpool=*/nullptr));
        }
        BOOST_CHECK_EQUAL(chainstate.m_chain.Height(), 100);
        BOOST_CHECK(chainstate.CoinsTip().HaveCoin(COutPoint{cb1->GetHash(), 0}));
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(u1));
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(u1_child));
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(u4));
        chainstate.CoinsTip().SanityCheck();
    }
}

BOOST_AUTO_TEST_CASE(ibd_window_conflict_triggers_serial_fallback)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& chainstate = chainman.ActiveChainstate();
    const CScript coinbase_script = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    const CScript anyone_script = CScript() << OP_TRUE;

    CTransactionRef cb1 = m_coinbase_txns[0];
    auto [tx1, _fee] = CreateValidTransaction(
        {cb1}, {COutPoint{cb1->GetHash(), 0}}, /*input_height=*/1,
        {coinbaseKey}, {CTxOut{49 * COIN, anyone_script}},
        /*feerate=*/std::nullopt, /*fee_output=*/std::nullopt);
    CTransactionRef tx1_ref = MakeTransactionRef(tx1);
    const COutPoint u1{tx1_ref->GetHash(), 0};

    // Block 102 validly spends U1 -> U2.
    CMutableTransaction tx2;
    tx2.vin.emplace_back(u1);
    tx2.vout.emplace_back(48 * COIN, anyone_script);
    CTransactionRef tx2_ref = MakeTransactionRef(tx2);
    const COutPoint u2{tx2_ref->GetHash(), 0};

    // Block 103 illegally double-spends U1 (which is still visible in WindowOutputTable from Block 101!).
    // During parallel window validation, Block 103 hits a CommitBlockBatch conflict and falls back to serial ConnectTip.
    CMutableTransaction tx3_double_spend;
    tx3_double_spend.vin.emplace_back(u1);
    tx3_double_spend.vout.emplace_back(47 * COIN, anyone_script);

    CBlockIndex* prev = WITH_LOCK(::cs_main, return chainstate.m_chain.Tip());
    CBlock blk101 = BuildBlockOnIndex(chainman, prev, {tx1}, coinbase_script);
    CBlockIndex* idx101 = StoreBlockForParallelIBD(chainman, blk101);

    CBlock blk102 = BuildBlockOnIndex(chainman, idx101, {tx2}, coinbase_script);
    CBlockIndex* idx102 = StoreBlockForParallelIBD(chainman, blk102);

    CBlock blk103 = BuildBlockOnIndex(chainman, idx102, {tx3_double_spend}, coinbase_script);
    CBlockIndex* idx103 = StoreBlockForParallelIBD(chainman, blk103);

    chainman.m_cached_is_ibd.store(true, std::memory_order_relaxed);
    SetMockTime(std::chrono::seconds{blk103.GetBlockTime() + 48 * 3600});

    BlockValidationState state;
    BOOST_REQUIRE(chainstate.ActivateBestChain(state));

    {
        LOCK(::cs_main);
        // Valid prefix 101..102 committed cleanly; conflicting block 103 rejected via serial fallback.
        BOOST_CHECK_EQUAL(chainstate.m_chain.Height(), 102);
        BOOST_CHECK_EQUAL(chainstate.m_chain.Tip(), idx102);
        BOOST_CHECK((idx103->nStatus & BLOCK_FAILED_VALID) != 0);
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(u1));
        BOOST_CHECK(chainstate.CoinsTip().HaveCoin(u2));
        chainstate.CoinsTip().SanityCheck();
    }
}

BOOST_AUTO_TEST_CASE(ibd_window_stopatheight_exact_bound)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& chainstate = chainman.ActiveChainstate();
    const CScript coinbase_script = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    CBlockIndex* prev = WITH_LOCK(::cs_main, return chainstate.m_chain.Tip());
    CBlock last_blk;
    for (int i = 0; i < 5; ++i) {
        last_blk = BuildBlockOnIndex(chainman, prev, {}, coinbase_script);
        prev = StoreBlockForParallelIBD(chainman, last_blk);
    }

    // Configure exact -stopatheight=103 while 5 blocks (101..105) are queued for a multi-block batch.
    m_node.notifications->m_stop_at_height = 103;
    m_node.notifications->m_shutdown_on_fatal_error = false;
    chainman.m_cached_is_ibd.store(true, std::memory_order_relaxed);
    SetMockTime(std::chrono::seconds{last_blk.GetBlockTime() + 48 * 3600});

    BlockValidationState state;
    BOOST_REQUIRE(chainstate.ActivateBestChain(state));

    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), 103);

    // Reset stop height for clean fixture teardown.
    m_node.notifications->m_stop_at_height = 0;
}

BOOST_AUTO_TEST_CASE(ibd_window_crash_before_undo_inline_and_async)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& chainstate = chainman.ActiveChainstate();
    const CScript coinbase_script = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    m_node.notifications->m_shutdown_on_fatal_error = false;

    // Part 1: Async undo write fault hook latches storage I/O error and blocks FlushStateToDisk.
    CBlockIndex* prev = WITH_LOCK(::cs_main, return chainstate.m_chain.Tip());
    CBlock blk101 = BuildBlockOnIndex(chainman, prev, {}, coinbase_script);
    CBlockIndex* idx101 = StoreBlockForParallelIBD(chainman, blk101);

    std::atomic<int> async_hook_hits{0};
    chainman.m_blockman.SetUndoWriteFaultHookForTest([&](const uint256& hash, int height) {
        if (height == 101 && hash == blk101.GetHash()) {
            ++async_hook_hits;
            return true;
        }
        return false;
    });

    {
        LOCK(::cs_main);
        CBlockUndo empty_undo;
        auto serialized = chainman.m_blockman.SerializeBlockUndoPayload(empty_undo, prev->GetBlockHash());
        BlockValidationState state;
        BOOST_REQUIRE(chainman.m_blockman.EnqueueBlockUndoWrite(std::move(serialized), state, *idx101));
        BOOST_CHECK(!chainman.m_blockman.SettlePendingUndoWrites());
        BOOST_CHECK_EQUAL(async_hook_hits.load(), 1);
        BOOST_CHECK(chainman.m_blockman.HasStorageIOError());

        // FlushStateToDisk must refuse to persist block index or UTXO state once latched
        // and fire the fatal flushError notification (setting exit_status = EXIT_FAILURE).
        m_node.exit_status.store(EXIT_SUCCESS);
        BlockValidationState flush_state;
        BOOST_CHECK(!chainstate.FlushStateToDisk(flush_state, FlushStateMode::FORCE_FLUSH));
        BOOST_CHECK_EQUAL(m_node.exit_status.load(), EXIT_FAILURE);
    }

    // Clear hook and stop async writer cleanly.
    chainman.m_blockman.SetUndoWriteFaultHookForTest({});
    chainman.m_blockman.StopAsyncUndoWriter();
    m_node.exit_status.store(EXIT_SUCCESS);
}

BOOST_AUTO_TEST_CASE(ibd_window_crash_before_undo_inline_path)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& chainstate = chainman.ActiveChainstate();
    const CScript coinbase_script = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    m_node.notifications->m_shutdown_on_fatal_error = false;

    // Inline post-IBD WriteBlockUndo path must also trigger fault hook and latch storage I/O error.
    chainman.m_cached_is_ibd.store(false, std::memory_order_relaxed);
    CBlockIndex* prev = WITH_LOCK(::cs_main, return chainstate.m_chain.Tip());
    CBlock blk101 = BuildBlockOnIndex(chainman, prev, {}, coinbase_script);
    StoreBlockForParallelIBD(chainman, blk101);

    std::atomic<int> inline_hook_hits{0};
    chainman.m_blockman.SetUndoWriteFaultHookForTest([&](const uint256& hash, int height) {
        if (height == 101 && hash == blk101.GetHash()) {
            ++inline_hook_hits;
            return true;
        }
        return false;
    });

    BlockValidationState state;
    BOOST_CHECK(!chainstate.ActivateBestChain(state));
    BOOST_CHECK_EQUAL(inline_hook_hits.load(), 1);
    BOOST_CHECK(chainman.m_blockman.HasStorageIOError());

    {
        LOCK(::cs_main);
        BlockValidationState flush_state;
        BOOST_CHECK(!chainstate.FlushStateToDisk(flush_state, FlushStateMode::FORCE_FLUSH));
    }

    chainman.m_blockman.SetUndoWriteFaultHookForTest({});
}

BOOST_AUTO_TEST_CASE(ibd_exit_resource_teardown)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& chainstate = chainman.ActiveChainstate();
    const CScript coinbase_script = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    chainman.m_cached_is_ibd.store(true, std::memory_order_relaxed);

    // Populate handoff cache and start the lazy async undo writer.
    CBlockIndex* prev = WITH_LOCK(::cs_main, return chainstate.m_chain.Tip());
    CBlock blk101 = BuildBlockOnIndex(chainman, prev, {}, coinbase_script);
    CBlockIndex* idx101 = StoreBlockForParallelIBD(chainman, blk101);
    auto pblk = std::make_shared<const CBlock>(blk101);

    chainman.m_handoff_cache.Put(blk101.GetHash(), pblk, 16_MiB);
    BOOST_CHECK_EQUAL(chainman.m_handoff_cache.Size(), 1U);
    BOOST_CHECK_GT(chainman.m_ibd_budget->handoff_bytes.load(), 0U);

    {
        LOCK(::cs_main);
        CBlockUndo empty_undo;
        auto serialized = chainman.m_blockman.SerializeBlockUndoPayload(empty_undo, prev->GetBlockHash());
        BlockValidationState state;
        BOOST_REQUIRE(chainman.m_blockman.EnqueueBlockUndoWrite(std::move(serialized), state, *idx101));
    }
    BOOST_CHECK(chainman.m_blockman.IsAsyncUndoWriterRunning());

    std::atomic<int> exit_cb_calls{0};
    auto exit_cb_reg = chainman.RegisterIBDExitCallback([&]() { ++exit_cb_calls; });

    // While still in IBD, teardown must be a no-op.
    chainman.MaybeTeardownIBDResources();
    BOOST_CHECK_EQUAL(exit_cb_calls.load(), 0);
    BOOST_CHECK(chainman.m_blockman.IsAsyncUndoWriterRunning());
    BOOST_CHECK_EQUAL(chainman.m_handoff_cache.Size(), 1U);

    // Once IBD exits, teardown settles & joins async undo writer, clears handoff cache,
    // resizes validation pool back to -prevoutfetchthreads, and fires the exit callback.
    chainman.m_cached_is_ibd.store(false, std::memory_order_relaxed);
    chainman.MaybeTeardownIBDResources();

    BOOST_CHECK(!chainman.m_blockman.IsAsyncUndoWriterRunning());
    BOOST_CHECK_EQUAL(chainman.m_handoff_cache.Size(), 0U);
    BOOST_CHECK_EQUAL(chainman.m_ibd_budget->handoff_bytes.load(), 0U);
    BOOST_CHECK_EQUAL(chainman.GetValidationPool()->WorkersCount(), static_cast<size_t>(chainman.m_options.prevoutfetch_threads_num));
    BOOST_CHECK_EQUAL(exit_cb_calls.load(), 1);
    BOOST_CHECK(!chainman.HasActiveIBDResources());

    // Subsequent calls are idempotent no-ops.
    chainman.MaybeTeardownIBDResources();
    BOOST_CHECK_EQUAL(exit_cb_calls.load(), 1);
    exit_cb_reg.Reset();
}

BOOST_AUTO_TEST_CASE(ibd_window_parallel_vs_serial_utxo_equivalence)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& chainstate = chainman.ActiveChainstate();
    const CScript coinbase_script = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    const CScript anyone_script = CScript() << OP_TRUE;

    // Build 6 blocks above height 100 mixing coinbase spends, intra-block chains,
    // cross-block dependencies, and multiple outputs per transaction.
    std::vector<CBlock> blocks;
    std::vector<CBlockIndex*> indices;
    std::vector<COutPoint> tracked_outpoints;

    CTransactionRef cb1 = m_coinbase_txns[0];
    CTransactionRef cb2 = m_coinbase_txns[1];
    tracked_outpoints.emplace_back(cb1->GetHash(), 0);
    tracked_outpoints.emplace_back(cb2->GetHash(), 0);

    auto [tx1, _f1] = CreateValidTransaction(
        {cb1}, {COutPoint{cb1->GetHash(), 0}}, /*input_height=*/1,
        {coinbaseKey}, {CTxOut{20 * COIN, anyone_script}, CTxOut{29 * COIN, anyone_script}},
        /*feerate=*/std::nullopt, /*fee_output=*/std::nullopt);
    CTransactionRef tx1_ref = MakeTransactionRef(tx1);
    const COutPoint u1a{tx1_ref->GetHash(), 0};
    const COutPoint u1b{tx1_ref->GetHash(), 1};
    tracked_outpoints.push_back(u1a);
    tracked_outpoints.push_back(u1b);

    // Intra-block spend of u1a in Block 101
    CMutableTransaction tx2;
    tx2.vin.emplace_back(u1a);
    tx2.vout.emplace_back(19 * COIN, anyone_script);
    CTransactionRef tx2_ref = MakeTransactionRef(tx2);
    const COutPoint u2{tx2_ref->GetHash(), 0};
    tracked_outpoints.push_back(u2);

    // Block 102 spends u2 (cross-block from 101) and cb2 (from tip cache)
    auto [tx3, _f3] = CreateValidTransaction(
        {cb2}, {COutPoint{cb2->GetHash(), 0}}, /*input_height=*/2,
        {coinbaseKey}, {CTxOut{49 * COIN, anyone_script}},
        /*feerate=*/std::nullopt, /*fee_output=*/std::nullopt);
    CTransactionRef tx3_ref = MakeTransactionRef(tx3);
    const COutPoint u3{tx3_ref->GetHash(), 0};
    tracked_outpoints.push_back(u3);

    CMutableTransaction tx4;
    tx4.vin.emplace_back(u2);
    tx4.vout.emplace_back(18 * COIN, anyone_script);
    CTransactionRef tx4_ref = MakeTransactionRef(tx4);
    const COutPoint u4{tx4_ref->GetHash(), 0};
    tracked_outpoints.push_back(u4);

    // Block 103..106 chain spends across window
    CMutableTransaction tx5;
    tx5.vin.emplace_back(u4);
    tx5.vin.emplace_back(u1b);
    tx5.vout.emplace_back(46 * COIN, anyone_script);
    CTransactionRef tx5_ref = MakeTransactionRef(tx5);
    const COutPoint u5{tx5_ref->GetHash(), 0};
    tracked_outpoints.push_back(u5);

    CMutableTransaction tx6;
    tx6.vin.emplace_back(u5);
    tx6.vout.emplace_back(45 * COIN, anyone_script);
    CTransactionRef tx6_ref = MakeTransactionRef(tx6);
    const COutPoint u6{tx6_ref->GetHash(), 0};
    tracked_outpoints.push_back(u6);

    CMutableTransaction tx7;
    tx7.vin.emplace_back(u6);
    tx7.vin.emplace_back(u3);
    tx7.vout.emplace_back(93 * COIN, anyone_script);
    CTransactionRef tx7_ref = MakeTransactionRef(tx7);
    const COutPoint u7{tx7_ref->GetHash(), 0};
    tracked_outpoints.push_back(u7);

    CMutableTransaction tx8;
    tx8.vin.emplace_back(u7);
    tx8.vout.emplace_back(92 * COIN, anyone_script);
    CTransactionRef tx8_ref = MakeTransactionRef(tx8);
    const COutPoint u8{tx8_ref->GetHash(), 0};
    tracked_outpoints.push_back(u8);

    const std::vector<std::vector<CMutableTransaction>> per_block_txs{
        {tx1, tx2},
        {tx3, tx4},
        {tx5},
        {tx6},
        {tx7},
        {tx8},
    };

    CBlockIndex* prev = WITH_LOCK(::cs_main, return chainstate.m_chain.Tip());
    for (const auto& txs : per_block_txs) {
        CBlock blk = BuildBlockOnIndex(chainman, prev, txs, coinbase_script);
        tracked_outpoints.emplace_back(blk.vtx[0]->GetHash(), 0);
        CBlockIndex* idx = StoreBlockForParallelIBD(chainman, blk);
        blocks.push_back(std::move(blk));
        indices.push_back(idx);
        prev = idx;
    }

    // Reference: connect all 6 blocks serially onto a temporary CCoinsViewCache overlay
    // without mutating CoinsTip().
    std::vector<std::optional<Coin>> serial_expected;
    uint256 serial_best_block;
    {
        LOCK(::cs_main);
        CCoinsViewCache serial_view{&chainstate.CoinsTip()};
        for (size_t i = 0; i < blocks.size(); ++i) {
            BlockValidationState state;
            BOOST_REQUIRE(chainstate.ConnectBlock(blocks[i], state, indices[i], serial_view, /*fJustCheck=*/false));
            serial_view.SetBestBlock(indices[i]->GetBlockHash());
        }
        serial_best_block = serial_view.GetBestBlock();
        serial_expected.reserve(tracked_outpoints.size());
        for (const auto& op : tracked_outpoints) {
            serial_expected.push_back(serial_view.GetCoin(op));
        }
    }

    // Parallel: drive ParallelIBDWindow directly with width=8 (runs regardless of host CPU core count!)
    // against a fresh ShardedCoinsView backed by chainstate.CoinsTip(), exercising all 4 resolution layers:
    // intra-block, WindowOutputTable, ShardedCoinsView hit (cb1 pre-cached), and base-view fallback (cb2).
    const CCoinsView* coins_base{nullptr};
    uint256 base_tip_hash;
    {
        LOCK(::cs_main);
        coins_base = &chainstate.CoinsTip();
        base_tip_hash = chainstate.m_chain.Tip()->GetBlockHash();
    }
    ShardedCoinsView parallel_coins_tip{const_cast<CCoinsView*>(coins_base)};
    parallel_coins_tip.SetBestBlock(base_tip_hash);
    BOOST_REQUIRE(parallel_coins_tip.GetCoin(COutPoint{cb1->GetHash(), 0}).has_value());

    ParallelIBDWindow window{chainman, parallel_coins_tip, *coins_base, /*window_width=*/8, /*coinstip_budget_bytes=*/64_MiB};
    {
        std::vector<std::shared_ptr<ParallelIBDWindow::Slot>> slots;
        {
            LOCK(::cs_main);
            for (CBlockIndex* idx : indices) {
                auto slot = std::make_shared<ParallelIBDWindow::Slot>();
                slot->pindex = idx;
                slot->height = idx->nHeight;
                slot->block_hash = idx->GetBlockHash();
                slot->prev_block_hash = idx->pprev->GetBlockHash();
                slot->disk_pos = idx->GetBlockPos();
                slot->script_check_reason = "test";
                slot->needs_undo = true;
                slot->val_ctx = std::make_unique<BlockValidationContext>(BlockValidationContext{
                    .pindex = idx,
                    .consensus_params = &chainman.GetConsensus(),
                    .script_flags = GetBlockScriptFlags(*idx, chainman),
                    .locktime_flags = 0,
                    .enforce_bip30 = ShouldEnforceBIP30(*idx, chainman.GetConsensus()),
                    .script_checks = true,
                    .run_check_block = true,
                    .check_pow_and_merkle = true,
                    .build_undo = true,
                    .cache_script_results = false,
                    .validation_cache = &chainman.m_validation_cache,
                    .check_queue = nullptr,
                });
                slots.push_back(std::move(slot));
            }
        }
        window.ScheduleSlots(std::move(slots));
    }

    // Wait for all 6 slots to reach READY and commit them as one batch.
    std::vector<std::shared_ptr<ParallelIBDWindow::Slot>> ready;
    while (ready.size() < indices.size()) {
        BOOST_REQUIRE(window.WaitForFrontReady({}));
        BOOST_REQUIRE(!window.IsFrontFailed());
        ready = window.PeekReadyPrefix(indices.size());
        if (ready.size() < indices.size()) std::this_thread::yield();
    }

    {
        LOCK(::cs_main);
        std::vector<ShardedCoinsView::ShardedBlockCommit> commits;
        commits.reserve(ready.size());
        for (const auto& s : ready) {
            commits.push_back(ShardedCoinsView::ShardedBlockCommit{
                .meta = {
                    .block_hash = s->block_hash,
                    .validated_flush_epoch = s->validated_flush_epoch,
                    .height = static_cast<uint32_t>(s->height),
                    .enforce_bip30 = s->val_ctx->enforce_bip30,
                },
                .spends = s->spends,
                .creates = s->creates,
                .spend_offsets = &s->spend_shard_offsets,
                .create_offsets = &s->create_shard_offsets,
            });
        }
        BOOST_CHECK_EQUAL(parallel_coins_tip.CommitBlockBatch(commits, &*chainman.GetValidationPool()), indices.size());
        BOOST_CHECK(parallel_coins_tip.GetBestBlock() == serial_best_block);
        for (size_t i = 0; i < tracked_outpoints.size(); ++i) {
            auto par_coin = parallel_coins_tip.GetCoin(tracked_outpoints[i]);
            BOOST_CHECK_EQUAL(par_coin.has_value(), serial_expected[i].has_value());
            if (par_coin && serial_expected[i]) {
                BOOST_CHECK(par_coin->out == serial_expected[i]->out);
                BOOST_CHECK_EQUAL(par_coin->nHeight, serial_expected[i]->nHeight);
                BOOST_CHECK_EQUAL(par_coin->fCoinBase, serial_expected[i]->fCoinBase);
            }
        }
        parallel_coins_tip.SanityCheck();
    }
    window.ReleaseCommittedPrefix(ready.size(), indices.back()->nHeight);
    window.DrainOffCsMain();
}

BOOST_AUTO_TEST_CASE(window_output_table_insert_lookup_bip30_and_ring_wrap)
{
    IBDMemoryBudget budget;
    WindowOutputTable table{budget, WindowOutputTable::MIN_RING_SIZE};

    auto make_block_with_tx = [](int height, uint32_t salt, CAmount val) {
        auto blk = std::make_shared<CBlock>();
        blk->nNonce = static_cast<uint32_t>(height * 1000 + salt);
        CMutableTransaction cb;
        cb.vin.resize(1);
        cb.vin[0].scriptSig = CScript() << height << salt;
        cb.vout.emplace_back(50 * COIN, CScript() << OP_TRUE);
        CMutableTransaction tx;
        tx.nLockTime = static_cast<uint32_t>(height * 1000 + salt);
        tx.vout.emplace_back(val, CScript() << OP_TRUE);
        tx.vout.emplace_back(val + 1, CScript() << OP_RETURN << std::vector<unsigned char>{0x01});
        tx.vout.emplace_back(val + 2, CScript() << OP_TRUE);
        blk->vtx = {MakeTransactionRef(std::move(cb)), MakeTransactionRef(std::move(tx))};
        return blk;
    };

    // Insert 4 blocks at heights 10..13 and verify strict height visibility.
    std::vector<std::shared_ptr<const CBlock>> blks;
    for (int h = 10; h <= 13; ++h) {
        auto b = make_block_with_tx(h, 1, 1000 + h);
        table.InsertBlockOutputs(h, b);
        blks.push_back(std::move(b));
    }
    BOOST_CHECK_GT(budget.window_outputs_bytes.load(), 0U);

    // Lookup from consumer height 12: sees heights 10 and 11, NOT 12 or 13, and skips out-of-range vout / OP_RETURN.
    const std::vector<COutPoint> queries{
        COutPoint{blks[0]->vtx[1]->GetHash(), 0}, // h=10, visible
        COutPoint{blks[0]->vtx[1]->GetHash(), 1}, // h=10, OP_RETURN -> unspendable
        COutPoint{blks[1]->vtx[1]->GetHash(), 2}, // h=11, visible
        COutPoint{blks[1]->vtx[1]->GetHash(), 9}, // h=11, out-of-range vout
        COutPoint{blks[2]->vtx[1]->GetHash(), 0}, // h=12, same height -> invisible
        COutPoint{blks[3]->vtx[1]->GetHash(), 0}, // h=13, higher -> invisible
    };
    std::vector<uint64_t> tags;
    tags.reserve(queries.size());
    for (const auto& q : queries) tags.push_back(table.HashTxidTag(q.hash));

    for (bool use_precomputed : {false, true}) {
        std::vector<std::optional<Coin>> res(queries.size());
        table.LookupBatch(12, queries, res, use_precomputed ? std::span<const uint64_t>{tags} : std::span<const uint64_t>{});
        BOOST_REQUIRE(res[0].has_value());
        BOOST_CHECK_EQUAL(res[0]->out.nValue, 1010);
        BOOST_CHECK_EQUAL(res[0]->nHeight, 10U);
        BOOST_CHECK(!res[0]->fCoinBase);
        BOOST_CHECK(!res[1].has_value());
        BOOST_REQUIRE(res[2].has_value());
        BOOST_CHECK_EQUAL(res[2]->out.nValue, 1013);
        BOOST_CHECK_EQUAL(res[2]->nHeight, 11U);
        BOOST_CHECK(!res[3].has_value());
        BOOST_CHECK(!res[4].has_value());
        BOOST_CHECK(!res[5].has_value());
    }

    // BIP30 conflict detection against earlier uncommitted block (h=10), ignoring same/higher heights.
    {
        CBlock dup_block;
        dup_block.vtx = {blks[0]->vtx[1]}; // collides with tx at height 10
        auto conflict_h11 = table.FindBIP30ConflictBelowHeight(11, dup_block);
        BOOST_REQUIRE(conflict_h11.has_value());
        BOOST_CHECK(conflict_h11->hash == blks[0]->vtx[1]->GetHash());
        BOOST_CHECK(!table.FindBIP30ConflictBelowHeight(10, dup_block).has_value());
    }

    // Release heights 10..11: consumer at height 12 no longer sees height 10 or 11 (published watermark advanced).
    const std::array<int, 2> released{10, 11};
    table.ReleaseHeights(/*committed_height=*/11, released);
    {
        std::vector<std::optional<Coin>> res(queries.size());
        table.LookupBatch(12, queries, res);
        BOOST_CHECK(!res[0].has_value());
        BOOST_CHECK(!res[2].has_value());
        CBlock dup_block;
        dup_block.vtx = {blks[0]->vtx[1]};
        BOOST_CHECK(!table.FindBIP30ConflictBelowHeight(12, dup_block).has_value());
    }
    table.Sweep();

    // Release remaining blocks and stress ring wrap-around past MIN_RING_SIZE (256 slots) over 320 heights.
    const std::array<int, 2> released2{12, 13};
    table.ReleaseHeights(/*committed_height=*/13, released2);
    table.Sweep();

    for (int h = 14; h < 340; ++h) {
        auto b = make_block_with_tx(h, 2, 5000 + h);
        table.InsertBlockOutputs(h, b);
        std::array<COutPoint, 1> q{COutPoint{b->vtx[1]->GetHash(), 0}};
        std::array<std::optional<Coin>, 1> r;
        table.LookupBatch(h + 1, q, r);
        BOOST_REQUIRE(r[0].has_value());
        BOOST_CHECK_EQUAL(r[0]->out.nValue, 5000 + h);
        const std::array<int, 1> rel{h};
        table.ReleaseHeights(h, rel);
        table.Sweep();
    }

    table.Clear();
    BOOST_CHECK_EQUAL(budget.window_outputs_bytes.load(), 0U);
}

BOOST_AUTO_TEST_CASE(ibd_window_invalid_script_and_cancel_drain)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& chainstate = chainman.ActiveChainstate();
    const CScript coinbase_script = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    const CScript fail_script = CScript() << OP_FALSE;

    // Create Block 101 paying to OP_FALSE, and Block 102 spending that OP_FALSE output (invalid script!).
    CTransactionRef cb1 = m_coinbase_txns[0];
    auto [tx1, _f1] = CreateValidTransaction(
        {cb1}, {COutPoint{cb1->GetHash(), 0}}, /*input_height=*/1,
        {coinbaseKey}, {CTxOut{49 * COIN, fail_script}},
        /*feerate=*/std::nullopt, /*fee_output=*/std::nullopt);
    CTransactionRef tx1_ref = MakeTransactionRef(tx1);
    const COutPoint u_fail{tx1_ref->GetHash(), 0};

    CMutableTransaction bad_script_tx;
    bad_script_tx.vin.emplace_back(u_fail);
    bad_script_tx.vout.emplace_back(48 * COIN, CScript() << OP_TRUE);

    CBlockIndex* prev = WITH_LOCK(::cs_main, return chainstate.m_chain.Tip());
    CBlock blk101 = BuildBlockOnIndex(chainman, prev, {tx1}, coinbase_script);
    CBlockIndex* idx101 = StoreBlockForParallelIBD(chainman, blk101);
    CBlock blk102 = BuildBlockOnIndex(chainman, idx101, {bad_script_tx}, coinbase_script);
    CBlockIndex* idx102 = StoreBlockForParallelIBD(chainman, blk102);

    // 1. Verify ParallelIBDWindow rejects invalid script when script_checks=true, and mid-flight cancel drains cleanly.
    const CCoinsView* coins_base{nullptr};
    uint256 base_tip_hash;
    {
        LOCK(::cs_main);
        coins_base = &chainstate.CoinsTip();
        base_tip_hash = chainstate.m_chain.Tip()->GetBlockHash();
    }
    ShardedCoinsView tmp_tip{const_cast<CCoinsView*>(coins_base)};
    tmp_tip.SetBestBlock(base_tip_hash);
    {
        ParallelIBDWindow window{chainman, tmp_tip, *coins_base, /*window_width=*/4, /*coinstip_budget_bytes=*/64_MiB};
        std::vector<std::shared_ptr<ParallelIBDWindow::Slot>> slots;
        {
            LOCK(::cs_main);
            for (CBlockIndex* idx : {idx101, idx102}) {
                auto slot = std::make_shared<ParallelIBDWindow::Slot>();
                slot->pindex = idx;
                slot->height = idx->nHeight;
                slot->block_hash = idx->GetBlockHash();
                slot->prev_block_hash = idx->pprev->GetBlockHash();
                slot->disk_pos = idx->GetBlockPos();
                slot->script_check_reason = "test";
                slot->needs_undo = true;
                slot->val_ctx = std::make_unique<BlockValidationContext>(BlockValidationContext{
                    .pindex = idx,
                    .consensus_params = &chainman.GetConsensus(),
                    .script_flags = GetBlockScriptFlags(*idx, chainman),
                    .locktime_flags = 0,
                    .enforce_bip30 = ShouldEnforceBIP30(*idx, chainman.GetConsensus()),
                    .script_checks = true,
                    .run_check_block = true,
                    .check_pow_and_merkle = true,
                    .build_undo = true,
                    .cache_script_results = false,
                    .validation_cache = &chainman.m_validation_cache,
                    .check_queue = nullptr,
                });
                slots.push_back(std::move(slot));
            }
        }
        window.ScheduleSlots(std::move(slots));
        BOOST_REQUIRE(window.WaitForFrontReady({}));
        // Slot 101 is READY, slot 102 must NOT enter ready prefix (invalid script fails validation).
        auto ready = window.PeekReadyPrefix(2);
        BOOST_CHECK_EQUAL(ready.size(), 1U);
        BOOST_CHECK_EQUAL(ready[0]->height, 101);
        window.DrainOffCsMain();
    }

    // 2. Full ActivateBestChain in IBD: Block 101 connects, Block 102 fails script verification and is marked BLOCK_FAILED_VALID.
    chainman.m_cached_is_ibd.store(true, std::memory_order_relaxed);
    SetMockTime(std::chrono::seconds{blk102.GetBlockTime() + 48 * 3600});
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.ActivateBestChain(state));
    {
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(chainstate.m_chain.Height(), 101);
        BOOST_CHECK_EQUAL(chainstate.m_chain.Tip(), idx101);
        BOOST_CHECK((idx102->nStatus & BLOCK_FAILED_VALID) != 0);
        chainstate.CoinsTip().SanityCheck();
    }
}

BOOST_AUTO_TEST_CASE(window_output_table_duplicate_txid_ordering)
{
    IBDMemoryBudget budget;
    WindowOutputTable table{budget, WindowOutputTable::MIN_RING_SIZE};

    // Build a shared coinbase transaction reused at heights a < d (BIP30-style duplicate txid).
    CMutableTransaction dup_cb;
    dup_cb.vin.resize(1);
    dup_cb.vin[0].prevout.SetNull();
    dup_cb.vin[0].scriptSig = CScript() << 1 << OP_0;
    dup_cb.vout.emplace_back(50 * COIN, CScript() << OP_TRUE);
    CTransactionRef dup_cb_ref = MakeTransactionRef(std::move(dup_cb));
    const Txid dup_txid = dup_cb_ref->GetHash();

    // Block at height a has 3 transactions (dup_cb + 2 unique txs); block at height d has only dup_cb.
    // The distinct entry counts let LiveEntryBytes() prove FIFO erases a first, leaving d.
    auto make_extra_tx = [](uint32_t locktime) {
        CMutableTransaction tx;
        tx.nLockTime = locktime;
        tx.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
        return MakeTransactionRef(std::move(tx));
    };
    const int height_a = 10;
    const int height_d = 20;

    auto blk_a = std::make_shared<CBlock>();
    blk_a->vtx = {dup_cb_ref, make_extra_tx(1001), make_extra_tx(1002)};
    const Txid extra_txid = blk_a->vtx[1]->GetHash();

    auto blk_d = std::make_shared<CBlock>();
    blk_d->vtx = {dup_cb_ref};

    table.InsertBlockOutputs(height_a, blk_a);
    const size_t bytes_a = table.LiveEntryBytes();
    BOOST_REQUIRE_GT(bytes_a, 0U);

    table.InsertBlockOutputs(height_d, blk_d);
    const size_t bytes_ad = table.LiveEntryBytes();
    BOOST_REQUIRE_GT(bytes_ad, bytes_a);
    const size_t bytes_d = bytes_ad - bytes_a;
    BOOST_REQUIRE_GT(bytes_a, bytes_d);

    // Combined lookup before release must return the newer height d (both n==1 and multi-op paths).
    const std::array<COutPoint, 1> single_q{COutPoint{dup_txid, 0}};
    const std::array<COutPoint, 2> multi_q{COutPoint{dup_txid, 0}, COutPoint{extra_txid, 0}};
    {
        std::array<std::optional<Coin>, 1> res1;
        table.LookupBatch(/*consumer_height=*/25, single_q, res1);
        BOOST_REQUIRE(res1[0].has_value());
        BOOST_CHECK_EQUAL(res1[0]->nHeight, static_cast<uint32_t>(height_d));
        BOOST_CHECK(res1[0]->fCoinBase);

        std::array<std::optional<Coin>, 2> res2;
        table.LookupBatch(/*consumer_height=*/25, multi_q, res2);
        BOOST_REQUIRE(res2[0].has_value());
        BOOST_CHECK_EQUAL(res2[0]->nHeight, static_cast<uint32_t>(height_d));
        BOOST_REQUIRE(res2[1].has_value());
        BOOST_CHECK_EQUAL(res2[1]->nHeight, static_cast<uint32_t>(height_a));
    }

    // Release {a, d} at committed_height = d and call EraseOne() once:
    // FIFO must erase a (bytes_a) first, leaving d (bytes_d).
    const std::array<int, 2> rel_fifo{height_a, height_d};
    table.ReleaseHeights(height_d, rel_fifo);
    BOOST_CHECK(table.EraseOne());
    BOOST_CHECK_EQUAL(table.LiveEntryBytes(), bytes_d);

    // LookupBatch must reject <= published inside the shard lock.
    {
        std::array<std::optional<Coin>, 1> res1;
        table.LookupBatch(/*consumer_height=*/25, single_q, res1);
        BOOST_CHECK(!res1[0].has_value());
        BOOST_CHECK(!table.FindBIP30ConflictBelowHeight(25, *blk_d).has_value());
    }
    BOOST_CHECK(table.EraseOne());
    BOOST_CHECK_EQUAL(table.LiveEntryBytes(), 0U);
    BOOST_CHECK(!table.EraseOne());

    // Even if d is erased while stale a still sits in the shard (e.g., concurrent worker sweep),
    // LookupBatch and FindBIP30ConflictBelowHeight re-read m_published under each shard lock
    // and never return stale a.
    table.Clear();
    table.InsertBlockOutputs(height_a, blk_a);
    table.InsertBlockOutputs(height_d, blk_d);
    const std::array<int, 2> rel_reverse{height_d, height_a};
    table.ReleaseHeights(height_d, rel_reverse);
    BOOST_CHECK(table.EraseOne()); // removes d, leaving stale a in shards
    BOOST_CHECK_EQUAL(table.LiveEntryBytes(), bytes_a);
    {
        std::array<std::optional<Coin>, 1> res1;
        table.LookupBatch(/*consumer_height=*/25, single_q, res1);
        BOOST_CHECK(!res1[0].has_value());

        std::array<std::optional<Coin>, 2> res2;
        table.LookupBatch(/*consumer_height=*/25, multi_q, res2);
        BOOST_CHECK(!res2[0].has_value());
        BOOST_CHECK(!res2[1].has_value());

        BOOST_CHECK(!table.FindBIP30ConflictBelowHeight(25, *blk_d).has_value());
    }
    table.Sweep();
    BOOST_CHECK_EQUAL(table.LiveEntryBytes(), 0U);
    table.Clear();
    BOOST_CHECK_EQUAL(budget.window_outputs_bytes.load(), 0U);
}

BOOST_AUTO_TEST_CASE(ibd_window_admission_respects_budget_before_disk_load)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& chainstate = chainman.ActiveChainstate();
    const CScript coinbase_script = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    const CCoinsView* coins_base{nullptr};
    uint256 base_tip_hash;
    CBlockIndex* prev{nullptr};
    {
        LOCK(::cs_main);
        coins_base = &chainstate.CoinsTip();
        prev = chainstate.m_chain.Tip();
        base_tip_hash = prev->GetBlockHash();
    }
    ShardedCoinsView tmp_tip{const_cast<CCoinsView*>(coins_base)};
    tmp_tip.SetBestBlock(base_tip_hash);

    // With coinstip_budget_bytes = 30 MiB, max_window_blocks_bytes is 3 MiB.
    // Even with window_width = 64, MaxSchedulableSlots() must bound admission by estimated block
    // bytes (~1 MiB initial average) rather than returning 64 before disk reads complete.
    {
        ParallelIBDWindow window{chainman, tmp_tip, *coins_base, /*window_width=*/64, /*coinstip_budget_bytes=*/30_MiB};
        const size_t initial_schedulable = window.MaxSchedulableSlots();
        BOOST_CHECK_GE(initial_schedulable, 1U);
        BOOST_CHECK_LT(initial_schedulable, 64U);
        BOOST_CHECK_EQUAL(initial_schedulable, 3U);

        // Schedule 3 real blocks and verify admission stays bounded well below window_width = 64
        // both immediately while loading and after loading completes (EWMA clamped >= 256 KiB).
        std::vector<CBlockIndex*> indices;
        for (int i = 0; i < 3; ++i) {
            CBlock blk = BuildBlockOnIndex(chainman, prev, {}, coinbase_script);
            prev = StoreBlockForParallelIBD(chainman, blk);
            indices.push_back(prev);
        }
        std::vector<std::shared_ptr<ParallelIBDWindow::Slot>> slots;
        {
            LOCK(::cs_main);
            for (CBlockIndex* idx : indices) {
                auto slot = std::make_shared<ParallelIBDWindow::Slot>();
                slot->pindex = idx;
                slot->height = idx->nHeight;
                slot->block_hash = idx->GetBlockHash();
                slot->prev_block_hash = idx->pprev->GetBlockHash();
                slot->disk_pos = idx->GetBlockPos();
                slot->script_check_reason = "test";
                slot->needs_undo = true;
                slot->val_ctx = std::make_unique<BlockValidationContext>(BlockValidationContext{
                    .pindex = idx,
                    .consensus_params = &chainman.GetConsensus(),
                    .script_flags = GetBlockScriptFlags(*idx, chainman),
                    .locktime_flags = 0,
                    .enforce_bip30 = ShouldEnforceBIP30(*idx, chainman.GetConsensus()),
                    .script_checks = true,
                    .run_check_block = true,
                    .check_pow_and_merkle = true,
                    .build_undo = true,
                    .cache_script_results = false,
                    .validation_cache = &chainman.m_validation_cache,
                    .check_queue = nullptr,
                });
                slots.push_back(std::move(slot));
            }
        }
        window.ScheduleSlots(std::move(slots));
        BOOST_CHECK_LT(window.MaxSchedulableSlots(), 64U);
        BOOST_REQUIRE(window.WaitForFrontReady({}));
        BOOST_CHECK(!window.IsFrontFailed());
        BOOST_CHECK_LT(window.MaxSchedulableSlots(), 64U);
        window.DrainOffCsMain();
    }

    // Even with a tiny budget where max_window_blocks_bytes < initial 1 MiB estimate,
    // an empty window still allows 1 slot for forward progress (never 64, never 0).
    {
        ParallelIBDWindow tiny_window{chainman, tmp_tip, *coins_base, /*window_width=*/64, /*coinstip_budget_bytes=*/5_MiB};
        BOOST_CHECK_EQUAL(tiny_window.MaxSchedulableSlots(), 1U);
    }
}

BOOST_AUTO_TEST_SUITE_END()
