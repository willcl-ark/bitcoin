// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/license/mit/.

#include <addresstype.h>
#include <bench/bench.h>
#include <index/base.h>
#include <index/scripthashindex.h>
#include <interfaces/chain.h>
#include <pubkey.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <util/time.h>
#include <validation.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {
constexpr int MINI_CHAIN_EXTRA_BLOCKS{160};
constexpr int MINI_CHAIN_TXS_PER_BLOCK{8};
constexpr size_t MINI_HOT_SCRIPTHASHES{32};
constexpr size_t MINI_QUERY_BATCH_SIZE{8};
constexpr CAmount MINI_TX_FEE{1000};

struct SpendableCoin {
    CTransactionRef tx;
    uint32_t vout;
    int height;
    CKey key;
};

static CKey DeterministicKey(size_t index)
{
    std::array<unsigned char, 32> key_data{};
    key_data[30] = static_cast<unsigned char>(index >> 8);
    key_data[31] = static_cast<unsigned char>(index + 1);

    CKey key;
    key.Set(key_data.begin(), key_data.end(), true);
    return key;
}

static CScript ScriptForKey(const CKey& key)
{
    return GetScriptForDestination(WitnessV0KeyHash(key.GetPubKey()));
}

class MiniScriptHashIndexFixture
{
public:
    MiniScriptHashIndexFixture()
        : m_test_setup{MakeNoLogFileContext<TestChain100Setup>()}
    {
        BuildHotSet();
        SeedSpendables();
        BuildMiniChain();
    }

    std::unique_ptr<ScriptHashIndex> MakeSyncedIndex(bool memory, bool wipe)
    {
        auto index = std::make_unique<ScriptHashIndex>(interfaces::MakeChain(m_test_setup->m_node),
                                                       /*n_cache_size=*/1 << 20,
                                                       memory,
                                                       wipe);
        assert(index->Init());
        if (!index->BlockUntilSyncedToCurrentChain()) {
            index->Sync();
        }

        IndexSummary summary = index->GetSummary();
        assert(summary.synced);
        assert(summary.best_block_hash == WITH_LOCK(::cs_main, return m_test_setup->m_node.chainman->ActiveTip()->GetBlockHash()));
        return index;
    }

    const std::vector<uint256>& HotScripthashes() const { return m_hot_scripthashes; }

    void WarmHotSetHistory(ScriptHashIndex& index) const
    {
        for (const auto& scripthash : m_hot_scripthashes) {
            ankerl::nanobench::doNotOptimizeAway(index.GetHistory(scripthash));
        }
    }

    void WarmHotSetState(ScriptHashIndex& index) const
    {
        for (const auto& scripthash : m_hot_scripthashes) {
            ankerl::nanobench::doNotOptimizeAway(index.GetUtxos(scripthash));
            ankerl::nanobench::doNotOptimizeAway(index.GetBalance(scripthash));
        }
    }

private:
    void BuildHotSet()
    {
        m_hot_keys.reserve(MINI_HOT_SCRIPTHASHES);
        m_hot_scripts.reserve(MINI_HOT_SCRIPTHASHES);
        m_hot_scripthashes.reserve(MINI_HOT_SCRIPTHASHES);

        for (size_t i = 0; i < MINI_HOT_SCRIPTHASHES; ++i) {
            CKey key = DeterministicKey(i + 1);
            CScript script = ScriptForKey(key);
            m_hot_keys.push_back(key);
            m_hot_scripthashes.push_back(ComputeScriptHashIndexHash(script));
            m_hot_scripts.push_back(std::move(script));
        }
    }

    void SeedSpendables()
    {
        for (size_t i = 0; i < m_test_setup->m_coinbase_txns.size(); ++i) {
            m_spendable.push_back({
                m_test_setup->m_coinbase_txns.at(i),
                /*vout=*/0,
                static_cast<int>(i + 1),
                m_test_setup->coinbaseKey,
            });
        }
    }

