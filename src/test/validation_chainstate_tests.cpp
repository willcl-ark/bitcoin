// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/validation.h>
#include <node/blockstorage.h>
#include <node/kernel_notifications.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <script/solver.h>
#include <sync.h>
#include <test/util/chainstate.h>
#include <test/util/coins.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/check.h>
#include <coinsstore.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class CTxMemPool;

BOOST_FIXTURE_TEST_SUITE(validation_chainstate_tests, ChainTestingSetup)

//! Test resizing coins-related Chainstate caches during runtime.
//!
BOOST_AUTO_TEST_CASE(validation_chainstate_resize_caches)
{
    ChainstateManager& manager = *Assert(m_node.chainman);
    CTxMemPool& mempool = *Assert(m_node.mempool);
    Chainstate& c1 = WITH_LOCK(cs_main, return manager.InitializeChainstate(&mempool));
    c1.InitCoinsDB(
        /*cache_size_bytes=*/8_MiB, /*in_memory=*/true, /*should_wipe=*/false);
    WITH_LOCK(::cs_main, c1.InitCoinsCache(8_MiB));
    BOOST_REQUIRE(manager.LoadGenesisBlock()); // Need at least one block loaded to be able to flush caches

    // Add a coin to the in-memory cache, upsize once, then downsize.
    {
        LOCK(::cs_main);
        const auto outpoint = AddTestCoin(m_rng, c1.CoinsTip());

        // Set a meaningless bestblock value in the coinsview cache - otherwise we won't
        // flush during ResizecoinsCaches() and will subsequently hit an assertion.
        c1.CoinsTip().SetBestBlock(m_rng.rand256());

        BOOST_CHECK(c1.CoinsTip().HaveCoinInCache(outpoint));

        c1.ResizeCoinsCaches(
            16_MiB, // upsizing the coinsview cache
            4_MiB // downsizing the coinsdb cache
        );

        // View should still have the coin cached, since we haven't destructed the cache on upsize.
        BOOST_CHECK(c1.CoinsTip().HaveCoinInCache(outpoint));

        c1.ResizeCoinsCaches(
            4_MiB, // downsizing the coinsview cache
            8_MiB // upsizing the coinsdb cache
        );

        // The view cache should be empty since we had to destruct to downsize.
        BOOST_CHECK(!c1.CoinsTip().HaveCoinInCache(outpoint));
    }
}

BOOST_FIXTURE_TEST_CASE(connect_tip_does_not_cache_inputs_on_failed_connect, TestChain100Setup)
{
    Chainstate& chainstate{Assert(m_node.chainman)->ActiveChainstate()};

    COutPoint outpoint;
    {
        LOCK(cs_main);
        outpoint = AddTestCoin(m_rng, chainstate.CoinsTip());
        chainstate.CoinsTip().Flush(/*reallocate_cache=*/false);
    }

    CMutableTransaction tx;
    tx.vin.emplace_back(outpoint);
    tx.vout.emplace_back(MAX_MONEY, CScript{} << OP_TRUE);

    const auto tip{WITH_LOCK(cs_main, return chainstate.m_chain.Tip()->GetBlockHash())};
    const CBlock block{CreateBlock({tx}, CScript{} << OP_TRUE)};
    BOOST_CHECK(Assert(m_node.chainman)->ProcessNewBlock(std::make_shared<CBlock>(block), true, true, nullptr));

    LOCK(cs_main);
    BOOST_CHECK_EQUAL(tip, chainstate.m_chain.Tip()->GetBlockHash()); // block rejected
    BOOST_CHECK(!chainstate.CoinsTip().HaveCoinInCache(outpoint));    // input not cached
}

