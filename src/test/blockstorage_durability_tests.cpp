// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <node/blockstorage.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <tinyformat.h>
#include <util/check.h>
#include <util/fs.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <fstream>
#include <memory>

BOOST_AUTO_TEST_SUITE(blockstorage_durability_tests)

BOOST_FIXTURE_TEST_CASE(durability_follows_the_files, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{static_cast<TestBlockManager&>(chainman.m_blockman)};

    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    CBlockIndex* tip{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
    BOOST_REQUIRE(tip);
    {
        LOCK(cs_main);
        BOOST_CHECK(!blockman.IsBlockDataSynced(*tip));
        BOOST_CHECK(!blockman.IsBlockUndoSynced(*tip));
    }

    chainman.ActiveChainstate().ForceFlushStateToDisk();
    {
        LOCK(cs_main);
        BOOST_CHECK(blockman.IsBlockDataSynced(*tip));
        BOOST_CHECK(blockman.IsBlockUndoSynced(*tip));
    }

    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    tip = WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
    {
        LOCK(cs_main);
        BOOST_CHECK(!blockman.IsBlockDataSynced(*tip));
        BOOST_CHECK(!blockman.IsBlockUndoSynced(*tip));
        BOOST_CHECK(blockman.SyncWrittenFiles());
        BOOST_CHECK(blockman.IsBlockDataSynced(*tip));
        BOOST_CHECK(blockman.IsBlockUndoSynced(*tip));

        blockman.m_have_pruned = true;
        blockman.PruneOneBlockFile(tip->nFile);
        BOOST_CHECK(!blockman.IsBlockDataSynced(*tip));
        BOOST_CHECK(!blockman.IsBlockUndoSynced(*tip));
    }
}

namespace {
void InjectOpenFailure(const fs::path& file)
{
    Assert(fs::remove(file));
    Assert(fs::create_directory(file));
}

void RemoveOpenFailure(const fs::path& file)
{
    Assert(fs::remove(file));
    Assert(std::ofstream{static_cast<const std::filesystem::path&>(file)}.is_open());
}
} // namespace

BOOST_FIXTURE_TEST_CASE(failed_flush_fails_every_later_write_out, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& chainstate{chainman.ActiveChainstate()};
    auto& blockman{chainman.m_blockman};
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.FlushStateToDisk(state, FlushStateMode::FORCE_FLUSH));
    const CBlockIndex* flushed{WITH_LOCK(cs_main, return chainstate.GetLastFlushedBlock())};

    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    const CBlockIndex* tip{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
    BOOST_REQUIRE(tip != flushed);
    const fs::path file{WITH_LOCK(cs_main, return blockman.GetBlockPosFilename(tip->GetBlockPos()))};

    InjectOpenFailure(file);
    BOOST_CHECK(!chainstate.FlushStateToDisk(state, FlushStateMode::FORCE_FLUSH));
    BOOST_CHECK(state.IsError());

    RemoveOpenFailure(file);
    BlockValidationState later;
    BOOST_CHECK(!chainstate.FlushStateToDisk(later, FlushStateMode::FORCE_FLUSH));
    BOOST_CHECK(later.IsError());

    LOCK(cs_main);
    BOOST_CHECK(!blockman.IsBlockDataSynced(*tip));
    BOOST_CHECK(!blockman.SyncWrittenFiles());
    BOOST_CHECK(chainstate.GetLastFlushedBlock() == flushed);
    kernel::CBlockFileInfo file_info;
    BOOST_REQUIRE(blockman.m_block_tree_db->ReadBlockFileInfo(tip->nFile, file_info));
    BOOST_CHECK_EQUAL(file_info.nHeightLast, static_cast<unsigned int>(flushed->nHeight));
}

BOOST_FIXTURE_TEST_CASE(failed_block_file_sync_is_remembered, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    const CBlockIndex* tip{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
    const fs::path file{WITH_LOCK(cs_main, return blockman.GetBlockPosFilename(tip->GetBlockPos()))};

    InjectOpenFailure(file);
    {
        LOCK(cs_main);
        BOOST_CHECK(!blockman.SyncWrittenFiles());
        BOOST_CHECK(!blockman.IsBlockDataSynced(*tip));
    }
    RemoveOpenFailure(file);
    BOOST_CHECK(!WITH_LOCK(cs_main, return blockman.SyncWrittenFiles()));
}

BOOST_FIXTURE_TEST_CASE(failed_undo_file_sync_is_remembered, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    const CBlockIndex* tip{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
    const fs::path block_file{WITH_LOCK(cs_main, return blockman.GetBlockPosFilename(tip->GetBlockPos()))};
    const int file_number{WITH_LOCK(cs_main, return tip->nFile)};
    const fs::path file{fs::path{block_file.parent_path()} / fs::PathFromString(strprintf("rev%05u.dat", file_number))};
    BOOST_REQUIRE(fs::exists(file));

    InjectOpenFailure(file);
    {
        LOCK(cs_main);
        BOOST_CHECK(!blockman.SyncWrittenFiles());
        BOOST_CHECK(blockman.IsBlockDataSynced(*tip));
        BOOST_CHECK(!blockman.IsBlockUndoSynced(*tip));
    }
    RemoveOpenFailure(file);
    BOOST_CHECK(!WITH_LOCK(cs_main, return blockman.SyncWrittenFiles()));
}

namespace {
struct FastPruneChain100Setup : public TestChain100Setup {
    FastPruneChain100Setup() : TestChain100Setup{ChainType::REGTEST, {.extra_args = {"-fastprune"}}} {}
};

std::shared_ptr<const CBlock> ChildBlock(const CBlockHeader& prev, int height, CAmount value, size_t script_size, const Consensus::Params& params)
{
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << height << OP_0;
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = value;
    coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;
    coinbase.vout[0].scriptPubKey.resize(script_size);

    CBlock block;
    block.nVersion = prev.nVersion;
    block.hashPrevBlock = prev.GetHash();
    block.nTime = prev.nTime + 1;
    block.nBits = prev.nBits;
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    block.hashMerkleRoot = BlockMerkleRoot(block);
    while (!CheckProofOfWork(block.GetHash(), block.nBits, params)) ++block.nNonce;
    return std::make_shared<const CBlock>(std::move(block));
}
} // namespace

BOOST_FIXTURE_TEST_CASE(undo_in_a_left_file_is_synced, FastPruneChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    const auto& params{chainman.GetConsensus()};
    const CBlockHeader tip_header{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->GetBlockHeader())};
    const int height{WITH_LOCK(cs_main, return chainman.ActiveChain().Height())};

    const auto first{ChildBlock(tip_header, height + 1, 50 * COIN, 1, params)};
    const auto second{ChildBlock(*first, height + 2, 51 * COIN, 1, params)};
    const auto large{ChildBlock(*second, height + 3, 50 * COIN, 70'000, params)};
    CBlockIndex* first_index{nullptr};
    CBlockIndex* second_index{nullptr};
    CBlockIndex* large_index{nullptr};
    {
        LOCK(cs_main);
        BlockValidationState state;
        BOOST_REQUIRE(chainman.AcceptBlock(first, state, &first_index, /*fRequested=*/true, nullptr, nullptr, /*min_pow_checked=*/true));
        BOOST_REQUIRE(chainman.AcceptBlock(second, state, &second_index, /*fRequested=*/true, nullptr, nullptr, /*min_pow_checked=*/true));
        BOOST_REQUIRE(chainman.AcceptBlock(large, state, &large_index, /*fRequested=*/true, nullptr, nullptr, /*min_pow_checked=*/true));
        BOOST_REQUIRE(first_index && large_index);
        BOOST_REQUIRE_LT(first_index->nFile, large_index->nFile);
    }

    BlockValidationState state;
    BOOST_REQUIRE(chainman.ActiveChainstate().ActivateBestChain(state));
    {
        LOCK(cs_main);
        BOOST_REQUIRE(chainman.ActiveChain().Tip() == first_index);
        BOOST_CHECK(!blockman.IsBlockUndoSynced(*first_index));
    }

    chainman.ActiveChainstate().ForceFlushStateToDisk();
    {
        LOCK(cs_main);
        BOOST_CHECK(blockman.IsBlockUndoSynced(*first_index));
    }
}

BOOST_FIXTURE_TEST_CASE(other_cursor_file_synced_by_the_write_out, TestChain100Setup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    const auto& params{chainman.GetConsensus()};
    const CBlockHeader tip_header{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()->GetBlockHeader())};
    const int height{WITH_LOCK(cs_main, return chainman.ActiveChain().Height())};

    const auto below{ChildBlock(tip_header, height + 1, 50 * COIN, 1, params)};
    const auto above{ChildBlock(*below, height + 2, 50 * COIN, 1, params)};
    CBlockIndex* below_index{nullptr};
    CBlockIndex* above_index{nullptr};
    {
        LOCK(cs_main);
        blockman.m_snapshot_height = height + 2;
        BlockValidationState state;
        BOOST_REQUIRE(chainman.AcceptBlock(below, state, &below_index, /*fRequested=*/true, nullptr, nullptr, /*min_pow_checked=*/true));
        BOOST_REQUIRE(chainman.AcceptBlock(above, state, &above_index, /*fRequested=*/true, nullptr, nullptr, /*min_pow_checked=*/true));
        BOOST_REQUIRE(below_index && above_index);
        BOOST_REQUIRE_NE(below_index->nFile, above_index->nFile);
        BOOST_CHECK(!blockman.IsBlockDataSynced(*below_index));
        BOOST_CHECK(!blockman.IsBlockDataSynced(*above_index));
    }

    chainman.ActiveChainstate().ForceFlushStateToDisk();
    LOCK(cs_main);
    BOOST_CHECK(blockman.IsBlockDataSynced(*below_index));
    BOOST_CHECK(blockman.IsBlockDataSynced(*above_index));
    blockman.m_snapshot_height.reset();
}

BOOST_AUTO_TEST_SUITE_END()
