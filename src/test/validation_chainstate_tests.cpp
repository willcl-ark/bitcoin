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
#include <node/miner.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/chainstate.h>
#include <test/util/coins.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <tinyformat.h>
#include <uint256.h>
#include <undo.h>
#include <util/check.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

namespace {

struct BatchChainSetup : TestChain100Setup {
    BatchChainSetup() : TestChain100Setup{ChainType::REGTEST, {.extra_args = {"-assumevalid=0"}}}
    {
        auto& chainman{*Assert(m_node.chainman)};
        m_clock += std::chrono::hours{24 * 30};
        static_cast<TestChainstateManager&>(chainman).ResetIbd();
        LOCK(cs_main);
        chainman.UpdateIBDStatus();
        BOOST_REQUIRE(chainman.IsInitialBlockDownload());
        BOOST_REQUIRE(chainman.AssumedValidBlock().IsNull());
        BOOST_REQUIRE_GE(chainman.m_options.prevoutfetch_threads_num, 2);
        chainman.ActiveChainstate().ResizeCoinsCaches(128_MiB, 8_MiB);
    }

    COutPoint FundingCoin()
    {
        LOCK(cs_main);
        auto& coins{m_node.chainman->ActiveChainstate().CoinsTip()};
        const auto outpoint{AddTestCoin(m_rng, coins)};
        coins.AddCoin(outpoint, Coin{CTxOut{5 * COIN, CScript{} << OP_TRUE}, 1, false}, true);
        return outpoint;
    }

    std::vector<std::shared_ptr<const CBlock>> AdmitBatch(const std::vector<std::vector<CMutableTransaction>>& transactions)
    {
        auto& chainman{*Assert(m_node.chainman)};
        CBlockIndex* parent{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
        std::vector<std::shared_ptr<const CBlock>> blocks;
        for (const auto& txs : transactions) {
            auto block{std::make_shared<CBlock>(CreateBlock(txs, CScript{} << OP_TRUE))};
            block->hashPrevBlock = parent->GetBlockHash();
            block->nTime = parent->nTime + 1;
            CMutableTransaction coinbase{*block->vtx[0]};
            coinbase.vin[0].scriptSig = CScript{} << parent->nHeight + 1 << OP_0;
            block->vtx[0] = MakeTransactionRef(std::move(coinbase));
            node::RegenerateCommitments(*block, chainman);
            block->fChecked = false;
            block->m_checked_merkle_root = false;
            block->m_checked_witness_commitment = false;
            block->nNonce = 0;
            while (!CheckProofOfWork(block->GetHash(), block->nBits, chainman.GetConsensus())) {
                ++block->nNonce;
            }
            BlockValidationState state;
            {
                LOCK(cs_main);
                BOOST_REQUIRE(chainman.AcceptBlock(block, state, &parent, true, nullptr, nullptr, true));
            }
            blocks.emplace_back(std::move(block));
        }
        BOOST_CHECK(!WITH_LOCK(cs_main, return chainman.ActiveChainstate().CanValidateBatch()));

        // Regtest's 600-second target spacing makes 2025 blocks exceed 14 days
        // of proof-equivalent time. Bodies remain unavailable beyond the group.
        std::vector<CBlockHeader> headers;
        CBlockHeader header{*blocks.back()};
        for (int i{0}; i < 2025; ++i) {
            header.hashPrevBlock = header.GetHash();
            ++header.nTime;
            header.nNonce = 0;
            while (!CheckProofOfWork(header.GetHash(), header.nBits, chainman.GetConsensus())) {
                ++header.nNonce;
            }
            headers.emplace_back(header);
        }
        BlockValidationState state;
        BOOST_REQUIRE(chainman.ProcessNewBlockHeaders(headers, true, state));
        BOOST_REQUIRE(WITH_LOCK(cs_main, return chainman.ActiveChainstate().CanValidateBatch()));
        return blocks;
    }
};

CMutableTransaction BatchSpend(const COutPoint& input, CAmount value, const CScript& script)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(input);
    tx.vout.emplace_back(value, script);
    return tx;
}

struct BatchChecked : CValidationInterface {
    struct Result {
        uint256 hash;
        bool valid;
        std::string reason;
        uint256 tip;
        uint256 coins_tip;
    };
    Chainstate& chainstate;
    std::vector<Result> results;