//! Test UpdateTip behavior for both active and background chainstates.
//!
//! When run on the background chainstate, UpdateTip should do a subset
//! of what it does for the active chainstate.
BOOST_FIXTURE_TEST_CASE(chainstate_update_tip, TestChain100Setup)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    const auto get_notify_tip{[&]() {
        LOCK(m_node.notifications->m_tip_block_mutex);
        BOOST_REQUIRE(m_node.notifications->TipBlock());
        return *m_node.notifications->TipBlock();
    }};
    uint256 curr_tip = get_notify_tip();

    // Mine 10 more blocks, putting at us height 110 where a valid assumeutxo value can
    // be found.
    mineBlocks(10);

    // After adding some blocks to the tip, best block should have changed.
    BOOST_CHECK(get_notify_tip() != curr_tip);

    // Grab block 1 from disk; we'll add it to the background chain later.
    std::shared_ptr<CBlock> pblockone = std::make_shared<CBlock>();
    {
        LOCK(::cs_main);
        chainman.m_blockman.ReadBlock(*pblockone, *chainman.ActiveChain()[1]);
    }

    BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(
        this, NoMalleation, /*reset_chainstate=*/ true));

    // Ensure our active chain is the snapshot chainstate.
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.CurrentChainstate().m_from_snapshot_blockhash));

    curr_tip = get_notify_tip();

    // Mine a new block on top of the activated snapshot chainstate.
    mineBlocks(1);  // Defined in TestChain100Setup.

    // After adding some blocks to the snapshot tip, best block should have changed.
    BOOST_CHECK(get_notify_tip() != curr_tip);

    curr_tip = get_notify_tip();

    Chainstate& background_cs{*Assert(WITH_LOCK(::cs_main, return chainman.HistoricalChainstate()))};

    // Append the first block to the background chain.
    BlockValidationState state;
    CBlockIndex* pindex = nullptr;
    const CChainParams& chainparams = Params();
    bool newblock = false;

    // TODO: much of this is inlined from ProcessNewBlock(); just reuse PNB()
    // once it is changed to support multiple chainstates.
    {
        LOCK(::cs_main);
        bool checked = CheckBlock(*pblockone, state, chainparams.GetConsensus());
        BOOST_CHECK(checked);
        bool accepted = chainman.AcceptBlock(
            pblockone, state, &pindex, true, nullptr, &newblock, true);
        BOOST_CHECK(accepted);
    }

    // UpdateTip is called here
    bool block_added = background_cs.ActivateBestChain(state, pblockone);

    // Ensure tip is as expected
    BOOST_CHECK_EQUAL(background_cs.m_chain.Tip()->GetBlockHash(), pblockone->GetHash());

    // get_notify_tip() should be unchanged after adding a block to the background
    // validation chain.
    BOOST_CHECK(block_added);
    BOOST_CHECK_EQUAL(curr_tip, get_notify_tip());
}

//! A coin created at or below the height the coins database settled at and spent above it, before a run carried
//! it, cancels in the young tier: the database never holds it, although it is alive at that height. A
//! disconnect of the spend restores it. However the write-out runs after that are bounded, the database must
//! then hold the coin, or be settled below its creation, where a replay creates it again.
BOOST_FIXTURE_TEST_CASE(disconnect_restores_a_coin_the_database_never_held, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const CScript script{GetScriptForRawPubKey(coinbaseKey.GetPubKey())};
    // A write-out run bounded at `bound` (unless it must be final), started as FlushStateToDisk() starts one and
    // waited for.
    const auto run_to{[&](int bound) {
        LOCK(::cs_main);
        BlockValidationState run_state;
        BOOST_REQUIRE(chainstate.StartWriteOutRun(run_state, bound, /*final=*/false));
        chainstate.SettleWriteOut();
    }};

    WITH_LOCK(::cs_main, chainstate.ForceFlushStateToDisk());
    const int base{WITH_LOCK(::cs_main, return chainstate.m_chain.Height())};
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.CoinsStoreTip().WrittenHeight()), base);

    // X is created in base+1 and spent in base+5.
    const CMutableTransaction create{CreateValidMempoolTransaction(m_coinbase_txns[0], 0, 1, coinbaseKey, script, 49 * COIN, /*submit=*/false)};
    CreateAndProcessBlock({create}, script);
    const COutPoint coin{create.GetHash(), 0};
    for (int i{0}; i < 3; ++i) CreateAndProcessBlock({}, script);
    const CMutableTransaction spend{CreateValidMempoolTransaction(MakeTransactionRef(create), 0, base + 1, coinbaseKey, script, 48 * COIN, /*submit=*/false)};
    const CBlock spend_block{CreateAndProcessBlock({spend}, script)};
    CreateAndProcessBlock({}, script);

    // A run bounded at base+2: X was created and spent inside the lag, so it cancels; the database settles
    // at base+2 without it.
    run_to(base + 2);
    {
        LOCK(::cs_main);
        BOOST_REQUIRE_EQUAL(chainstate.CoinsStoreTip().WrittenHeight(), base + 2);
        BOOST_REQUIRE(!chainstate.CoinsDB().HaveCoin(coin));
    }

    // The spend is disconnected (the fork is at base+4): X is alive again.
    CBlockIndex* const spend_index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(spend_block.GetHash()))};
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, spend_index));
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainstate.CoinsTip().HaveCoin(coin)));

    // A run bounded between the settled height and the fork.
    run_to(base + 3);
    LOCK(::cs_main);
    chainman.m_blockman.SettleUndoWrites();
    BOOST_CHECK_MESSAGE(chainstate.CoinsDB().HaveCoin(coin) || chainstate.CoinsStoreTip().WrittenHeight() < base + 1,
                        "the database settled at " << chainstate.CoinsStoreTip().WrittenHeight() << " without the restored coin");
}

