// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_SCRIPTHASHINDEX_H
#define BITCOIN_INDEX_SCRIPTHASHINDEX_H

#include <consensus/amount.h>
#include <index/base.h>
#include <interfaces/chain.h>
#include <kernel/cs_main.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class CScript;
class TxoSpenderIndex;

static constexpr bool DEFAULT_SCRIPTHASHINDEX{false};

struct ScriptHashHistory {
    Txid txid;
    int height;

    friend bool operator==(const ScriptHashHistory& a, const ScriptHashHistory& b)
    {
        return a.txid == b.txid && a.height == b.height;
    }
};

struct ScriptHashUtxo {
    COutPoint outpoint;
    int height;
    uint32_t tx_order;
    CAmount value;
};

struct ScriptHashActivity {
    std::vector<ScriptHashHistory> history;
    std::vector<ScriptHashUtxo> utxos;
    CAmount balance{0};
    uint256 bestblock;
    int height{-1};
};

uint256 ComputeScriptHashIndexHash(const CScript& script);

class ScriptHashIndex final : public BaseIndex
{
private:
    std::unique_ptr<BaseIndex::DB> m_db;
    const TxoSpenderIndex* const m_txospender_index{nullptr};
    mutable Mutex m_scan_mutex;

    bool AllowPrune() const override { return false; }

protected:
    interfaces::Chain::NotifyOptions CustomOptions() override;
    bool CustomAppend(const interfaces::BlockInfo& block) override EXCLUSIVE_LOCKS_REQUIRED(!m_scan_mutex);
    bool CustomRemove(const interfaces::BlockInfo& block) override EXCLUSIVE_LOCKS_REQUIRED(!m_scan_mutex);
    BaseIndex::DB& GetDB() const override;

public:
    explicit ScriptHashIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory = false, bool f_wipe = false);
    explicit ScriptHashIndex(std::unique_ptr<interfaces::Chain> chain,
                             size_t n_cache_size,
                             const TxoSpenderIndex& txospender_index,
                             bool f_memory = false,
                             bool f_wipe = false);

    bool BlockUntilSyncedToActiveChain() const LOCKS_EXCLUDED(::cs_main);
    std::vector<ScriptHashHistory> GetHistory(const uint256& scripthash) const EXCLUSIVE_LOCKS_REQUIRED(!m_scan_mutex);
    std::vector<ScriptHashUtxo> GetUtxos(const uint256& scripthash) const EXCLUSIVE_LOCKS_REQUIRED(!m_scan_mutex);
    CAmount GetBalance(const uint256& scripthash) const EXCLUSIVE_LOCKS_REQUIRED(!m_scan_mutex);
    ScriptHashActivity GetActivity(const uint256& scripthash) const EXCLUSIVE_LOCKS_REQUIRED(!m_scan_mutex);
};

extern std::unique_ptr<ScriptHashIndex> g_scripthashindex;

#endif // BITCOIN_INDEX_SCRIPTHASHINDEX_H
