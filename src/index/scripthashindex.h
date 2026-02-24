// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_SCRIPTHASHINDEX_H
#define BITCOIN_INDEX_SCRIPTHASHINDEX_H

#include <consensus/amount.h>
#include <index/base.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstddef>
#include <memory>
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
    std::unique_ptr<BaseIndex::DB> m_db;

    bool AllowPrune() const override { return false; }

protected:
    interfaces::Chain::NotifyOptions CustomOptions() override;
    bool CustomAppend(const interfaces::BlockInfo& block) override;
    bool CustomRemove(const interfaces::BlockInfo& block) override;
    BaseIndex::DB& GetDB() const override;

public:
    explicit ScriptHashIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

    std::vector<ScriptHashHistory> GetHistory(const uint256& scripthash) const;
    std::vector<ScriptHashUtxo> GetUtxos(const uint256& scripthash) const;
    CAmount GetBalance(const uint256& scripthash) const;
};

extern std::unique_ptr<ScriptHashIndex> g_scripthashindex;

#endif // BITCOIN_INDEX_SCRIPTHASHINDEX_H