    explicit BatchChecked(Chainstate& chainstate_in) : chainstate{chainstate_in} {}
    void BlockChecked(const std::shared_ptr<const CBlock>& block, const BlockValidationState& state) override
    {
        LOCK(cs_main);
        results.push_back({block->GetHash(), state.IsValid(), state.GetRejectReason(), chainstate.m_chain.Tip()->GetBlockHash(), chainstate.CoinsTip().GetBestBlock()});
    }
};

} // namespace

BOOST_FIXTURE_TEST_CASE(grouped_activation_publishes_final_tip_and_disconnects, BatchChainSetup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& chainstate{chainman.ActiveChainstate()};
    const auto original_tip{WITH_LOCK(cs_main, return chainstate.m_chain.Tip()->GetBlockHash())};
    const auto funding{FundingCoin()};
    const CScript script{CScript{} << OP_TRUE};
    const auto first{BatchSpend(funding, 4 * COIN, script)};
    const COutPoint intermediate{first.GetHash(), 0};
    const auto second{BatchSpend(intermediate, 3 * COIN, script)};
    const COutPoint final{second.GetHash(), 0};
    const auto blocks{AdmitBatch({{first}, {second}})};
    auto checked{std::make_shared<BatchChecked>(chainstate)};
    m_node.validation_signals->RegisterSharedValidationInterface(checked);
    BlockValidationState state;
    const bool activated{chainstate.ActivateBestChain(state, nullptr, blocks)};
    m_node.validation_signals->UnregisterSharedValidationInterface(checked);
    BOOST_REQUIRE(activated);
    BOOST_REQUIRE_EQUAL(checked->results.size(), 2U);
    for (size_t i{0}; i < blocks.size(); ++i) {
        BOOST_CHECK(checked->results[i].valid);
        BOOST_CHECK(checked->results[i].hash == blocks[i]->GetHash());
        // Serial ConnectTip signals before merging its overlay or updating tip.
        BOOST_CHECK(checked->results[i].tip == blocks.back()->GetHash());
        BOOST_CHECK(checked->results[i].coins_tip == blocks.back()->GetHash());
    }
    CBlockIndex* first_index;
    CBlockIndex* last_index;
    {
        LOCK(cs_main);
        first_index = chainman.m_blockman.LookupBlockIndex(blocks.front()->GetHash());
        last_index = chainman.m_blockman.LookupBlockIndex(blocks.back()->GetHash());
        BOOST_REQUIRE(chainstate.CoinsTip().HaveCoin(final));
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(funding));
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(intermediate));
        for (const auto* index : {first_index, last_index}) {
            CBlockUndo undo;
            BOOST_REQUIRE(chainman.m_blockman.ReadBlockUndo(undo, *index));
            BOOST_REQUIRE_EQUAL(undo.vtxundo.size(), 1U);
            BOOST_REQUIRE_EQUAL(undo.vtxundo[0].vprevout.size(), 1U);
            const auto& coin{undo.vtxundo[0].vprevout[0]};
            BOOST_CHECK_EQUAL(coin.out.nValue, index == first_index ? 5 * COIN : 4 * COIN);
            BOOST_CHECK_EQUAL(coin.nHeight, index == first_index ? 1 : first_index->nHeight);
            BOOST_CHECK(coin.out.scriptPubKey == script);
            BOOST_CHECK(!coin.IsCoinBase());
        }
    }
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, last_index));
    {
        LOCK(cs_main);
        BOOST_CHECK(chainstate.m_chain.Tip() == first_index);
        BOOST_REQUIRE(chainstate.CoinsTip().HaveCoin(intermediate));
        BOOST_CHECK_EQUAL(chainstate.CoinsTip().GetCoin(intermediate)->out.nValue, 4 * COIN);
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(final));
    }
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, first_index));
    {
        LOCK(cs_main);
        BOOST_CHECK(chainstate.m_chain.Tip()->GetBlockHash() == original_tip);
        BOOST_CHECK(chainstate.CoinsTip().GetBestBlock() == original_tip);
        BOOST_REQUIRE(chainstate.CoinsTip().HaveCoin(funding));
        BOOST_CHECK_EQUAL(chainstate.CoinsTip().GetCoin(funding)->out.nValue, 5 * COIN);
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(intermediate));
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(final));
    }
}

