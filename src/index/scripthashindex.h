// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_SCRIPTHASHINDEX_H
#define BITCOIN_INDEX_SCRIPTHASHINDEX_H

#include <consensus/amount.h>
#include <index/base.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <uint256.h>
#include <util/hasher.h>

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

static constexpr bool DEFAULT_SCRIPTHASHINDEX{false};

struct ScriptHashHistory {
    Txid txid;
    int height;
};

struct ScriptHashUtxo {
    COutPoint outpoint;
    int height;
    CAmount value;
};

uint256 ComputeElectrumScriptHash(const CScript& script);

class ScriptHashIndex final : public BaseIndex
{
private:
    struct CacheEntry {
        std::vector<ScriptHashHistory> history;
        std::vector<ScriptHashUtxo> utxos;
        CAmount balance{0};
    };

    std::unique_ptr<BaseIndex::DB> m_db;
    mutable Mutex m_cache_mutex;
    mutable std::unordered_map<uint256, CacheEntry, SaltedUint256Hasher> m_cache GUARDED_BY(m_cache_mutex);
    mutable std::unordered_map<uint256, size_t, SaltedUint256Hasher> m_pinned_refs GUARDED_BY(m_cache_mutex);

    CacheEntry GetOrCreateCacheEntry(const uint256& scripthash) const LOCKS_EXCLUDED(m_cache_mutex);

    bool AllowPrune() const override { return false; }

protected:
    interfaces::Chain::NotifyOptions CustomOptions() override;
    bool CustomAppend(const interfaces::BlockInfo& block) override LOCKS_EXCLUDED(m_cache_mutex);
    bool CustomRemove(const interfaces::BlockInfo& block) override LOCKS_EXCLUDED(m_cache_mutex);
    BaseIndex::DB& GetDB() const override;

public:
    explicit ScriptHashIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

    std::vector<ScriptHashHistory> GetHistory(const uint256& scripthash) const LOCKS_EXCLUDED(m_cache_mutex);
    std::vector<ScriptHashUtxo> GetUtxos(const uint256& scripthash) const LOCKS_EXCLUDED(m_cache_mutex);
    CAmount GetBalance(const uint256& scripthash) const LOCKS_EXCLUDED(m_cache_mutex);
    void CacheScriptHash(const uint256& scripthash) LOCKS_EXCLUDED(m_cache_mutex);
    void UncacheScriptHash(const uint256& scripthash) LOCKS_EXCLUDED(m_cache_mutex);
    void CompactOnceAfterSync();
};

extern std::unique_ptr<ScriptHashIndex> g_scripthashindex;

#endif // BITCOIN_INDEX_SCRIPTHASHINDEX_H
