// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <node/block_template_manager.h>
#include <node/blockstorage.h>
#include <node/miner.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/common.h>
#include <test/util/mining.h>
#include <test/util/script.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/check.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using kernel::ChainstateRole;

namespace validation_block_tests {
struct MinerTestingSetup : public RegTestingSetup {
    /// Prepare a coinbase-only block extending the known header at `prev_hash`.
    /// The coinbase has a unique zero-value output and a reward output spendable with OP_TRUE.
    /// Call FinalizeBlock() after any further edits to update commitments, mine the block,
    /// and submit its header.
    std::shared_ptr<CBlock> Block(const uint256& prev_hash);
    std::shared_ptr<const CBlock> GoodBlock(const uint256& prev_hash);
    std::shared_ptr<const CBlock> BadBlock(const uint256& prev_hash);
    std::shared_ptr<CBlock> FinalizeBlock(std::shared_ptr<CBlock> pblock);
    void BuildChain(const uint256& root, int height, unsigned int invalid_rate, unsigned int branch_rate, unsigned int max_size, std::vector<std::shared_ptr<const CBlock>>& blocks);
};
} // namespace validation_block_tests

BOOST_FIXTURE_TEST_SUITE(validation_block_tests, MinerTestingSetup)

struct TestSubscriber final : public CValidationInterface {
    uint256 m_expected_tip;

    explicit TestSubscriber(uint256 tip) : m_expected_tip(tip) {}

    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload) override
    {
        BOOST_CHECK_EQUAL(m_expected_tip, pindexNew->GetBlockHash());
    }

    void BlockConnected(const ChainstateRole& role, const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        BOOST_CHECK_EQUAL(m_expected_tip, block->hashPrevBlock);
        BOOST_CHECK_EQUAL(m_expected_tip, pindex->pprev->GetBlockHash());

        m_expected_tip = block->GetHash();
    }

    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        BOOST_CHECK_EQUAL(m_expected_tip, block->GetHash());
        BOOST_CHECK_EQUAL(m_expected_tip, pindex->GetBlockHash());

        m_expected_tip = block->hashPrevBlock;
    }
};

std::shared_ptr<CBlock> MinerTestingSetup::Block(const uint256& prev_hash)
{
    const auto& chainman{*Assert(m_node.chainman)};
    static int i = 0;
    static uint64_t time{chainman.GetParams().GenesisBlock().nTime};

    auto& block_template_manager{*Assert(m_node.block_template_manager)};
    auto block_template{block_template_manager.CreateNewTemplate({
        .use_mempool = false,
        .coinbase_output_script = CScript{} << i++ << OP_TRUE,
    })};
    auto pblock = std::make_shared<CBlock>(block_template->block);
    const auto& parent{*Assert(WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(prev_hash)))};
    SwitchBlockParent(*pblock, parent, /*new_time=*/++time, chainman.GetConsensus());

    // Make the coinbase transaction with two outputs:
    // One zero-value one that has a unique pubkey to make sure that blocks at the same height can have a different hash
    // Another one that has the coinbase reward in a P2WSH with OP_TRUE as witness program to make it easy to spend
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vout.resize(2);
    txCoinbase.vout[1].scriptPubKey = P2WSH_OP_TRUE;
    txCoinbase.vout[1].nValue = txCoinbase.vout[0].nValue;
    txCoinbase.vout[0].nValue = 0;
    txCoinbase.vin[0].scriptWitness = {};
    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));

    return pblock;
}

std::shared_ptr<CBlock> MinerTestingSetup::FinalizeBlock(std::shared_ptr<CBlock> pblock)
{
    const CBlockIndex* prev_block{WITH_LOCK(::cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(pblock->hashPrevBlock))};
    m_node.chainman->GenerateCoinbaseCommitment(*pblock, prev_block);

    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);

    while (!CheckProofOfWork(pblock->GetHash(), pblock->nBits, Params().GetConsensus())) {
        ++(pblock->nNonce);
    }

    // submit block header, so that miner can get the block height from the
    // global state and the node has the topology of the chain
    BlockValidationState ignored;
    BOOST_CHECK(Assert(m_node.chainman)->ProcessNewBlockHeaders({{*pblock}}, true, ignored));

    return pblock;
}