//! A disconnect writes the coins it restores only on the written head's own chain. With the written head on
//! another branch, a restored coin may come from a block that no chain the database can be replayed to holds:
//! written, it would stay in the database after a crash as a coin nothing created.
BOOST_FIXTURE_TEST_CASE(disconnect_writes_no_coin_of_another_branch, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const CScript script{GetScriptForRawPubKey(coinbaseKey.GetPubKey())};

    // The database is written at the tip, block T, two blocks up so that the fork can spend a coinbase.
    mineBlocks(2);
    WITH_LOCK(::cs_main, chainstate.ForceFlushStateToDisk());
    CBlockIndex* const tip{WITH_LOCK(::cs_main, return chainstate.m_chain.Tip())};
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainstate.CoinsStoreTip().WrittenHead()) == tip->GetBlockHash());

    // A fork from T-1: T' creates a coin Y that T'+1 spends.
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, tip));
    const CMutableTransaction create{CreateValidMempoolTransaction(m_coinbase_txns[0], 0, 1, coinbaseKey, script, 49 * COIN, /*submit=*/false)};
    const CBlock fork1{CreateAndProcessBlock({create}, script)};
    const COutPoint y{create.GetHash(), 0};
    const CMutableTransaction spend{CreateValidMempoolTransaction(MakeTransactionRef(create), 0, tip->nHeight, coinbaseKey, script, 48 * COIN, /*submit=*/false)};
    CreateAndProcessBlock({spend}, script);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), tip->nHeight + 1);
    BOOST_REQUIRE_MESSAGE(WITH_LOCK(::cs_main, return chainstate.CoinsStoreTip().WrittenHead()) == tip->GetBlockHash(),
                          "written at height " << WITH_LOCK(::cs_main, return chainstate.CoinsStoreTip().WrittenHeight()));
    // T'+1 and T' are disconnected: Y comes back with T'+1's disconnect, then goes with T''s.
    CBlockIndex* const fork1_index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(fork1.GetHash()))};
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, fork1_index));
    LOCK(::cs_main);
    chainman.m_blockman.SettleUndoWrites();
    BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(y));
    // The coins the next run writes before its marker: Y, created at the written height but on T''s branch,
    // is not among them.
    const auto restored{chainstate.CoinsStoreTip().TakeRestored()};
    BOOST_CHECK_MESSAGE(std::none_of(restored.begin(), restored.end(), [&](const CoinWrite& write) { return write.outpoint == y; }),
                        "a coin of a branch the written head is not on is to be written");
    BOOST_CHECK(!chainstate.CoinsDB().HaveCoin(y));
}

