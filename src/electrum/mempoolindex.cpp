// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <electrum/mempoolindex.h>

#include <index/scripthashindex.h>
#include <kernel/mempool_entry.h>
#include <txmempool.h>

#include <algorithm>
#include <bit>
#include <map>

namespace {
int64_t FeeHistogramLabel(CAmount fee, int64_t vsize)
{
    const int64_t fee_rate = fee / std::max<int64_t>(vsize, 1);
    if (fee_rate <= 0) return 0;
    return (int64_t{1} << std::bit_width(static_cast<uint64_t>(fee_rate))) - 1;
}
} // namespace

void MarkDirtyForInput(const COutPoint& prevout,
                      const std::unordered_map<COutPoint, uint256, SaltedOutpointHasher>& outpoint_to_scripthash,
                      std::set<uint256>& dirty_scripthashes,
                      bool& dirty_all)
{
    const auto outpoint_it = outpoint_to_scripthash.find(prevout);
    if (outpoint_it != outpoint_to_scripthash.end()) {
        dirty_scripthashes.insert(outpoint_it->second);
        return;
    }
    // Avoid DB reads while m_mutex is held by callers.
    dirty_all = true;
}

void MempoolScriptHashIndex::AddTx(const CTransactionRef& tx, CAmount fee, int64_t vsize, bool has_unconfirmed_inputs)
{
    const Txid& txid = tx->GetHash();
    if (m_entries.count(txid)) return;

    const int64_t fee_histogram_label = FeeHistogramLabel(fee, vsize);
    m_entries[txid] = {tx, fee, vsize, fee_histogram_label, has_unconfirmed_inputs};
    m_fee_histogram[fee_histogram_label] += vsize;

    for (uint32_t n = 0; n < tx->vout.size(); ++n) {
        uint256 sh = ComputeElectrumScriptHash(tx->vout[n].scriptPubKey);
        m_by_funding.emplace(sh, txid);
        m_outpoint_to_scripthash[COutPoint{txid, n}] = sh;
        m_dirty_scripthashes.insert(sh);
    }

    for (const auto& txin : tx->vin) {
        m_by_spending.emplace(txin.prevout, txid);
        MarkDirtyForInput(txin.prevout, m_outpoint_to_scripthash, m_dirty_scripthashes, m_dirty_all);
    }
}

void MempoolScriptHashIndex::RemoveTx(const Txid& txid)
{
    auto it = m_entries.find(txid);
    if (it == m_entries.end()) return;

    const CTransactionRef& tx = it->second.tx;
    const int64_t fee_histogram_label = it->second.fee_histogram_label;
    const int64_t vsize = it->second.vsize;

    for (const auto& txout : tx->vout) {
        uint256 sh = ComputeElectrumScriptHash(txout.scriptPubKey);
        m_by_funding.erase({sh, txid});
        m_dirty_scripthashes.insert(sh);
    }
    for (uint32_t n = 0; n < tx->vout.size(); ++n) {
        m_outpoint_to_scripthash.erase(COutPoint{txid, n});
    }

    for (const auto& txin : tx->vin) {
        m_by_spending.erase({txin.prevout, txid});
        MarkDirtyForInput(txin.prevout, m_outpoint_to_scripthash, m_dirty_scripthashes, m_dirty_all);
    }

    auto histogram_it = m_fee_histogram.find(fee_histogram_label);
    if (histogram_it != m_fee_histogram.end()) {
        histogram_it->second -= vsize;
        if (histogram_it->second <= 0) {
            m_fee_histogram.erase(histogram_it);
        }
    }

    m_entries.erase(it);
}

void MempoolScriptHashIndex::TransactionAddedToMempool(const NewMempoolTransactionInfo& tx_info, uint64_t /*mempool_sequence*/)
{
    LOCK(m_mutex);
    AddTx(tx_info.info.m_tx, tx_info.info.m_fee, tx_info.info.m_virtual_transaction_size, !tx_info.m_has_no_mempool_parents);
}

void MempoolScriptHashIndex::TransactionRemovedFromMempool(const CTransactionRef& tx, MemPoolRemovalReason /*reason*/, uint64_t /*mempool_sequence*/)
{
    LOCK(m_mutex);
    RemoveTx(tx->GetHash());
}

void MempoolScriptHashIndex::MempoolTransactionsRemovedForBlock(const std::vector<RemovedMempoolTransactionInfo>& txs_removed_for_block, unsigned int /*nBlockHeight*/)
{
    LOCK(m_mutex);
    for (const auto& tx_info : txs_removed_for_block) {
        RemoveTx(tx_info.info.m_tx->GetHash());
    }
}

void MempoolScriptHashIndex::LoadFromMempool(const CTxMemPool& mempool)
{
    struct TxSnapshot {
        CTransactionRef tx;
        CAmount fee;
        int64_t vsize;
        bool has_unconfirmed_inputs;
    };

    LOCK(m_mutex);
    std::vector<TxSnapshot> snapshot;
    {
        LOCK(mempool.cs);
        const auto& txs_by_txid = mempool.mapTx.get<0>();
        snapshot.reserve(txs_by_txid.size());
        for (const auto& entry : txs_by_txid) {
            snapshot.push_back({entry.GetSharedTx(), entry.GetModifiedFee(), entry.GetTxSize(), !mempool.HasNoInputsOf(entry.GetTx())});
        }
    }

    for (const auto& s : snapshot) {
        AddTx(s.tx, s.fee, s.vsize, s.has_unconfirmed_inputs);
    }
}