// construct a valid block
std::shared_ptr<const CBlock> MinerTestingSetup::GoodBlock(const uint256& prev_hash)
{
    return FinalizeBlock(Block(prev_hash));
}

// construct an invalid block (but with a valid header)
std::shared_ptr<const CBlock> MinerTestingSetup::BadBlock(const uint256& prev_hash)
{
    auto pblock = Block(prev_hash);

    CMutableTransaction coinbase_spend;
    coinbase_spend.vin.emplace_back(COutPoint(pblock->vtx[0]->GetHash(), 0), CScript(), 0);
    coinbase_spend.vout.push_back(pblock->vtx[0]->vout[0]);

    CTransactionRef tx = MakeTransactionRef(coinbase_spend);
    pblock->vtx.push_back(tx);

    auto ret = FinalizeBlock(pblock);
    return ret;
}

// NOLINTNEXTLINE(misc-no-recursion)
void MinerTestingSetup::BuildChain(const uint256& root, int height, const unsigned int invalid_rate, const unsigned int branch_rate, const unsigned int max_size, std::vector<std::shared_ptr<const CBlock>>& blocks)
{
    if (height <= 0 || blocks.size() >= max_size) return;

    bool gen_invalid = m_rng.randrange(100U) < invalid_rate;
    bool gen_fork = m_rng.randrange(100U) < branch_rate;

    const std::shared_ptr<const CBlock> pblock = gen_invalid ? BadBlock(root) : GoodBlock(root);
    blocks.push_back(pblock);
    if (!gen_invalid) {
        BuildChain(pblock->GetHash(), height - 1, invalid_rate, branch_rate, max_size, blocks);
    }

    if (gen_fork) {
        blocks.push_back(GoodBlock(root));
        BuildChain(blocks.back()->GetHash(), height - 1, invalid_rate, branch_rate, max_size, blocks);
    }
}