//! A block index write records a block whose undo write is still queued with the undo data it is getting, so
//! that a write-out run need not wait for the write under cs_main: the run commits the batch only behind the
//! write. The entry itself is left as it is until the write's completion records it.
BOOST_FIXTURE_TEST_CASE(index_batch_records_a_queued_undo_write, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    auto& blockman{static_cast<TestBlockManager&>(chainman.m_blockman)};
    blockman.SettleUndoWrites();
    LOCK(::cs_main);
    CBlockIndex* tip{chainman.ActiveChain().Tip()};
    const uint32_t status{tip->nStatus};
    const uint64_t undo_pos{tip->nUndoPos};
    // The tip as a block just published has it: no undo data recorded, its write claimed and queued.
    tip->nStatus &= ~BLOCK_HAVE_UNDO;
    tip->nUndoPos = 0;
    BlockValidationState state;
    const auto claimed{blockman.ClaimUndoPos(state, *tip, /*undo_size=*/100)};
    BOOST_REQUIRE(claimed);
    blockman.MarkDirty(tip);
    auto batch{blockman.PrepareBlockIndexBatch()};
    BOOST_REQUIRE(blockman.SyncBatchFiles(batch));
    blockman.CommitBlockIndexBatch(batch);

    CDiskBlockIndex disk;
    BOOST_REQUIRE(blockman.m_block_tree_db->Read(std::make_pair(uint8_t{'b'}, tip->GetBlockHash()), disk));
    BOOST_CHECK(disk.nStatus & BLOCK_HAVE_UNDO);
    BOOST_CHECK_EQUAL(disk.nUndoPos, claimed->nPos + node::STORAGE_HEADER_BYTES);
    BOOST_CHECK(!(tip->nStatus & BLOCK_HAVE_UNDO));

    // Put back as it was.
    blockman.UndoWriteSettled(*tip);
    tip->nStatus = status;
    tip->nUndoPos = undo_pos;
    blockman.MarkDirty(tip);
}

//! In a reorganisation the transactions of the disconnected blocks are back in the mempool, and announced,
//! before the blocks of the new branch are announced connected. With `periodic_due` the periodic write-out
//! is due when the reorganisation starts.
static void CheckReorgAnnouncementOrder(TestChain100Setup& setup, bool periodic_due = false)
{
    auto& m_node{setup.m_node};
    auto& m_coinbase_txns{setup.m_coinbase_txns};
    auto& coinbaseKey{setup.coinbaseKey};
    const auto CreateAndProcessBlock{[&](const std::vector<CMutableTransaction>& txns, const CScript& script) { return setup.CreateAndProcessBlock(txns, script); }};
    const auto CreateValidMempoolTransaction{[&](CTransactionRef in, uint32_t vout, int height, CKey key, CScript out, CAmount amount) {
        return setup.CreateValidMempoolTransaction(in, vout, height, key, out, amount);
    }};
    struct Recorder final : CValidationInterface {
        std::vector<std::string> events;
        void TransactionAddedToMempool(const NewMempoolTransactionInfo& tx, uint64_t) override { events.push_back("A " + tx.info.m_tx->GetHash().ToString()); }
        void BlockConnected(const kernel::ChainstateRole&, const std::shared_ptr<const CBlock>& block, const CBlockIndex*) override { events.push_back("C " + block->GetHash().ToString()); }
    };
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const CScript script{CScript() << OP_TRUE};

    // Two empty blocks on top of 100, set aside; then a block 101 that mines two mempool transactions, the
    // second spending the first.
    const CBlock fork1{CreateAndProcessBlock({}, script)};
    const CBlock fork2{CreateAndProcessBlock({}, script)};
    CBlockIndex* const fork1_index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(fork1.GetHash()))};
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, fork1_index));
    const CMutableTransaction tx{CreateValidMempoolTransaction(m_coinbase_txns[0], 0, 1, coinbaseKey, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), 49 * COIN)};
    const CMutableTransaction tx2{CreateValidMempoolTransaction(MakeTransactionRef(tx), 0, 101, coinbaseKey, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), 48 * COIN)};
    const CBlock mined{CreateAndProcessBlock({tx, tx2}, script)};
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()->GetBlockHash()), mined.GetHash());
    BOOST_REQUIRE_EQUAL(WITH_LOCK(m_node.mempool->cs, return m_node.mempool->size()), 0U);

    // The two blocks back: the reorganisation disconnects 101 and connects them.
    auto recorder{std::make_shared<Recorder>()};
    m_node.validation_signals->RegisterSharedValidationInterface(recorder);
    {
        LOCK(::cs_main);
        chainstate.ResetBlockFailureFlags(fork1_index);
        chainman.RecalculateBestHeader();
    }
    // Past the interval of the periodic write-out: the first write-out check that asks for it finds it due.
    if (periodic_due) setup.m_clock += std::chrono::hours{2};
    BOOST_REQUIRE(chainstate.ActivateBestChain(state));
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    m_node.validation_signals->UnregisterSharedValidationInterface(recorder);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()->GetBlockHash()), fork2.GetHash());

    const std::vector<std::string> expected{"A " + CTransaction{tx}.GetHash().ToString(), "A " + CTransaction{tx2}.GetHash().ToString(), "C " + fork1.GetHash().ToString(), "C " + fork2.GetHash().ToString()};
    BOOST_CHECK_EQUAL_COLLECTIONS(recorder->events.begin(), recorder->events.end(), expected.begin(), expected.end());
}

