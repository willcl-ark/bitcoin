// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/window_outputs.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace {

std::shared_ptr<CBlock> MakeBlockWithTx(CAmount val, bool unspendable = false)
{
    auto block{std::make_shared<CBlock>()};
    CMutableTransaction cb;
    cb.vin.emplace_back(COutPoint{});
    std::vector<unsigned char> script_bytes(25, static_cast<unsigned char>(OP_TRUE));
    cb.vout.emplace_back(val, unspendable ? (CScript() << OP_RETURN) : CScript(script_bytes.begin(), script_bytes.end()));
    block->vtx.push_back(MakeTransactionRef(cb));
    return block;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(window_outputs_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(visibility_release_and_fifo_sweep)
{
    node::WindowOutputs outputs;
    outputs.SetPublished(10);

    auto b11{MakeBlockWithTx(1100)};
    auto b12{MakeBlockWithTx(1200)};
    auto b13{MakeBlockWithTx(1300, /*unspendable=*/true)};

    outputs.Index(11, b11);
    outputs.Index(12, b12);
    outputs.Index(13, b13);

    const COutPoint op11{b11->vtx[0]->GetHash(), 0};
    const COutPoint op12{b12->vtx[0]->GetHash(), 0};
    const COutPoint op13{b13->vtx[0]->GetHash(), 0};

    // Viewer at height 12 sees height 11, not height 12 or 13.
    std::optional<Coin> coin;
    BOOST_CHECK(outputs.FindCoin(op11, 12, coin));
    BOOST_REQUIRE(coin.has_value());
    BOOST_CHECK_EQUAL(coin->out.nValue, 1100);
    BOOST_CHECK_EQUAL(coin->nHeight, 11U);
    BOOST_CHECK(coin->IsCoinBase());

    BOOST_CHECK(!outputs.FindCoin(op12, 12, coin));
    BOOST_CHECK(!outputs.FindCoin(op13, 12, coin));

    // Viewer at height 14 sees unspendable op13 as decided (returns true) with empty coin.
    BOOST_CHECK(outputs.FindCoin(op13, 14, coin));
    BOOST_CHECK(!coin.has_value());

    // Batch FindInputs decides lower-block outputs (including unspendable) and leaves unknown ones undecided.
    FastRandomContext rng{/*fDeterministic=*/true};
    const COutPoint op_unknown{Txid::FromUint256(rng.rand256()), 0};
    std::vector<COutPoint> lookup{op11, op_unknown, op12, op13};
    std::vector<uint32_t> positions{1, 1, 1, 1};
    std::vector<uint8_t> decided(lookup.size(), 0);
    std::vector<Coin> resolved(lookup.size());
    outputs.FindInputs(lookup, positions, 14, decided, resolved);
    BOOST_CHECK_EQUAL(decided[0], 1U);
    BOOST_CHECK_EQUAL(resolved[0].out.nValue, 1100);
    BOOST_CHECK_EQUAL(decided[1], 0U);
    BOOST_CHECK_EQUAL(decided[2], 1U);
    BOOST_CHECK_EQUAL(resolved[2].out.nValue, 1200);
    BOOST_CHECK_EQUAL(decided[3], 1U);
    BOOST_CHECK(resolved[3].IsSpent());

    // NamedIds flags transactions from strictly lower unpublished blocks.
    std::vector<CTransactionRef> txs{b11->vtx[0], b12->vtx[0]};
    std::vector<uint8_t> named;
    outputs.NamedIds(txs, 12, named);
    BOOST_REQUIRE_EQUAL(named.size(), 2U);
    BOOST_CHECK_EQUAL(named[0], 1U);
    BOOST_CHECK_EQUAL(named[1], 0U);

    // Releasing height 11 hides it immediately even before Sweep().
    outputs.Release(11);
    BOOST_CHECK_EQUAL(outputs.ReleasedCount(), 1U);
    BOOST_CHECK(!outputs.FindCoin(op11, 14, coin));
    BOOST_CHECK(outputs.FindCoin(op12, 14, coin));

    outputs.Release(12);
    outputs.Release(13);
    BOOST_CHECK_EQUAL(outputs.ReleasedCount(), 3U);
    outputs.Sweep(/*max_blocks=*/1);
    BOOST_CHECK_EQUAL(outputs.ReleasedCount(), 2U);
    outputs.Sweep();
    BOOST_CHECK_EQUAL(outputs.ReleasedCount(), 0U);
    BOOST_CHECK(!outputs.FindCoin(op12, 14, coin));
}

BOOST_AUTO_TEST_CASE(find_inputs_sees_earlier_txs_in_same_block)
{
    node::WindowOutputs outputs;
    outputs.SetPublished(10);

    auto block{std::make_shared<CBlock>()};
    CMutableTransaction tx0;
    tx0.vin.emplace_back(COutPoint{});
    tx0.vout.emplace_back(5000, CScript() << OP_TRUE);
    auto ref0{MakeTransactionRef(tx0)};

    CMutableTransaction tx1;
    tx1.vin.emplace_back(COutPoint{ref0->GetHash(), 0});
    tx1.vout.emplace_back(4000, CScript() << OP_TRUE);
    auto ref1{MakeTransactionRef(tx1)};

    block->vtx = {ref0, ref1};
    outputs.Index(11, block);

    const std::vector<COutPoint> ops{COutPoint{ref0->GetHash(), 0}, COutPoint{ref1->GetHash(), 0}};
    const std::vector<uint32_t> pos{1, 1};
    std::vector<uint8_t> decided(2, 0);
    std::vector<Coin> coins(2);
    outputs.FindInputs(ops, pos, /*viewer_height=*/11, decided, coins);

    // Position 1 sees tx 0 from the same block, but not tx 1 itself.
    BOOST_CHECK_EQUAL(decided[0], 1U);
    BOOST_CHECK_EQUAL(coins[0].out.nValue, 5000);
    BOOST_CHECK_EQUAL(coins[0].nHeight, 11U);
    BOOST_CHECK_EQUAL(decided[1], 0U);
    BOOST_CHECK(coins[1].IsSpent());
}

BOOST_AUTO_TEST_SUITE_END()