BOOST_AUTO_TEST_CASE(processnewblock_signals_ordering)
{
    // build a large-ish chain that's likely to have some forks
    std::vector<std::shared_ptr<const CBlock>> blocks;
    while (blocks.size() < 50) {
        blocks.clear();
        BuildChain(Params().GenesisBlock().GetHash(), 100, 15, 10, 500, blocks);
    }

    bool ignored;
    // Connect the genesis block and drain any outstanding events
    BOOST_CHECK(Assert(m_node.chainman)->ProcessNewBlock(std::make_shared<CBlock>(Params().GenesisBlock()), true, true, &ignored));
    m_node.validation_signals->SyncWithValidationInterfaceQueue();

    // subscribe to events (this subscriber will validate event ordering)
    const CBlockIndex* initial_tip = nullptr;
    {
        LOCK(cs_main);
        initial_tip = m_node.chainman->ActiveChain().Tip();
    }
    auto sub = std::make_shared<TestSubscriber>(initial_tip->GetBlockHash());
    m_node.validation_signals->RegisterSharedValidationInterface(sub);

    // create a bunch of threads that repeatedly process a block generated above at random
    // this will create parallelism and randomness inside validation - the ValidationInterface
    // will subscribe to events generated during block validation and assert on ordering invariance
    std::vector<std::thread> threads;
    threads.reserve(10);
    for (int i = 0; i < 10; i++) {
        threads.emplace_back([&]() {
            bool ignored;
            FastRandomContext insecure;
            for (int i = 0; i < 1000; i++) {
                const auto& block = blocks[insecure.randrange(blocks.size() - 1)];
                Assert(m_node.chainman)->ProcessNewBlock(block, true, true, &ignored);
            }

            // to make sure that eventually we process the full chain - do it here
            for (const auto& block : blocks) {
                if (block->vtx.size() == 1) {
                    bool processed = Assert(m_node.chainman)->ProcessNewBlock(block, true, true, &ignored);
                    assert(processed);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
    m_node.validation_signals->SyncWithValidationInterfaceQueue();

    m_node.validation_signals->UnregisterSharedValidationInterface(sub);

    LOCK(cs_main);
    BOOST_CHECK_EQUAL(sub->m_expected_tip, m_node.chainman->ActiveChain().Tip()->GetBlockHash());
}

/**
 * Test that mempool updates happen atomically with reorgs.
 *
 * This prevents RPC clients, among others, from retrieving immediately-out-of-date mempool data
 * during large reorgs.
 *
 * The test verifies this by creating a chain of `num_txs` blocks, matures their coinbases, and then
 * submits txns spending from their coinbase to the mempool. A fork chain is then processed,
 * invalidating the txns and evicting them from the mempool.
 *
 * We verify that the mempool updates atomically by polling it continuously
 * from another thread during the reorg and checking that its size only changes
 * once. The size changing exactly once indicates that the polling thread's
 * view of the mempool is either consistent with the chain state before reorg,
 * or consistent with the chain state after the reorg, and not just consistent
 * with some intermediate state during the reorg.
 */
BOOST_AUTO_TEST_CASE(mempool_locks_reorg)
{
    bool ignored;
    auto ProcessBlock = [&](std::shared_ptr<const CBlock> block) -> bool {
        return Assert(m_node.chainman)->ProcessNewBlock(block, /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/&ignored);
    };

    // Process all mined blocks
    BOOST_REQUIRE(ProcessBlock(std::make_shared<CBlock>(Params().GenesisBlock())));
    auto last_mined = GoodBlock(Params().GenesisBlock().GetHash());
    BOOST_REQUIRE(ProcessBlock(last_mined));

    // Run the test multiple times
    for (int test_runs = 3; test_runs > 0; --test_runs) {
        BOOST_CHECK_EQUAL(last_mined->GetHash(), WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip()->GetBlockHash()));

        // Later on split from here
        const uint256 split_hash{last_mined->hashPrevBlock};

        // Create a bunch of transactions to spend the miner rewards of the
        // most recent blocks
        std::vector<CTransactionRef> txs;
        for (int num_txs = 22; num_txs > 0; --num_txs) {
            CMutableTransaction mtx;
            mtx.vin.emplace_back(COutPoint{last_mined->vtx[0]->GetHash(), 1}, CScript{});
            mtx.vin[0].scriptWitness = CScriptWitness{{WITNESS_STACK_ELEM_OP_TRUE}};
            mtx.vout.push_back(last_mined->vtx[0]->vout[1]);
            mtx.vout[0].nValue -= 1000;
            txs.push_back(MakeTransactionRef(mtx));

            last_mined = GoodBlock(last_mined->GetHash());
            BOOST_REQUIRE(ProcessBlock(last_mined));
        }

        // Mature the inputs of the txs
        for (int j = COINBASE_MATURITY; j > 0; --j) {
            last_mined = GoodBlock(last_mined->GetHash());
            BOOST_REQUIRE(ProcessBlock(last_mined));
        }

        // Mine a reorg (and hold it back) before adding the txs to the mempool
        const uint256 tip_init{last_mined->GetHash()};

        std::vector<std::shared_ptr<const CBlock>> reorg;
        last_mined = GoodBlock(split_hash);
        reorg.push_back(last_mined);
        for (size_t j = COINBASE_MATURITY + txs.size() + 1; j > 0; --j) {
            last_mined = GoodBlock(last_mined->GetHash());
            reorg.push_back(last_mined);
        }

        // Add the txs to the tx pool
        {
            LOCK(cs_main);
            for (const auto& tx : txs) {
                const MempoolAcceptResult result = m_node.chainman->ProcessTransaction(tx);
                BOOST_REQUIRE(result.m_result_type == MempoolAcceptResult::ResultType::VALID);
            }
        }

        // Check that all txs are in the pool
        {
            BOOST_CHECK_EQUAL(m_node.mempool->size(), txs.size());
        }

        // Run a thread that simulates an RPC caller that is polling while
        // validation is doing a reorg
        std::thread rpc_thread{[&]() {
            // This thread is checking that the mempool either contains all of
            // the transactions invalidated by the reorg, or none of them, and
            // not some intermediate amount.
            while (true) {
                LOCK(m_node.mempool->cs);
                if (m_node.mempool->size() == 0) {
                    // We are done with the reorg
                    break;
                }
                // Internally, we might be in the middle of the reorg, but
                // externally the reorg to the most-proof-of-work chain should
                // be atomic. So the caller assumes that the returned mempool
                // is consistent. That is, it has all txs that were there
                // before the reorg.
                assert(m_node.mempool->size() == txs.size());
                continue;
            }
            LOCK(cs_main);
            // We are done with the reorg, so the tip must have changed
            assert(tip_init != m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        }};

        // Submit the reorg in this thread to invalidate and remove the txs from the tx pool
        for (const auto& b : reorg) {
            ProcessBlock(b);
        }
        // Check that the reorg was eventually successful
        BOOST_CHECK_EQUAL(last_mined->GetHash(), WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip()->GetBlockHash()));

        // We can join the other thread, which returns when the reorg was successful
        rpc_thread.join();
    }
}

BOOST_AUTO_TEST_CASE(witness_commitment_index)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    CScript pubKey;
    pubKey << 1 << OP_TRUE;
    auto& block_template_manager{*Assert(m_node.block_template_manager)};
    auto block_template{block_template_manager.CreateNewTemplate({
        .coinbase_output_script = pubKey,
    })};
    CBlock pblock{block_template->block};

    CTxOut witness;
    witness.scriptPubKey.resize(MINIMUM_WITNESS_COMMITMENT);
    witness.scriptPubKey[0] = OP_RETURN;
    witness.scriptPubKey[1] = 0x24;
    witness.scriptPubKey[2] = 0xaa;
    witness.scriptPubKey[3] = 0x21;
    witness.scriptPubKey[4] = 0xa9;
    witness.scriptPubKey[5] = 0xed;

    // A witness larger than the minimum size is still valid
    CTxOut min_plus_one = witness;
    min_plus_one.scriptPubKey.resize(MINIMUM_WITNESS_COMMITMENT + 1);

    CTxOut invalid = witness;
    invalid.scriptPubKey[0] = OP_VERIFY;

    CMutableTransaction txCoinbase(*pblock.vtx[0]);
    txCoinbase.vout.resize(4);
    txCoinbase.vout[0] = witness;
    txCoinbase.vout[1] = witness;
    txCoinbase.vout[2] = min_plus_one;
    txCoinbase.vout[3] = invalid;
    pblock.vtx[0] = MakeTransactionRef(std::move(txCoinbase));

    BOOST_CHECK_EQUAL(GetWitnessCommitmentIndex(pblock), 2);
}

BOOST_FIXTURE_TEST_CASE(switch_block_parent_target, RegTestingSetup)
{
    const auto& params{m_node.chainman->GetParams()};
    const auto& consensus{params.GetConsensus()};
    const auto& genesis{params.GenesisBlock()};
    const auto parent_hash{genesis.GetHash()};
    CBlockIndex parent{genesis};
    parent.phashBlock = &parent_hash;
    // Regtest disables retargeting, so this synthetic target cannot occur in a real regtest chain.
    // Its testnet-style 20-minute min-difficulty rule still applies, restoring the pow limit only above the threshold.
    --parent.nBits;

    const auto threshold{parent.nTime + uint32_t(2 * consensus.nPowTargetSpacing)};
    CBlock block{genesis};
    SwitchBlockParent(block, parent, threshold, consensus);
    BOOST_CHECK_EQUAL(block.nBits, parent.nBits);
    SwitchBlockParent(block, parent, threshold + 1, consensus);
    BOOST_CHECK_EQUAL(block.nBits, genesis.nBits);
}

BOOST_FIXTURE_TEST_CASE(switch_block_parent_coinbase, RegTestingSetup)
{
    auto& chainman{*Assert(m_node.chainman)};
    const auto& consensus{chainman.GetConsensus()};
    const CBlockIndex* genesis{WITH_LOCK(::cs_main, return chainman.ActiveChain().Genesis())};
    const int length{consensus.nSubsidyHalvingInterval};
    std::vector<std::shared_ptr<CBlock>> chain;
    BOOST_REQUIRE(BuildChain(m_node, genesis, CScript{} << OP_TRUE, length, chain));
    BOOST_CHECK_EQUAL(chain[0]->vtx[0]->vin[0].scriptSig.size(), 2);
    for (int i{0}; i < length; ++i) {
        const auto& block{chain[i]};
        BOOST_CHECK_EQUAL(block->vtx[0]->nLockTime, i);
        BOOST_CHECK_EQUAL(block->vtx[0]->vout[0].nValue, GetBlockSubsidy(i + 1, consensus));
        BOOST_REQUIRE(chainman.ProcessNewBlock(block, /*force_processing=*/true, /*min_pow_checked=*/true, nullptr));
    }
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainman.ActiveChain().Height()), length);
}

BOOST_FIXTURE_TEST_CASE(validate_block_with_flat_inputs_standalone, BasicTestingSetup)
{
    // Validate a block purely from (block, ctx, flat spent coins) with no ChainstateManager,
    // no UTXO store, no BlockManager, and no worker ThreadPool.
    const auto params{CreateChainParams(ArgsManager{}, ChainType::REGTEST)};
    const Consensus::Params& consensus{params->GetConsensus()};
    const CBlock& genesis{params->GenesisBlock()};
    const uint256 genesis_hash{genesis.GetHash()};

    CBlockIndex genesis_index{genesis};
    genesis_index.phashBlock = &genesis_hash;
    genesis_index.nHeight = 0;

    const CScript anyone_can_spend{CScript() << OP_TRUE};
    const CAmount subsidy{GetBlockSubsidy(1, consensus)};

    auto make_block = [&](CAmount coinbase_value) {
        CBlock block;
        block.nVersion = 0x20000000;
        block.hashPrevBlock = genesis_hash;
        block.nTime = genesis.nTime + 1;
        block.nBits = genesis.nBits;
        block.nNonce = 0;

        CMutableTransaction cb;
        cb.vin.resize(1);
        cb.vin[0].scriptSig = CScript() << 1 << OP_0;
        cb.vout.emplace_back(coinbase_value, anyone_can_spend);

        CMutableTransaction tx1;
        tx1.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
        tx1.vout.emplace_back(9000, anyone_can_spend);
        const CTransactionRef tx1_ref{MakeTransactionRef(std::move(tx1))};

        CMutableTransaction tx2;
        tx2.vin.emplace_back(COutPoint{tx1_ref->GetHash(), 0});
        tx2.vout.emplace_back(8500, anyone_can_spend);
        const CTransactionRef tx2_ref{MakeTransactionRef(std::move(tx2))};

        block.vtx = {MakeTransactionRef(std::move(cb)), tx1_ref, tx2_ref};
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, consensus)) {
            ++block.nNonce;
        }
        return block;
    };

    const CBlock valid_block{make_block(subsidy + 1500)};
    const uint256 valid_hash{valid_block.GetHash()};
    CBlockIndex tip_index{valid_block};
    tip_index.phashBlock = &valid_hash;
    tip_index.pprev = &genesis_index;
    tip_index.nHeight = 1;

    ValidationCache val_cache{1 << 20, 1 << 20};
    const std::vector<uint32_t> offsets{0, 0, 1, 2};
    auto make_inputs = [&](const CScript& ext_script) {
        return std::vector<Coin>{
            Coin{CTxOut{10000, ext_script}, /*nHeightIn=*/0, /*fCoinBaseIn=*/false},
            Coin{valid_block.vtx[1]->vout[0], /*nHeightIn=*/1, /*fCoinBaseIn=*/false},
        };
    };

    BlockValidationContext ctx{
        .pindex = &tip_index,
        .consensus_params = &consensus,
        .script_flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS,
        .locktime_flags = 0,
        .enforce_bip30 = false,
        .script_checks = true,
        .run_check_block = true,
        .check_pow_and_merkle = true,
        .build_undo = true,
        .cache_script_results = false,
        .validation_cache = &val_cache,
        .check_queue = nullptr,
    };

    // 1. Valid block with external spend + intra-block spend chain and CBlockUndo assembly.
    {
        auto inputs{make_inputs(anyone_can_spend)};
        BlockValidationState state;
        BlockValidationResultData res;
        BOOST_REQUIRE(ValidateBlockWithFlatInputs(valid_block, ctx, inputs, offsets, {}, state, res));
        BOOST_CHECK(state.IsValid());
        BOOST_CHECK_EQUAL(res.fees, 1500);
        BOOST_CHECK_EQUAL(res.inputs_count, 3);
        BOOST_REQUIRE_EQUAL(res.blockundo.vtxundo.size(), 2U);
        BOOST_CHECK_EQUAL(res.blockundo.vtxundo[0].vprevout[0].out.nValue, 10000);
        BOOST_CHECK_EQUAL(res.blockundo.vtxundo[1].vprevout[0].out.nValue, 9000);
    }

    // 2. Skipping CBlockUndo construction leaves blockundo empty while validating identically.
    {
        BlockValidationContext no_undo_ctx{ctx};
        no_undo_ctx.build_undo = false;
        auto inputs{make_inputs(anyone_can_spend)};
        BlockValidationState state;
        BlockValidationResultData res;
        BOOST_REQUIRE(ValidateBlockWithFlatInputs(valid_block, no_undo_ctx, inputs, offsets, {}, state, res));
        BOOST_CHECK_EQUAL(res.fees, 1500);
        BOOST_CHECK(res.blockundo.vtxundo.empty());
    }

    // 3. Missing/spent input slot fails consensus validation.
    {
        auto inputs{make_inputs(anyone_can_spend)};
        inputs[0] = Coin{};
        BlockValidationState state;
        BlockValidationResultData res;
        BOOST_CHECK(!ValidateBlockWithFlatInputs(valid_block, ctx, inputs, offsets, {}, state, res));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputs-missingorspent");
    }

    // 4. BIP30 callback conflict is rejected when enforce_bip30 is true.
    {
        BlockValidationContext bip30_ctx{ctx};
        bip30_ctx.enforce_bip30 = true;
        auto inputs{make_inputs(anyone_can_spend)};
        const COutPoint dup{valid_block.vtx[1]->GetHash(), 0};
        BlockValidationState state;
        BlockValidationResultData res;
        BOOST_CHECK(!ValidateBlockWithFlatInputs(
            valid_block, bip30_ctx, inputs, offsets,
            [&](const COutPoint& op) { return op == dup; },
            state, res));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-BIP30");
    }

    // 5. Script failure is rejected when script_checks is enabled.
    {
        auto inputs{make_inputs(CScript() << OP_FALSE)};
        BlockValidationState state;
        BlockValidationResultData res;
        BOOST_CHECK(!ValidateBlockWithFlatInputs(valid_block, ctx, inputs, offsets, {}, state, res));
        BOOST_CHECK(state.GetRejectReason().starts_with("block-script-verify-flag-failed"));
    }

    // 6. Overpaying coinbase reward fails bad-cb-amount.
    {
        const CBlock bad_cb_block{make_block(subsidy + 1501)};
        const uint256 bad_cb_hash{bad_cb_block.GetHash()};
        CBlockIndex bad_tip{bad_cb_block};
        bad_tip.phashBlock = &bad_cb_hash;
        bad_tip.pprev = &genesis_index;
        bad_tip.nHeight = 1;
        BlockValidationContext bad_ctx{ctx};
        bad_ctx.pindex = &bad_tip;
        auto inputs{make_inputs(anyone_can_spend)};
        BlockValidationState state;
        BlockValidationResultData res;
        BOOST_CHECK(!ValidateBlockWithFlatInputs(bad_cb_block, bad_ctx, inputs, offsets, {}, state, res));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amount");
    }

    // 7. Immature coinbase input fails bad-txns-premature-spend-of-coinbase.
    {
        auto inputs{make_inputs(anyone_can_spend)};
        inputs[0] = Coin{CTxOut{10000, anyone_can_spend}, /*nHeightIn=*/0, /*fCoinBaseIn=*/true};
        BlockValidationState state;
        BlockValidationResultData res;
        BOOST_CHECK(!ValidateBlockWithFlatInputs(valid_block, ctx, inputs, offsets, {}, state, res));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-premature-spend-of-coinbase");
    }

    // 8. Input value below output value fails bad-txns-in-belowout.
    {
        auto inputs{make_inputs(anyone_can_spend)};
        inputs[0] = Coin{CTxOut{8999, anyone_can_spend}, /*nHeightIn=*/0, /*fCoinBaseIn=*/false};
        BlockValidationState state;
        BlockValidationResultData res;
        BOOST_CHECK(!ValidateBlockWithFlatInputs(valid_block, ctx, inputs, offsets, {}, state, res));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-in-belowout");
    }

    // 9. Bad script is skipped when script_checks=false (assumevalid behavior), rejected when true.
    {
        BlockValidationContext skip_ctx{ctx};
        skip_ctx.script_checks = false;
        auto inputs{make_inputs(CScript() << OP_FALSE)};
        BlockValidationState state;
        BlockValidationResultData res;
        BOOST_CHECK(ValidateBlockWithFlatInputs(valid_block, skip_ctx, inputs, offsets, {}, state, res));
        BOOST_CHECK(state.IsValid());
    }

    // 10. cache_script_results=true populates ValidationCache and validates cleanly on repeat + concurrently.
    {
        BlockValidationContext cache_ctx{ctx};
        cache_ctx.cache_script_results = true;
        std::atomic<int> passed{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                CBlock blk_copy{valid_block};
                auto inputs{make_inputs(anyone_can_spend)};
                BlockValidationState state;
                BlockValidationResultData res;
                if (ValidateBlockWithFlatInputs(blk_copy, cache_ctx, inputs, offsets, {}, state, res) && res.fees == 1500) {
                    passed.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : threads) th.join();
        BOOST_CHECK_EQUAL(passed.load(), 4);
    }

    // 11. Corrupted merkle root fails CheckBlock() when run_check_block=true.
    {
        CBlock bad_merkle{valid_block};
        bad_merkle.fChecked = false;
        bad_merkle.m_checked_merkle_root = false;
        bad_merkle.hashMerkleRoot = uint256::ONE;
        while (!CheckProofOfWork(bad_merkle.GetHash(), bad_merkle.nBits, consensus)) {
            ++bad_merkle.nNonce;
        }
        const uint256 bad_hash{bad_merkle.GetHash()};
        CBlockIndex bad_idx{bad_merkle};
        bad_idx.phashBlock = &bad_hash;
        bad_idx.pprev = &genesis_index;
        bad_idx.nHeight = 1;
        BlockValidationContext bad_ctx{ctx};
        bad_ctx.pindex = &bad_idx;
        auto inputs{make_inputs(anyone_can_spend)};
        BlockValidationState state;
        BlockValidationResultData res;
        BOOST_CHECK(!ValidateBlockWithFlatInputs(bad_merkle, bad_ctx, inputs, offsets, {}, state, res));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txnmrklroot");
    }

    // 12. Excessive sigops fails bad-blk-sigops.
    {
        CBlock sigop_block;
        sigop_block.nVersion = 0x20000000;
        sigop_block.hashPrevBlock = genesis_hash;
        sigop_block.nTime = genesis.nTime + 1;
        sigop_block.nBits = genesis.nBits;
        sigop_block.nNonce = 0;

        CMutableTransaction cb;
        cb.vin.resize(1);
        cb.vin[0].scriptSig = CScript() << 1 << OP_0;
        cb.vout.emplace_back(subsidy, anyone_can_spend);

        // Each OP_CHECKMULTISIG counts as 20 legacy sigops * WITNESS_SCALE_FACTOR (4) = 80 sigop cost.
        // Put 100 OP_CHECKMULTISIG in a P2SH redeemScript inside 11 transactions -> 11 * 100 * 80 = 88,000 > MAX_BLOCK_SIGOPS_COST (80,000).
        CScript heavy_script;
        for (int k = 0; k < 100; ++k) heavy_script << OP_CHECKMULTISIG;
        CMutableTransaction spam_tx;
        spam_tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
        for (int o = 0; o < 11; ++o) {
            spam_tx.vout.emplace_back(100, heavy_script);
        }
        sigop_block.vtx = {MakeTransactionRef(std::move(cb)), MakeTransactionRef(std::move(spam_tx))};
        sigop_block.hashMerkleRoot = BlockMerkleRoot(sigop_block);
        while (!CheckProofOfWork(sigop_block.GetHash(), sigop_block.nBits, consensus)) {
            ++sigop_block.nNonce;
        }
        const uint256 sigop_hash{sigop_block.GetHash()};
        CBlockIndex sigop_idx{sigop_block};
        sigop_idx.phashBlock = &sigop_hash;
        sigop_idx.pprev = &genesis_index;
        sigop_idx.nHeight = 1;
        BlockValidationContext sigop_ctx{ctx};
        sigop_ctx.pindex = &sigop_idx;
        sigop_ctx.script_checks = false;
        std::vector<Coin> sigop_inputs{Coin{CTxOut{2000, anyone_can_spend}, 0, false}};
        std::vector<uint32_t> sigop_offsets{0, 0, 1};
        BlockValidationState state;
        BlockValidationResultData res;
        BOOST_CHECK(!ValidateBlockWithFlatInputs(sigop_block, sigop_ctx, sigop_inputs, sigop_offsets, {}, state, res));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-sigops");
    }

    // 13. ShouldEnforceBIP30() boundary matrix on mainnet consensus params.
    {
        const auto mainnet{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
        const auto& mp{mainnet->GetConsensus()};
        const uint256 dummy_hash{uint256::ONE};
        const uint256 bip30_91842{"00000000000a4d0a398161ffc163c503763b1f4360639393e0e4c8e300e0caec"};
        const uint256 bip30_91880{"00000000000743f190a18c5577a3c2d2a1f610ae9601ac046a38084ccb7cd721"};

        CBlockIndex bip34_good;
        bip34_good.nHeight = mp.BIP34Height;
        bip34_good.phashBlock = &mp.BIP34Hash;

        CBlockIndex bip34_bad;
        bip34_bad.nHeight = mp.BIP34Height;
        bip34_bad.phashBlock = &dummy_hash;

        auto check_bip30 = [&](int height, CBlockIndex* parent, const uint256& hash) {
            CBlockIndex idx;
            idx.pprev = parent;
            idx.pskip = parent;
            idx.nHeight = height;
            idx.phashBlock = &hash;
            return ShouldEnforceBIP30(idx, mp);
        };

        CBlockIndex pre_bip34_parent;
        pre_bip34_parent.nHeight = 99;
        pre_bip34_parent.phashBlock = &dummy_hash;
        BOOST_CHECK(check_bip30(100, &pre_bip34_parent, dummy_hash));
        BOOST_CHECK(!check_bip30(91842, &pre_bip34_parent, bip30_91842));
        BOOST_CHECK(!check_bip30(91880, &pre_bip34_parent, bip30_91880));

        // Block at BIP34Height itself still enforces BIP30 (parent is below BIP34Height).
        CBlockIndex bip34_minus_one;
        bip34_minus_one.nHeight = mp.BIP34Height - 1;
        bip34_minus_one.phashBlock = &dummy_hash;
        BOOST_CHECK(check_bip30(mp.BIP34Height, &bip34_minus_one, mp.BIP34Hash));

        // After BIP34Height: skipped iff ancestor at BIP34Height matches BIP34Hash.
        BOOST_CHECK(!check_bip30(mp.BIP34Height + 1, &bip34_good, dummy_hash));
        BOOST_CHECK(check_bip30(mp.BIP34Height + 1, &bip34_bad, dummy_hash));

        // Verify the 1,983,701 vs 1,983,702 BIP34_IMPLIES_BIP30_LIMIT boundary directly.
        Consensus::Params limit_params{mp};
        limit_params.BIP34Height = 1983700;
        CBlockIndex bip34_at_limit;
        bip34_at_limit.nHeight = 1983700;
        bip34_at_limit.phashBlock = &limit_params.BIP34Hash;
        {
            CBlockIndex at_1983701;
            at_1983701.pprev = &bip34_at_limit;
            at_1983701.nHeight = 1983701;
            at_1983701.phashBlock = &dummy_hash;
            BOOST_CHECK(!ShouldEnforceBIP30(at_1983701, limit_params));

            CBlockIndex at_1983702;
            at_1983702.pprev = &at_1983701;
            at_1983702.nHeight = 1983702;
            at_1983702.phashBlock = &dummy_hash;
            BOOST_CHECK(ShouldEnforceBIP30(at_1983702, limit_params));
        }
    }
}
BOOST_AUTO_TEST_SUITE_END()