BOOST_FIXTURE_TEST_CASE(reorg_announces_readded_transactions_before_connected_blocks, TestChain100Setup)
{
    CheckReorgAnnouncementOrder(*this);
}

//! The same with a write-out run started at every block, inside the reorganisation's step too: a run's settle
//! of the effects queue must not hand on the connected blocks' signals before the re-added transactions'.
struct WriteOutEveryBlockSetup : TestChain100Setup {
    WriteOutEveryBlockSetup() : TestChain100Setup{ChainType::REGTEST, {.extra_args = {"-test=write_out_every_block"}}} {}
};

//! A block whose write-out fails does not become the tip, and is not announced: the write is made before
//! the tip moves, as it always was.
BOOST_FIXTURE_TEST_CASE(failed_write_out_leaves_the_tip, WriteOutEveryBlockSetup)
{
    struct Recorder final : CValidationInterface {
        std::atomic<int> connected{0};
        std::atomic<int> tips{0};
        void BlockConnected(const kernel::ChainstateRole&, const std::shared_ptr<const CBlock>&, const CBlockIndex*) override { ++connected; }
        void UpdatedBlockTip(const CBlockIndex*, const CBlockIndex*, bool) override { ++tips; }
    };
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    BOOST_REQUIRE(chainman.m_options.test_write_out_every_block);
    const CBlockIndex* const old_tip{WITH_LOCK(::cs_main, return chainstate.m_chain.Tip())};

    // The block is stored, and not connected yet.
    const auto block{std::make_shared<const CBlock>(CreateBlock({}, CScript() << OP_TRUE))};
    BlockValidationState state;
    CBlockIndex* index{nullptr};
    bool new_block{false};
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainman.AcceptBlock(block, state, &index, /*fRequested=*/true, /*dbp=*/nullptr, &new_block, /*min_pow_checked=*/true)));
    BOOST_REQUIRE(new_block && index);

    // Its block file cannot be opened from here on: the run its connection starts fails where it syncs the
    // file.
    const fs::path file{WITH_LOCK(::cs_main, return chainman.m_blockman.GetBlockPosFilename(index->GetBlockPos()))};
    BOOST_REQUIRE(fs::remove(file));
    BOOST_REQUIRE(fs::create_directory(file));

    const auto recorder{std::make_shared<Recorder>()};
    m_node.validation_signals->RegisterSharedValidationInterface(recorder);
    BOOST_CHECK(!chainstate.ActivateBestChain(state, block));
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    m_node.validation_signals->UnregisterSharedValidationInterface(recorder);

    BOOST_CHECK_EQUAL(m_node.exit_status.load(), EXIT_FAILURE);
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()) == old_tip);
    BOOST_CHECK_EQUAL(recorder->connected.load(), 0);
    BOOST_CHECK_EQUAL(recorder->tips.load(), 0);
}