BOOST_FIXTURE_TEST_CASE(grouped_script_failure_accepts_only_serial_prefix, BatchChainSetup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& chainstate{chainman.ActiveChainstate()};
    const auto original_tip{WITH_LOCK(cs_main, return chainstate.m_chain.Tip()->GetBlockHash())};
    const auto funding{FundingCoin()};
    const auto first{BatchSpend(funding, 4 * COIN, CScript{} << OP_FALSE)};
    const COutPoint intermediate{first.GetHash(), 0};
    const auto invalid{BatchSpend(intermediate, 3 * COIN, CScript{} << OP_TRUE)};
    const COutPoint rejected_output{invalid.GetHash(), 0};
    const auto blocks{AdmitBatch({{first}, {invalid}, {}})};
    auto checked{std::make_shared<BatchChecked>(chainstate)};
    m_node.validation_signals->RegisterSharedValidationInterface(checked);
    BlockValidationState state;
    const bool activated{chainstate.ActivateBestChain(state, nullptr, blocks)};
    m_node.validation_signals->UnregisterSharedValidationInterface(checked);
    BOOST_REQUIRE(activated);
    BOOST_REQUIRE_EQUAL(checked->results.size(), 2U);
    BOOST_CHECK(checked->results[0].valid);
    BOOST_CHECK(checked->results[0].hash == blocks[0]->GetHash());
    BOOST_CHECK(checked->results[0].tip == original_tip);
    BOOST_CHECK(checked->results[0].coins_tip == original_tip);
    BOOST_CHECK(!checked->results[1].valid);
    BOOST_CHECK(checked->results[1].hash == blocks[1]->GetHash());
    BOOST_CHECK(checked->results[1].reason.starts_with("block-script-verify-flag-failed"));
    {
        LOCK(cs_main);
        BOOST_CHECK(chainstate.m_chain.Tip()->GetBlockHash() == blocks[0]->GetHash());
        BOOST_CHECK(chainstate.CoinsTip().GetBestBlock() == blocks[0]->GetHash());
        BOOST_CHECK(chainman.m_blockman.LookupBlockIndex(blocks[1]->GetHash())->nStatus & BLOCK_FAILED_VALID);
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(funding));
        BOOST_CHECK(chainstate.CoinsTip().HaveCoin(intermediate));
        BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(rejected_output));
    }
}

