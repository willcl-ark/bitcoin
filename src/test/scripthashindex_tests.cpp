// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <index/scripthashindex.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(scripthashindex_tests)

BOOST_FIXTURE_TEST_CASE(scripthashindex_initial_sync, TestChain100Setup)
{
    ScriptHashIndex index(interfaces::MakeChain(m_node), 1 << 20, true);
    BOOST_REQUIRE(index.Init());

    const CScript coinbase_spk = m_coinbase_txns[0]->vout[0].scriptPubKey;
    const uint256 coinbase_sh = ComputeElectrumScriptHash(coinbase_spk);

    // Mine blocks for coinbase maturity
    for (int i = 0; i < 50; i++) {
        std::vector<CMutableTransaction> no_txns;
        CreateAndProcessBlock(no_txns, coinbase_spk);
    }

    // Create a spending transaction: spend coinbase[0] to a new script
    CScript dest_spk = CScript() << OP_TRUE;
    const uint256 dest_sh = ComputeElectrumScriptHash(dest_spk);

    CMutableTransaction spend_tx;
    spend_tx.version = 1;
    spend_tx.vin.resize(1);
    spend_tx.vin[0].prevout = COutPoint(m_coinbase_txns[0]->GetHash(), 0);
    spend_tx.vout.resize(1);
    spend_tx.vout[0].nValue = m_coinbase_txns[0]->GetValueOut();
    spend_tx.vout[0].scriptPubKey = dest_spk;

    // Sign
    std::vector<unsigned char> vchSig;
    const uint256 hash = SignatureHash(coinbase_spk, spend_tx, 0, SIGHASH_ALL, 0, SigVersion::BASE);
    coinbaseKey.Sign(hash, vchSig);
    vchSig.push_back((unsigned char)SIGHASH_ALL);
    spend_tx.vin[0].scriptSig << vchSig;

    std::vector<CMutableTransaction> txns{spend_tx};
    CBlock block = CreateAndProcessBlock(txns, coinbase_spk);

    BOOST_CHECK(!index.BlockUntilSyncedToCurrentChain());

    index.Sync();

    // The coinbase scripthash should have history entries (all coinbase outputs use the same key)
    auto history = index.GetHistory(coinbase_sh);
    BOOST_CHECK(!history.empty());

    // The destination scripthash should have exactly one history entry
    auto dest_history = index.GetHistory(dest_sh);
    BOOST_CHECK_EQUAL(dest_history.size(), 1U);
    BOOST_CHECK_EQUAL(dest_history[0].txid, spend_tx.GetHash());

    // The destination scripthash should have one UTXO
    auto dest_utxos = index.GetUtxos(dest_sh);
    BOOST_CHECK_EQUAL(dest_utxos.size(), 1U);
    BOOST_CHECK_EQUAL(dest_utxos[0].outpoint.hash, spend_tx.GetHash());
    BOOST_CHECK_EQUAL(dest_utxos[0].outpoint.n, 0U);
    BOOST_CHECK_EQUAL(dest_utxos[0].value, m_coinbase_txns[0]->GetValueOut());

    // The balance for the destination should match
    BOOST_CHECK_EQUAL(index.GetBalance(dest_sh), m_coinbase_txns[0]->GetValueOut());

    // The spent coinbase output should no longer be in the UTXO set for the coinbase scripthash
    auto coinbase_utxos = index.GetUtxos(coinbase_sh);
    for (const auto& utxo : coinbase_utxos) {
        BOOST_CHECK(!(utxo.outpoint.hash == m_coinbase_txns[0]->GetHash() && utxo.outpoint.n == 0));
    }

    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    index.Stop();
}

BOOST_FIXTURE_TEST_CASE(scripthashindex_reorg_rollback, TestChain100Setup)
{
    ScriptHashIndex index(interfaces::MakeChain(m_node), 1 << 20, true);
    BOOST_REQUIRE(index.Init());

    const CScript coinbase_spk = m_coinbase_txns[0]->vout[0].scriptPubKey;
    const uint256 coinbase_sh = ComputeElectrumScriptHash(coinbase_spk);

    for (int i = 0; i < 50; i++) {
        CreateAndProcessBlock({}, coinbase_spk);
    }

    CScript dest_spk = CScript() << OP_TRUE;
    const uint256 dest_sh = ComputeElectrumScriptHash(dest_spk);

    CMutableTransaction spend_tx;
    spend_tx.version = 1;
    spend_tx.vin.resize(1);
    spend_tx.vin[0].prevout = COutPoint(m_coinbase_txns[0]->GetHash(), 0);
    spend_tx.vout.resize(1);
    spend_tx.vout[0].nValue = m_coinbase_txns[0]->GetValueOut();
    spend_tx.vout[0].scriptPubKey = dest_spk;

    std::vector<unsigned char> vchSig;
    const uint256 hash = SignatureHash(coinbase_spk, spend_tx, 0, SIGHASH_ALL, 0, SigVersion::BASE);
    coinbaseKey.Sign(hash, vchSig);
    vchSig.push_back((unsigned char)SIGHASH_ALL);
    spend_tx.vin[0].scriptSig << vchSig;

    CBlock spend_block = CreateAndProcessBlock({spend_tx}, coinbase_spk);
    BOOST_CHECK(!index.BlockUntilSyncedToCurrentChain());
    index.Sync();

    auto dest_utxos = index.GetUtxos(dest_sh);
    BOOST_REQUIRE_EQUAL(dest_utxos.size(), 1U);
    BOOST_CHECK_EQUAL(dest_utxos[0].outpoint.hash, spend_tx.GetHash());

    BlockValidationState state;
    CBlockIndex* tip_before_disconnect{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip())};
    BOOST_REQUIRE(tip_before_disconnect);
    BOOST_REQUIRE_EQUAL(tip_before_disconnect->GetBlockHash(), spend_block.GetHash());
    m_node.chainman->ActiveChainstate().InvalidateBlock(state, tip_before_disconnect);
    BOOST_REQUIRE(state.IsValid());

    CreateAndProcessBlock({}, coinbase_spk);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK(index.BlockUntilSyncedToCurrentChain());

    BOOST_CHECK(index.GetHistory(dest_sh).empty());
    BOOST_CHECK(index.GetUtxos(dest_sh).empty());
    BOOST_CHECK_EQUAL(index.GetBalance(dest_sh), 0);

    bool restored_outpoint{false};
    for (const auto& utxo : index.GetUtxos(coinbase_sh)) {
        if (utxo.outpoint.hash == m_coinbase_txns[0]->GetHash() && utxo.outpoint.n == 0) {
            restored_outpoint = true;
            BOOST_CHECK_EQUAL(utxo.value, m_coinbase_txns[0]->GetValueOut());
            break;
        }
    }
    BOOST_CHECK(restored_outpoint);

    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    index.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