BOOST_FIXTURE_TEST_CASE(reorg_announcement_order_survives_a_run_in_the_step, WriteOutEveryBlockSetup)
{
    BOOST_REQUIRE(m_node.chainman->m_options.test_write_out_every_block);
    CheckReorgAnnouncementOrder(*this);
}

//! The same with the periodic write-out due: mempool acceptance makes no write-out check between two of the
//! transactions a reorganisation adds back, which would settle the effects queue and hand on the connected
//! blocks' signals before the second transaction's.
BOOST_FIXTURE_TEST_CASE(reorg_announcement_order_survives_a_periodic_write, TestChain100Setup)
{
    CheckReorgAnnouncementOrder(*this, /*periodic_due=*/true);
}
BOOST_FIXTURE_TEST_CASE(reorg_announcement_order_survives_a_periodic_write_and_runs, WriteOutEveryBlockSetup)
{
    CheckReorgAnnouncementOrder(*this, /*periodic_due=*/true);
}

//! At the tip, the block that improves the tip and the pass's notifications are one hold of cs_main: a thread
//! that takes cs_main never sees a tip the kernel's tip notification has not named yet.
BOOST_FIXTURE_TEST_CASE(tip_and_its_notification_are_one_hold, TestChain100Setup)
{
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return m_node.chainman->IsInitialBlockDownload()));
    auto& notifications{*m_node.notifications};
    std::atomic<bool> stop{false};
    std::atomic<int> checks{0}, mismatches{0};
    std::thread observer{[&] {
        while (!stop) {
            LOCK(::cs_main);
            const uint256 tip{m_node.chainman->ActiveChain().Tip()->GetBlockHash()};
            const auto notified{WITH_LOCK(notifications.m_tip_block_mutex, return notifications.TipBlock())};
            if (notified && *notified != tip) ++mismatches;
            ++checks;
        }
    }};
    for (int i{0}; i < 200; ++i) CreateAndProcessBlock({}, CScript() << OP_TRUE);
    stop = true;
    observer.join();
    BOOST_CHECK(checks.load() > 0);
    BOOST_CHECK_EQUAL(mismatches.load(), 0);
}

//! A block handed to the connector (ProcessNewBlock() with activate_async) is the tip once
//! WaitForPendingBlocks() returns. A stopped connector takes nothing: the block is stored, nobody waits for
//! it, and whoever activates the chain next connects it.
BOOST_FIXTURE_TEST_CASE(connector_connects_what_it_is_handed, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    BOOST_REQUIRE(!chainman.IsInitialBlockDownload());
    const CScript script{CScript() << OP_TRUE};
    const auto tip{[&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()->GetBlockHash()); }};

    const auto first{std::make_shared<const CBlock>(CreateBlock({}, script))};
    BOOST_REQUIRE(chainman.ProcessNewBlock(first, /*force_processing=*/true, /*min_pow_checked=*/true, nullptr, /*activate_async=*/true));
    chainman.WaitForPendingBlocks(chainman.SyncModeNow());
    BOOST_CHECK_EQUAL(tip(), first->GetHash());

    chainman.StopConnector();
    const auto second{std::make_shared<const CBlock>(CreateBlock({}, script))};
    BOOST_REQUIRE(chainman.ProcessNewBlock(second, /*force_processing=*/true, /*min_pow_checked=*/true, nullptr, /*activate_async=*/true));
    chainman.WaitForPendingBlocks(chainman.SyncModeNow());
    {
        LOCK(::cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(second->GetHash())};
        BOOST_REQUIRE(index);
        BOOST_CHECK(index->nStatus & BLOCK_HAVE_DATA);
    }
    BOOST_CHECK_EQUAL(tip(), first->GetHash());

    BlockValidationState state;
    BOOST_REQUIRE(chainman.ActiveChainstate().ActivateBestChain(state));
    BOOST_CHECK_EQUAL(tip(), second->GetHash());
}

BOOST_AUTO_TEST_SUITE_END()