BOOST_FIXTURE_TEST_CASE(grouped_relative_lock_failure_accepts_only_producer, BatchChainSetup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& chainstate{chainman.ActiveChainstate()};
    const auto original_tip{WITH_LOCK(cs_main, return chainstate.m_chain.Tip()->GetBlockHash())};
    const auto funding{FundingCoin()};
    const CScript script{CScript{} << OP_TRUE};
    const auto producer{BatchSpend(funding, 4 * COIN, script)};
    const COutPoint intermediate{producer.GetHash(), 0};
    auto consumer{BatchSpend(intermediate, 3 * COIN, script)};
    consumer.version = 2;
    consumer.vin[0].nSequence = 2;
    const COutPoint rejected_output{consumer.GetHash(), 0};
    const auto blocks{AdmitBatch({{producer}, {consumer}})};
    auto checked{std::make_shared<BatchChecked>(chainstate)};
    m_node.validation_signals->RegisterSharedValidationInterface(checked);
    BlockValidationState state;
    const bool activated{chainstate.ActivateBestChain(state, nullptr, blocks)};
    m_node.validation_signals->UnregisterSharedValidationInterface(checked);
    BOOST_REQUIRE(activated);
    BOOST_REQUIRE_EQUAL(checked->results.size(), 2U);
    BOOST_CHECK(checked->results[0].valid);
    BOOST_CHECK(checked->results[0].hash == blocks[0]->GetHash());
    BOOST_CHECK(checked->results[0].tip == original_tip);
    BOOST_CHECK(!checked->results[1].valid);
    BOOST_CHECK(checked->results[1].hash == blocks[1]->GetHash());
    BOOST_CHECK_EQUAL(checked->results[1].reason, "bad-txns-nonfinal");
    LOCK(cs_main);
    BOOST_CHECK(chainstate.m_chain.Tip()->GetBlockHash() == blocks[0]->GetHash());
    BOOST_CHECK(chainstate.CoinsTip().GetBestBlock() == blocks[0]->GetHash());
    BOOST_REQUIRE(chainstate.CoinsTip().HaveCoin(intermediate));
    BOOST_CHECK_EQUAL(chainstate.CoinsTip().GetCoin(intermediate)->nHeight, chainstate.m_chain.Height());
    BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(funding));
    BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(rejected_output));
}

BOOST_FIXTURE_TEST_CASE(grouped_memory_limit_uses_serial_connection, BatchChainSetup)
{
    auto& chainman{*Assert(m_node.chainman)};
    auto& chainstate{chainman.ActiveChainstate()};
    const auto original_tip{WITH_LOCK(cs_main, return chainstate.m_chain.Tip()->GetBlockHash())};
    const auto funding{FundingCoin()};
    const CScript script{CScript{} << OP_TRUE};
    const auto first{BatchSpend(funding, 4 * COIN, script)};
    const COutPoint intermediate{first.GetHash(), 0};
    const auto second{BatchSpend(intermediate, 3 * COIN, script)};
    const COutPoint final{second.GetHash(), 0};
    const auto blocks{AdmitBatch({{first}, {second}})};
    {
        LOCK(cs_main);
        chainstate.ResizeCoinsCaches(128 * 1024, 8_MiB);
        BOOST_REQUIRE(chainstate.CanValidateBatch());
    }
    auto checked{std::make_shared<BatchChecked>(chainstate)};
    m_node.validation_signals->RegisterSharedValidationInterface(checked);
    BlockValidationState state;
    const bool activated{chainstate.ActivateBestChain(state, nullptr, blocks)};
    m_node.validation_signals->UnregisterSharedValidationInterface(checked);
    BOOST_REQUIRE(activated);
    BOOST_REQUIRE_EQUAL(checked->results.size(), 2U);
    BOOST_CHECK(checked->results[0].valid);
    BOOST_CHECK(checked->results[0].tip == original_tip);
    BOOST_CHECK(checked->results[0].coins_tip == original_tip);
    BOOST_CHECK(checked->results[1].valid);
    LOCK(cs_main);
    BOOST_CHECK(chainstate.m_chain.Tip()->GetBlockHash() == blocks.back()->GetHash());
    BOOST_CHECK(chainstate.CoinsTip().GetBestBlock() == blocks.back()->GetHash());
    BOOST_REQUIRE(chainstate.CoinsTip().HaveCoin(final));
    BOOST_CHECK_EQUAL(chainstate.CoinsTip().GetCoin(final)->out.nValue, 3 * COIN);
    BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(funding));
    BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(intermediate));
}

BOOST_AUTO_TEST_SUITE_END()
