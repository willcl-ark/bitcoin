// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <clientversion.h>
#include <consensus/validation.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <node/kernel_notifications.h>
#include <pow.h>
#include <script/solver.h>
#include <primitives/block.h>
#include <undo.h>
#include <util/chaintype.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>
#include <test/util/common.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>

#include <atomic>
#include <thread>
#include <vector>

using kernel::CBlockFileInfo;
using node::STORAGE_HEADER_BYTES;
using node::BlockManager;
using node::KernelNotifications;
using node::MAX_BLOCKFILE_SIZE;

// use BasicTestingSetup here for the data directory configuration, setup, and cleanup
BOOST_FIXTURE_TEST_SUITE(blockmanager_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(blockmanager_find_block_pos)
{
    const auto params {CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    KernelNotifications notifications{Assert(m_node.shutdown_request), m_node.exit_status, *Assert(m_node.warnings)};
    const BlockManager::Options blockman_opts{
        .chainparams = *params,
        .blocks_dir = m_args.GetBlocksDirPath(),
        .notifications = notifications,
        .block_tree_db_params = DBParams{
            .path = m_args.GetDataDirNet() / "blocks" / "index",
            .cache_bytes = 0,
        },
    };
    BlockManager blockman{*Assert(m_node.shutdown_signal), blockman_opts};
    // simulate adding a genesis block normally
    LOCK(::cs_main);
    BOOST_CHECK_EQUAL(blockman.WriteBlock(params->GenesisBlock(), 0).nPos, STORAGE_HEADER_BYTES);
    // simulate what happens during reindex
    // simulate a well-formed genesis block being found at offset 8 in the blk00000.dat file
    // the block is found at offset 8 because there is an 8 byte serialization header
    // consisting of 4 magic bytes + 4 length bytes before each block in a well-formed blk file.
    const FlatFilePos pos{0, STORAGE_HEADER_BYTES};
    blockman.UpdateBlockInfo(params->GenesisBlock(), 0, pos);
    // now simulate what happens after reindex for the first new block processed
    // the actual block contents don't matter, just that it's a block.
    // verify that the write position is at offset 0x12d.
    // this is a check to make sure that https://github.com/bitcoin/bitcoin/issues/21379 does not recur
    // 8 bytes (for serialization header) + 285 (for serialized genesis block) = 293
    // add another 8 bytes for the second block's serialization header and we get 293 + 8 = 301
    FlatFilePos actual{blockman.WriteBlock(params->GenesisBlock(), 1)};
    BOOST_CHECK_EQUAL(actual.nPos, STORAGE_HEADER_BYTES + ::GetSerializeSize(TX_WITH_WITNESS(params->GenesisBlock())) + STORAGE_HEADER_BYTES);
}

BOOST_FIXTURE_TEST_CASE(blockmanager_scan_unlink_already_pruned_files, TestChain100Setup)
{
    // Cap last block file size, and mine new block in a new block file.
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    const CBlockIndex* old_tip{WITH_LOCK(chainman.GetMutex(), return chainman.ActiveChain().Tip())};
    WITH_LOCK(chainman.GetMutex(), blockman.GetBlockFileInfo(old_tip->GetBlockPos().nFile)->nSize = MAX_BLOCKFILE_SIZE);
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

    // Prune the older block file, but don't unlink it
    int file_number;
    {
        LOCK(chainman.GetMutex());
        file_number = old_tip->GetBlockPos().nFile;
        blockman.PruneOneBlockFile(file_number);
    }

    const FlatFilePos pos(file_number, 0);

    // Check that the file is not unlinked after ScanAndUnlinkAlreadyPrunedFiles
    // if m_have_pruned is not yet set
    WITH_LOCK(chainman.GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());
    BOOST_CHECK(!blockman.OpenBlockFile(pos, true).IsNull());

    // Check that the file is unlinked after ScanAndUnlinkAlreadyPrunedFiles
    // once m_have_pruned is set
    blockman.m_have_pruned = true;
    WITH_LOCK(chainman.GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());
    BOOST_CHECK(blockman.OpenBlockFile(pos, true).IsNull());

    // Check that calling with already pruned files doesn't cause an error
    WITH_LOCK(chainman.GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());

    // Check that the new tip file has not been removed
    const CBlockIndex* new_tip{WITH_LOCK(chainman.GetMutex(), return chainman.ActiveChain().Tip())};
    BOOST_CHECK_NE(old_tip, new_tip);
    const int new_file_number{WITH_LOCK(chainman.GetMutex(), return new_tip->GetBlockPos().nFile)};
    const FlatFilePos new_pos(new_file_number, 0);
    BOOST_CHECK(!blockman.OpenBlockFile(new_pos, true).IsNull());
}

BOOST_FIXTURE_TEST_CASE(blockmanager_block_data_availability, TestChain100Setup)
{
    // The goal of the function is to return the first not pruned block in the range [upper_block, lower_block].
    LOCK(::cs_main);
    auto& chainman = m_node.chainman;
    auto& blockman = chainman->m_blockman;
    const CBlockIndex& tip = *chainman->ActiveTip();

    // Function to prune all blocks from 'last_pruned_block' down to the genesis block
    const auto& func_prune_blocks = [&](CBlockIndex* last_pruned_block)
    {
        LOCK(::cs_main);
        CBlockIndex* it = last_pruned_block;
        while (it != nullptr && it->nStatus & BLOCK_HAVE_DATA) {
            it->nStatus &= ~BLOCK_HAVE_DATA;
            it = it->pprev;
        }
    };

    // 1) Return genesis block when all blocks are available
    BOOST_CHECK_EQUAL(&blockman.GetFirstBlock(tip, BLOCK_HAVE_DATA), chainman->ActiveChain()[0]);
    BOOST_CHECK(blockman.CheckBlockDataAvailability(tip, *chainman->ActiveChain()[0]));

    // 2) Check lower_block when all blocks are available
    CBlockIndex* lower_block = chainman->ActiveChain()[tip.nHeight / 2];
    BOOST_CHECK(blockman.CheckBlockDataAvailability(tip, *lower_block));

    // Ensure we don't fail due to the expected absence of undo data in the genesis block
    CBlockIndex* upper_block = chainman->ActiveChain()[2];
    CBlockIndex* genesis = chainman->ActiveChain()[0];
    BOOST_CHECK(blockman.CheckBlockDataAvailability(*upper_block, *genesis, BlockStatus{BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO}));
    // Ensure we detect absence of undo data in the first block
    chainman->ActiveChain()[1]->nStatus &= ~BLOCK_HAVE_UNDO;
    BOOST_CHECK(!blockman.CheckBlockDataAvailability(tip, *genesis, BlockStatus{BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO}));

    // Prune half of the blocks
    int height_to_prune = tip.nHeight / 2;
    CBlockIndex* first_available_block = chainman->ActiveChain()[height_to_prune + 1];
    CBlockIndex* last_pruned_block = first_available_block->pprev;
    func_prune_blocks(last_pruned_block);

    // 3) The last block not pruned is in-between upper-block and the genesis block
    BOOST_CHECK_EQUAL(&blockman.GetFirstBlock(tip, BLOCK_HAVE_DATA), first_available_block);
    BOOST_CHECK(blockman.CheckBlockDataAvailability(tip, *first_available_block));
    BOOST_CHECK(!blockman.CheckBlockDataAvailability(tip, *last_pruned_block));

    // Simulate that the first available block is missing undo data and
    // detect this by using a status mask.
    first_available_block->nStatus &= ~BLOCK_HAVE_UNDO;
    BOOST_CHECK(!blockman.CheckBlockDataAvailability(tip, *first_available_block, BlockStatus{BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO}));
    BOOST_CHECK(blockman.CheckBlockDataAvailability(tip, *first_available_block, BlockStatus{BLOCK_HAVE_DATA}));
}

BOOST_FIXTURE_TEST_CASE(blockmanager_block_data_part, TestChain100Setup)
{
    LOCK(::cs_main);
    auto& chainman{m_node.chainman};
    auto& blockman{chainman->m_blockman};
    const CBlockIndex& tip{*chainman->ActiveTip()};
    const FlatFilePos tip_block_pos{tip.GetBlockPos()};

    auto block{blockman.ReadRawBlock(tip_block_pos)};
    BOOST_REQUIRE(block);
    BOOST_REQUIRE_GE(block->size(), 200);

    const auto expect_part{[&](size_t offset, size_t size) {
        auto res{blockman.ReadRawBlock(tip_block_pos, std::pair{offset, size})};
        BOOST_CHECK(res);
        const auto& part{res.value()};
        BOOST_CHECK_EQUAL_COLLECTIONS(part.begin(), part.end(), block->begin() + offset, block->begin() + offset + size);
    }};

    expect_part(0, 20);
    expect_part(0, block->size() - 1);
    expect_part(0, block->size() - 10);
    expect_part(0, block->size());
    expect_part(1, block->size() - 1);
    expect_part(10, 20);
    expect_part(block->size() - 1, 1);
}

BOOST_FIXTURE_TEST_CASE(blockmanager_block_data_part_error, TestChain100Setup)
{
    LOCK(::cs_main);
    auto& chainman{m_node.chainman};
    auto& blockman{chainman->m_blockman};
    const CBlockIndex& tip{*chainman->ActiveTip()};
    const FlatFilePos tip_block_pos{tip.GetBlockPos()};

    auto block{blockman.ReadRawBlock(tip_block_pos)};
    BOOST_REQUIRE(block);
    BOOST_REQUIRE_GE(block->size(), 200);

    const auto expect_part_error{[&](size_t offset, size_t size) {
        auto res{blockman.ReadRawBlock(tip_block_pos, std::pair{offset, size})};
        BOOST_CHECK(!res);
        BOOST_CHECK_EQUAL(res.error(), node::ReadRawError::BadPartRange);
    }};

    expect_part_error(0, 0);
    expect_part_error(0, block->size() + 1);
    expect_part_error(0, std::numeric_limits<size_t>::max());
    expect_part_error(1, block->size());
    expect_part_error(2, block->size() - 1);
    expect_part_error(block->size() - 1, 2);
    expect_part_error(block->size() - 2, 3);
    expect_part_error(block->size() + 1, 0);
    expect_part_error(block->size() + 1, 1);
    expect_part_error(block->size() + 2, 2);
    expect_part_error(block->size(), 0);
    expect_part_error(block->size(), 1);
    expect_part_error(std::numeric_limits<size_t>::max(), 1);
    expect_part_error(std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max());
}

BOOST_FIXTURE_TEST_CASE(blockmanager_readblock_hash_mismatch, TestingSetup)
{
    CBlockIndex index;
    {
        LOCK(cs_main);
        const auto tip{m_node.chainman->ActiveTip()};
        index.nStatus = tip->nStatus;
        index.nDataPos = tip->nDataPos;
        index.phashBlock = &uint256::ONE; // mismatched block hash
    }

    ASSERT_DEBUG_LOG("GetHash() doesn't match index");
    CBlock block;
    BOOST_CHECK(!m_node.chainman->m_blockman.ReadBlock(block, index));
}

BOOST_AUTO_TEST_CASE(blockmanager_flush_block_file)
{
    KernelNotifications notifications{Assert(m_node.shutdown_request), m_node.exit_status, *Assert(m_node.warnings)};
    node::BlockManager::Options blockman_opts{
        .chainparams = Params(),
        .blocks_dir = m_args.GetBlocksDirPath(),
        .notifications = notifications,
        .block_tree_db_params = DBParams{
            .path = m_args.GetDataDirNet() / "blocks" / "index",
            .cache_bytes = 0,
        },
    };
    BlockManager blockman{*Assert(m_node.shutdown_signal), blockman_opts};

    // Test blocks with no transactions, not even a coinbase
    CBlock block1;
    block1.nVersion = 1;
    CBlock block2;
    block2.nVersion = 2;
    CBlock block3;
    block3.nVersion = 3;

    // They are 80 bytes header + 1 byte 0x00 for vtx length
    constexpr int TEST_BLOCK_SIZE{81};

    // Blockstore is empty
    LOCK(::cs_main);
    BOOST_CHECK_EQUAL(blockman.CalculateCurrentUsage(), 0);

    // Write the first block to a new location.
    FlatFilePos pos1{blockman.WriteBlock(block1, /*nHeight=*/1)};

    // Write second block
    FlatFilePos pos2{blockman.WriteBlock(block2, /*nHeight=*/2)};

    // Two blocks in the file
    BOOST_CHECK_EQUAL(blockman.CalculateCurrentUsage(), (TEST_BLOCK_SIZE + STORAGE_HEADER_BYTES) * 2);

    // First two blocks are written as expected
    // Errors are expected because block data is junk, thrown AFTER successful read
    CBlock read_block;
    BOOST_CHECK_EQUAL(read_block.nVersion, 0);
    {
        ASSERT_DEBUG_LOG("Errors in block header");
        BOOST_CHECK(!blockman.ReadBlock(read_block, pos1, {}));
        BOOST_CHECK_EQUAL(read_block.nVersion, 1);
    }
    {
        ASSERT_DEBUG_LOG("Errors in block header");
        BOOST_CHECK(!blockman.ReadBlock(read_block, pos2, {}));
        BOOST_CHECK_EQUAL(read_block.nVersion, 2);
    }

    // During reindex, the flat file block storage will not be written to.
    // UpdateBlockInfo will, however, update the blockfile metadata.
    // Verify this behavior by attempting (and failing) to write block 3 data
    // to block 2 location.
    CBlockFileInfo* block_data = blockman.GetBlockFileInfo(0);
    BOOST_CHECK_EQUAL(block_data->nBlocks, 2);
    blockman.UpdateBlockInfo(block3, /*nHeight=*/3, /*pos=*/pos2);
    // Metadata is updated...
    BOOST_CHECK_EQUAL(block_data->nBlocks, 3);
    // ...but there are still only two blocks in the file
    BOOST_CHECK_EQUAL(blockman.CalculateCurrentUsage(), (TEST_BLOCK_SIZE + STORAGE_HEADER_BYTES) * 2);

    // Block 2 was not overwritten:
    BOOST_CHECK(!blockman.ReadBlock(read_block, pos2, {}));
    BOOST_CHECK_EQUAL(read_block.nVersion, 2);
}

BOOST_FIXTURE_TEST_CASE(prune_lock_update_and_delete, TestingSetup)
{
    LOCK(::cs_main);
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};

    // Create a prune lock
    blockman.UpdatePruneLock("test_lock", node::PruneLockInfo{.height_first = 100});

    // Update it to a new height
    blockman.UpdatePruneLock("test_lock", node::PruneLockInfo{.height_first = 200});

    // Delete existing prune lock
    BOOST_CHECK(blockman.DeletePruneLock("test_lock"));

    // Verify deletion worked by trying to delete the same lock again
    BOOST_CHECK(!blockman.DeletePruneLock("test_lock"));

    // Deleting a non-existent lock returns false
    BOOST_CHECK(!blockman.DeletePruneLock("nonexistent"));
}

BOOST_FIXTURE_TEST_CASE(blockmanager_add_to_block_index_best_header, TestingSetup)
{
    LOCK(::cs_main);
    auto& blockman{m_node.chainman->m_blockman};
    const CBlockIndex* genesis{m_node.chainman->ActiveTip()};

    CBlockHeader header;
    header.hashPrevBlock = genesis->GetBlockHash();
    header.nTime = genesis->nTime + 1;
    header.nBits = genesis->nBits;

    CBlockIndex* best_header{nullptr};
    header.nNonce = 1;
    CBlockIndex* first{blockman.AddToBlockIndex(header, best_header)};
    BOOST_CHECK_EQUAL(best_header, first);

    // A sibling with equal chain work must not replace the first-seen header
    header.nNonce = 2;
    CBlockIndex* second{blockman.AddToBlockIndex(header, best_header)};
    BOOST_CHECK(second != first);
    BOOST_CHECK(second->nChainWork == first->nChainWork);
    BOOST_CHECK_EQUAL(best_header, first);
}

BOOST_AUTO_TEST_CASE(blockmanager_off_cs_main_write_and_concurrent_reads)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::REGTEST)};
    KernelNotifications notifications{Assert(m_node.shutdown_request), m_node.exit_status, *Assert(m_node.warnings)};
    const BlockManager::Options blockman_opts{
        .chainparams = *params,
        .blocks_dir = m_args.GetBlocksDirPath(),
        .notifications = notifications,
        .block_tree_db_params = DBParams{
            .path = m_args.GetDataDirNet() / "blocks" / "index",
            .cache_bytes = 0,
        },
    };
    BlockManager blockman{*Assert(m_node.shutdown_signal), blockman_opts};

    // Build 4 distinct valid-header blocks and write them via SerializeBlockForDisk ->
    // ReserveBlockPos (under cs_main) -> WriteRawBlockToDisk (off cs_main).
    std::vector<CBlock> blocks;
    std::vector<FlatFilePos> data_positions;
    uint256 prev_hash = params->GenesisBlock().GetHash();
    for (unsigned int h = 0; h < 4; ++h) {
        CBlock blk = params->GenesisBlock();
        if (h > 0) {
            blk.hashPrevBlock = prev_hash;
            blk.nTime += h;
            blk.nNonce = 0;
            while (!CheckProofOfWork(blk.GetHash(), blk.nBits, params->GetConsensus())) {
                ++blk.nNonce;
            }
        }
        auto raw = blockman.SerializeBlockForDisk(blk);
        BlockManager::BlockWriteReservation res;
        {
            LOCK(::cs_main);
            res = blockman.ReserveBlockPos(raw.size(), h, blk.GetBlockTime());
            BOOST_REQUIRE(!res.pos.IsNull());
        }
        // Write off cs_main, keeping the persistent write handle open for the first 3 blocks.
        const FlatFilePos data_pos{res.pos.nFile, res.pos.nPos + STORAGE_HEADER_BYTES};
        BOOST_REQUIRE(blockman.WriteRawBlockToDisk(res.pos, raw, /*keep_handle_open=*/(h < 3)));
        res.Release();

        blocks.push_back(blk);
        data_positions.push_back(data_pos);
        prev_hash = blk.GetHash();
    }

    BOOST_CHECK(blockman.CloseBlockWriteHandle());
    BOOST_CHECK(!blockman.HasStorageIOError());

    // AdviseBlockRange works off cs_main.
    blockman.AdviseBlockRange(data_positions[0], 0, 4096);

    // Verify ReadBlockConcurrent across 8 threads reading all 4 blocks simultaneously.
    std::atomic<size_t> mismatches{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 8; ++t) {
        readers.emplace_back([&, t] {
            for (int iter = 0; iter < 16; ++iter) {
                const size_t idx = static_cast<size_t>((t + iter) % blocks.size());
                CBlock decoded;
                if (!blockman.ReadBlockConcurrent(decoded, data_positions[idx], blocks[idx].GetHash()) ||
                    decoded.GetHash() != blocks[idx].GetHash()) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : readers) th.join();
    BOOST_CHECK_EQUAL(mismatches.load(), 0U);

    // Hash mismatch must fail ReadBlockConcurrent cleanly.
    {
        CBlock decoded;
        BOOST_CHECK(!blockman.ReadBlockConcurrent(decoded, data_positions[0], uint256::ONE));
    }

    // Closing concurrent read handles still allows subsequent reads to reopen seamlessly.
    blockman.CloseConcurrentReadHandles();
    {
        CBlock decoded;
        BOOST_CHECK(blockman.ReadBlockConcurrent(decoded, data_positions[0], blocks[0].GetHash()));
        BOOST_CHECK(decoded.GetHash() == blocks[0].GetHash());
    }

    // Storage I/O latch transitions from false to true and sticks.
    blockman.LatchStorageIOError();
    BOOST_CHECK(blockman.HasStorageIOError());
}

BOOST_FIXTURE_TEST_CASE(blockmanager_async_undo_roundtrip_and_stop_race, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    m_node.notifications->m_shutdown_on_fatal_error = false;

    // Async undo writer starts lazily (not running until first EnqueueBlockUndoWrite).
    BOOST_CHECK(!blockman.IsAsyncUndoWriterRunning());

    // Prepare a synthetic CBlockUndo containing a real spent Coin and write it asynchronously
    // for 8 existing blocks (heights 93..100) while another thread concurrently stops the writer.
    std::vector<CBlockIndex*> targets;
    std::vector<uint256> prev_hashes;
    {
        LOCK(::cs_main);
        for (int h = 93; h <= 100; ++h) {
            CBlockIndex* idx = chainman.ActiveChain()[h];
            BOOST_REQUIRE(idx != nullptr);
            idx->nStatus &= ~BLOCK_HAVE_UNDO;
            idx->nUndoPos = 0;
            targets.push_back(idx);
            prev_hashes.push_back(idx->pprev->GetBlockHash());
        }
    }

    auto make_undo = [](int h) {
        CBlockUndo undo;
        CTxUndo txundo;
        txundo.vprevout.emplace_back(CTxOut{static_cast<CAmount>(10'000 + h), CScript() << OP_TRUE}, h - 1, false);
        undo.vtxundo.push_back(std::move(txundo));
        return undo;
    };

    // Race EnqueueBlockUndoWrite() under cs_main against concurrent StopAsyncUndoWriter() off cs_main.
    std::atomic<bool> stop_stopper{false};
    std::thread stopper([&] {
        while (!stop_stopper.load(std::memory_order_acquire)) {
            blockman.StopAsyncUndoWriter();
            std::this_thread::yield();
        }
    });

    for (size_t i = 0; i < targets.size(); ++i) {
        auto serialized = blockman.SerializeBlockUndoPayload(make_undo(targets[i]->nHeight), prev_hashes[i]);
        LOCK(::cs_main);
        BlockValidationState state;
        BOOST_REQUIRE(blockman.EnqueueBlockUndoWrite(std::move(serialized), state, *targets[i]));
    }

    stop_stopper.store(true, std::memory_order_release);
    stopper.join();

    // Every enqueued undo must settle cleanly and round-trip identically via ReadBlockUndo().
    {
        LOCK(::cs_main);
        BOOST_REQUIRE(blockman.SettlePendingUndoWrites());
        BOOST_CHECK(!blockman.HasPendingUndoWrites());
        BOOST_CHECK(!blockman.HasStorageIOError());
        for (CBlockIndex* idx : targets) {
            BOOST_CHECK((idx->nStatus & BLOCK_HAVE_UNDO) != 0);
            CBlockUndo read_back;
            BOOST_REQUIRE(blockman.ReadBlockUndo(read_back, *idx));
            BOOST_REQUIRE_EQUAL(read_back.vtxundo.size(), 1U);
            BOOST_REQUIRE_EQUAL(read_back.vtxundo[0].vprevout.size(), 1U);
            BOOST_CHECK_EQUAL(read_back.vtxundo[0].vprevout[0].out.nValue, static_cast<CAmount>(10'000 + idx->nHeight));
            BOOST_CHECK_EQUAL(read_back.vtxundo[0].vprevout[0].nHeight, static_cast<uint32_t>(idx->nHeight - 1));
        }
    }

    // Injecting an async undo write fault must fail SettlePendingUndoWrites() and latch HasStorageIOError().
    std::atomic<int> hook_hits{0};
    blockman.SetUndoWriteFaultHookForTest([&](const uint256& hash, int height) {
        if (height == targets.back()->nHeight && hash == targets.back()->GetBlockHash()) {
            ++hook_hits;
            return true;
        }
        return false;
    });
    {
        auto serialized = blockman.SerializeBlockUndoPayload(make_undo(targets.back()->nHeight), prev_hashes.back());
        LOCK(::cs_main);
        targets.back()->nStatus &= ~BLOCK_HAVE_UNDO;
        targets.back()->nUndoPos = 0;
        BlockValidationState state;
        BOOST_REQUIRE(blockman.EnqueueBlockUndoWrite(std::move(serialized), state, *targets.back()));
        BOOST_CHECK(!blockman.SettlePendingUndoWrites());
        BOOST_CHECK_EQUAL(hook_hits.load(), 1);
        BOOST_CHECK(blockman.HasStorageIOError());
    }
    blockman.SetUndoWriteFaultHookForTest({});
    blockman.StopAsyncUndoWriter();
    BOOST_CHECK(!blockman.IsAsyncUndoWriterRunning());
}

BOOST_AUTO_TEST_SUITE_END()
