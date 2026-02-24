// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ELECTRUM_MEMPOOLINDEX_H
#define BITCOIN_ELECTRUM_MEMPOOLINDEX_H

#include <primitives/transaction.h>
#include <sync.h>
#include <uint256.h>
#include <util/hasher.h>
#include <validationinterface.h>

#include <functional>
#include <set>
#include <map>
#include <unordered_map>
#include <vector>

class CTxMemPool;

struct MempoolScriptHashEntry {
    Txid txid;
    int height; // 0 = all confirmed inputs, -1 = has unconfirmed input
    CAmount fee;
};

class MempoolScriptHashIndex final : public CValidationInterface
{
public:
    std::vector<MempoolScriptHashEntry> GetEntries(const uint256& scripthash) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    CAmount GetBalanceDelta(const uint256& scripthash) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::vector<std::pair<int64_t, int64_t>> GetFeeHistogram() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::vector<uint256> TakeDirtyScripthashes(bool& dirty_all) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    void LoadFromMempool(const CTxMemPool& mempool) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

protected:
    void TransactionAddedToMempool(const NewMempoolTransactionInfo& tx, uint64_t mempool_sequence) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void TransactionRemovedFromMempool(const CTransactionRef& tx, MemPoolRemovalReason reason, uint64_t mempool_sequence) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void MempoolTransactionsRemovedForBlock(const std::vector<RemovedMempoolTransactionInfo>& txs_removed_for_block, unsigned int nBlockHeight) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    struct Entry {
        CTransactionRef tx;
        CAmount fee;
        int64_t vsize;
        int64_t fee_histogram_label;
        bool has_unconfirmed_inputs;
    };

    void AddTx(const CTransactionRef& tx, CAmount fee, int64_t vsize, bool has_unconfirmed_inputs) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void RemoveTx(const Txid& txid) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void CollectMatchingTxs(const uint256& scripthash,
        const std::vector<COutPoint>& confirmed_outpoints,
        std::set<Txid>& result_txids,
        std::vector<std::pair<COutPoint, CAmount>>& mempool_outpoints) const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    mutable Mutex m_mutex;
    std::unordered_map<Txid, Entry, SaltedTxidHasher> m_entries GUARDED_BY(m_mutex);
    std::set<std::pair<uint256, Txid>> m_by_funding GUARDED_BY(m_mutex);
    std::set<std::pair<COutPoint, Txid>> m_by_spending GUARDED_BY(m_mutex);
    std::map<int64_t, int64_t, std::greater<>> m_fee_histogram GUARDED_BY(m_mutex);
    std::unordered_map<COutPoint, uint256, SaltedOutpointHasher> m_outpoint_to_scripthash GUARDED_BY(m_mutex);
    std::set<uint256> m_dirty_scripthashes GUARDED_BY(m_mutex);
    bool m_dirty_all GUARDED_BY(m_mutex){false};
};

#endif // BITCOIN_ELECTRUM_MEMPOOLINDEX_H