    void BuildMiniChain()
    {
        const CScript coinbase_script = m_test_setup->m_coinbase_txns.front()->vout[0].scriptPubKey;

        for (int block_num = 0; block_num < MINI_CHAIN_EXTRA_BLOCKS; ++block_num) {
            std::vector<CMutableTransaction> transactions;
            std::vector<size_t> destination_indexes;
            transactions.reserve(MINI_CHAIN_TXS_PER_BLOCK);
            destination_indexes.reserve(MINI_CHAIN_TXS_PER_BLOCK);

            for (int tx_num = 0; tx_num < MINI_CHAIN_TXS_PER_BLOCK; ++tx_num) {
                assert(!m_spendable.empty());

                const SpendableCoin spendable = m_spendable.front();
                m_spendable.pop_front();

                const size_t dest_index = static_cast<size_t>(block_num * MINI_CHAIN_TXS_PER_BLOCK + tx_num) % m_hot_scripts.size();
                const CAmount input_value = spendable.tx->vout.at(spendable.vout).nValue;
                assert(input_value > MINI_TX_FEE);

                const std::vector<CTxOut> outputs{{input_value - MINI_TX_FEE, m_hot_scripts.at(dest_index)}};
                auto [tx, fee] = m_test_setup->CreateValidTransaction(
                    /*input_transactions=*/{spendable.tx},
                    /*inputs=*/{COutPoint{spendable.tx->GetHash(), spendable.vout}},
                    /*input_height=*/spendable.height,
                    /*input_signing_keys=*/{spendable.key},
                    /*outputs=*/outputs,
                    /*feerate=*/std::nullopt,
                    /*fee_output=*/std::nullopt);
                assert(fee == MINI_TX_FEE);

                transactions.push_back(std::move(tx));
                destination_indexes.push_back(dest_index);
            }

            const CBlock block = m_test_setup->CreateAndProcessBlock(transactions, coinbase_script);
            const int height = WITH_LOCK(::cs_main, return m_test_setup->m_node.chainman->ActiveHeight());

            for (size_t i = 1; i < block.vtx.size(); ++i) {
                const size_t dest_index = destination_indexes.at(i - 1);
                m_spendable.push_back({
                    block.vtx.at(i),
                    /*vout=*/0,
                    height,
                    m_hot_keys.at(dest_index),
                });
            }

            SetMockTime(GetTime() + 1);
        }
    }

    std::unique_ptr<TestChain100Setup> m_test_setup;
    std::vector<CKey> m_hot_keys;
    std::vector<CScript> m_hot_scripts;
    std::vector<uint256> m_hot_scripthashes;
    std::deque<SpendableCoin> m_spendable;
};

// Small deterministic script hash index workload:
// - start from the standard 100 matured regtest coinbases
// - cycle spendable outputs through a fixed hot set of scripts
// - keep enough history per scripthash to exercise both funding and spending scans
static void ScriptHashIndexSyncMini(benchmark::Bench& bench)
{
    MiniScriptHashIndexFixture fixture;

    bench.minEpochIterations(5).unit("sync").run([&] {
        auto index = fixture.MakeSyncedIndex(/*memory=*/false, /*wipe=*/true);
        ankerl::nanobench::doNotOptimizeAway(index->GetSummary());
    });
}

static void ScriptHashIndexHistoryMini(benchmark::Bench& bench)
{
    MiniScriptHashIndexFixture fixture;
    auto index = fixture.MakeSyncedIndex(/*memory=*/false, /*wipe=*/true);
    fixture.WarmHotSetHistory(*index);

    size_t cursor{0};
    bench.batch(MINI_QUERY_BATCH_SIZE).unit("query").minEpochIterations(10'000).run([&] {
        for (size_t i = 0; i < MINI_QUERY_BATCH_SIZE; ++i) {
            const uint256& scripthash = fixture.HotScripthashes().at(cursor);
            cursor = (cursor + 1) % fixture.HotScripthashes().size();

            const auto history = index->GetHistory(scripthash);
            ankerl::nanobench::doNotOptimizeAway(history);
        }
    });
}

static void ScriptHashIndexHistoryMiniCold(benchmark::Bench& bench)
{
    MiniScriptHashIndexFixture fixture;
    {
        auto index = fixture.MakeSyncedIndex(/*memory=*/false, /*wipe=*/true);
        ankerl::nanobench::doNotOptimizeAway(index->GetSummary());
    }

    size_t cursor{0};
    // Re-open the synced DB each iteration to keep the query path cold.
    bench.batch(MINI_QUERY_BATCH_SIZE).unit("query").minEpochIterations(100).run([&] {
        auto index = fixture.MakeSyncedIndex(/*memory=*/false, /*wipe=*/false);
        for (size_t i = 0; i < MINI_QUERY_BATCH_SIZE; ++i) {
            const uint256& scripthash = fixture.HotScripthashes().at(cursor);
            cursor = (cursor + 1) % fixture.HotScripthashes().size();

            const auto history = index->GetHistory(scripthash);
            ankerl::nanobench::doNotOptimizeAway(history);
        }
    });
}

static void ScriptHashIndexUtxoMini(benchmark::Bench& bench)
{
    MiniScriptHashIndexFixture fixture;
    auto index = fixture.MakeSyncedIndex(/*memory=*/false, /*wipe=*/true);
    fixture.WarmHotSetState(*index);

    size_t cursor{0};
    bench.batch(MINI_QUERY_BATCH_SIZE).unit("query").minEpochIterations(10'000).run([&] {
        for (size_t i = 0; i < MINI_QUERY_BATCH_SIZE; ++i) {
            const uint256& scripthash = fixture.HotScripthashes().at(cursor);
            cursor = (cursor + 1) % fixture.HotScripthashes().size();

            const auto utxos = index->GetUtxos(scripthash);
            const auto balance = index->GetBalance(scripthash);
            ankerl::nanobench::doNotOptimizeAway(utxos);
            ankerl::nanobench::doNotOptimizeAway(balance);
        }
    });
}
} // namespace

BENCHMARK(ScriptHashIndexSyncMini);
BENCHMARK(ScriptHashIndexHistoryMini);
BENCHMARK(ScriptHashIndexHistoryMiniCold);
BENCHMARK(ScriptHashIndexUtxoMini);