void MempoolScriptHashIndex::CollectMatchingTxs(const uint256& scripthash,
    const std::vector<COutPoint>& confirmed_outpoints,
    std::set<Txid>& result_txids,
    std::vector<std::pair<COutPoint, CAmount>>& mempool_outpoints) const
{
    auto funding_it = m_by_funding.lower_bound({scripthash, Txid{}});
    while (funding_it != m_by_funding.end() && funding_it->first == scripthash) {
        result_txids.insert(funding_it->second);
        ++funding_it;
    }

    for (const auto& txid : result_txids) {
        auto entry_it = m_entries.find(txid);
        if (entry_it == m_entries.end()) continue;
        const auto& tx = entry_it->second.tx;
        for (uint32_t n = 0; n < tx->vout.size(); ++n) {
            if (ComputeElectrumScriptHash(tx->vout[n].scriptPubKey) == scripthash) {
                mempool_outpoints.emplace_back(COutPoint(txid, n), tx->vout[n].nValue);
            }
        }
    }

    auto find_spenders = [this](const auto& outpoints, std::set<Txid>& txids) EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
        for (const auto& [outpoint, value] : outpoints) {
            auto spending_it = m_by_spending.lower_bound({outpoint, Txid{}});
            while (spending_it != m_by_spending.end() && spending_it->first == outpoint) {
                txids.insert(spending_it->second);
                ++spending_it;
            }
        }
    };

    std::vector<std::pair<COutPoint, CAmount>> confirmed_with_values;
    confirmed_with_values.reserve(confirmed_outpoints.size());
    for (const auto& op : confirmed_outpoints) {
        confirmed_with_values.emplace_back(op, CAmount{0});
    }
    find_spenders(confirmed_with_values, result_txids);
    find_spenders(mempool_outpoints, result_txids);
}

static std::vector<COutPoint> GetConfirmedOutpoints(const uint256& scripthash)
{
    std::vector<COutPoint> result;
    if (!g_scripthashindex) return result;
    auto utxos = g_scripthashindex->GetUtxos(scripthash);
    result.reserve(utxos.size());
    for (const auto& utxo : utxos) {
        result.push_back(utxo.outpoint);
    }
    return result;
}

std::vector<MempoolScriptHashEntry> MempoolScriptHashIndex::GetEntries(const uint256& scripthash) const
{
    auto confirmed_outpoints = GetConfirmedOutpoints(scripthash);

    LOCK(m_mutex);

    std::set<Txid> result_txids;
    std::vector<std::pair<COutPoint, CAmount>> mempool_outpoints;
    CollectMatchingTxs(scripthash, confirmed_outpoints, result_txids, mempool_outpoints);

    std::vector<MempoolScriptHashEntry> result;
    result.reserve(result_txids.size());
    for (const auto& txid : result_txids) {
        auto entry_it = m_entries.find(txid);
        if (entry_it == m_entries.end()) continue;
        const auto& entry = entry_it->second;
        result.push_back({txid, entry.has_unconfirmed_inputs ? -1 : 0, entry.fee});
    }

    return result;
}

CAmount MempoolScriptHashIndex::GetBalanceDelta(const uint256& scripthash) const
{
    std::vector<COutPoint> confirmed_outpoints;
    std::map<COutPoint, CAmount> confirmed_values;
    if (g_scripthashindex) {
        for (const auto& utxo : g_scripthashindex->GetUtxos(scripthash)) {
            confirmed_outpoints.push_back(utxo.outpoint);
            confirmed_values[utxo.outpoint] = utxo.value;
        }
    }

    LOCK(m_mutex);

    std::set<Txid> result_txids;
    std::vector<std::pair<COutPoint, CAmount>> mempool_outpoints;
    CollectMatchingTxs(scripthash, confirmed_outpoints, result_txids, mempool_outpoints);

    CAmount delta{0};

    for (const auto& [outpoint, value] : mempool_outpoints) {
        delta += value;
    }

    std::map<COutPoint, CAmount> mempool_values;
    for (const auto& [outpoint, value] : mempool_outpoints) {
        mempool_values[outpoint] = value;
    }

    for (const auto& txid : result_txids) {
        auto entry_it = m_entries.find(txid);
        if (entry_it == m_entries.end()) continue;
        const auto& tx = entry_it->second.tx;
        for (const auto& txin : tx->vin) {
            auto cv_it = confirmed_values.find(txin.prevout);
            if (cv_it != confirmed_values.end()) {
                delta -= cv_it->second;
                continue;
            }
            auto mv_it = mempool_values.find(txin.prevout);
            if (mv_it != mempool_values.end()) {
                delta -= mv_it->second;
            }
        }
    }

    return delta;
}

std::vector<std::pair<int64_t, int64_t>> MempoolScriptHashIndex::GetFeeHistogram() const
{
    LOCK(m_mutex);
    std::vector<std::pair<int64_t, int64_t>> result;
    result.reserve(m_fee_histogram.size());
    for (const auto& [label, vsize] : m_fee_histogram) {
        result.emplace_back(label, vsize);
    }
    return result;
}

std::vector<uint256> MempoolScriptHashIndex::TakeDirtyScripthashes(bool& dirty_all)
{
    LOCK(m_mutex);
    dirty_all = m_dirty_all;
    std::vector<uint256> result;
    result.reserve(m_dirty_scripthashes.size());
    for (const auto& scripthash : m_dirty_scripthashes) {
        result.push_back(scripthash);
    }
    m_dirty_scripthashes.clear();
    m_dirty_all = false;
    return result;
}
