// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/scripthashindex.h>

#include <common/args.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <dbwrapper.h>
#include <index/base.h>
#include <interfaces/chain.h>
#include <kernel/cs_main.h>
#include <logging.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>
#include <util/check.h>
#include <util/fs.h>
#include <validation.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

constexpr uint8_t DB_FUNDING{'F'};
constexpr uint8_t DB_SPENDING{'S'};
constexpr size_t DB_PREFIX_SIZE{8};

std::unique_ptr<ScriptHashIndex> g_scripthashindex;

uint256 ComputeElectrumScriptHash(const CScript& script)
{
    uint256 hash;
    CSHA256().Write(script.data(), script.size()).Finalize(hash.begin());
    // Electrum convention: reverse byte order
    std::reverse(hash.begin(), hash.end());
    return hash;
}

namespace {

using DBPrefix = std::array<unsigned char, DB_PREFIX_SIZE>;

static DBPrefix ScriptHashPrefix(const uint256& scripthash)
{
    DBPrefix result{};
    std::copy_n(scripthash.begin(), DB_PREFIX_SIZE, result.begin());
    return result;
}

static uint64_t OutpointPrefix(const COutPoint& outpoint)
{
    return ReadBE64(outpoint.hash.begin()) + outpoint.n;
}

struct FundingKey {
    DBPrefix scripthash_prefix;
    uint32_t height;
    uint32_t tx_pos;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_FUNDING);
        s.write(std::as_bytes(std::span{scripthash_prefix}));
        ser_writedata32be(s, height);
        ser_writedata32be(s, tx_pos);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        const uint8_t prefix{ser_readdata8(s)};
        if (prefix != DB_FUNDING) {
            throw std::ios_base::failure("Invalid format for scripthash index funding key");
        }
        s.read(std::as_writable_bytes(std::span{scripthash_prefix}));
        height = ser_readdata32be(s);
        tx_pos = ser_readdata32be(s);
    }
};

struct FundingKeyPrefix {
    DBPrefix scripthash_prefix;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_FUNDING);
        s.write(std::as_bytes(std::span{scripthash_prefix}));
    }
};

struct SpendingKey {
    uint64_t outpoint_prefix;
    uint32_t height;
    uint32_t tx_pos;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_SPENDING);
        ser_writedata64(s, outpoint_prefix);
        ser_writedata32be(s, height);
        ser_writedata32be(s, tx_pos);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        const uint8_t prefix{ser_readdata8(s)};
        if (prefix != DB_SPENDING) {
            throw std::ios_base::failure("Invalid format for scripthash index spending key");
        }
        outpoint_prefix = ser_readdata64(s);
        height = ser_readdata32be(s);
        tx_pos = ser_readdata32be(s);
    }
};

struct SpendingKeyPrefix {
    uint64_t outpoint_prefix;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_SPENDING);
        ser_writedata64(s, outpoint_prefix);
    }
};

struct TxPosition {
    uint32_t height;
    uint32_t tx_pos;

    friend bool operator<(const TxPosition& a, const TxPosition& b)
    {
        return a.height < b.height || (a.height == b.height && a.tx_pos < b.tx_pos);
    }
};

static const CBlock* GetBlockAtHeight(const Chainstate& chainstate, uint32_t height, std::map<uint32_t, CBlock>& block_cache)
{
    const auto found{block_cache.find(height)};
    if (found != block_cache.end()) return &found->second;

    const CBlockIndex* pindex = WITH_LOCK(::cs_main, return (chainstate.m_chain.Height() >= 0 && height <= static_cast<uint32_t>(chainstate.m_chain.Height())) ? chainstate.m_chain[height] : nullptr);
    if (!pindex) return nullptr;

    CBlock block;
    if (!chainstate.m_blockman.ReadBlock(block, *pindex)) return nullptr;
    return &block_cache.emplace(height, std::move(block)).first->second;
}

static bool ReadTxByPosition(const Chainstate& chainstate, const TxPosition& position, std::map<uint32_t, CBlock>& block_cache, CTransactionRef& tx)
{
    const CBlock* block{GetBlockAtHeight(chainstate, position.height, block_cache)};
    if (!block) return false;
    if (position.tx_pos >= block->vtx.size()) return false;
    tx = block->vtx[position.tx_pos];
    return true;
}

struct ScriptHashScanResult {
    std::map<TxPosition, Txid> history_entries;
    std::vector<ScriptHashUtxo> utxos;
    CAmount balance{0};
};

static ScriptHashScanResult ScanScriptHash(const Chainstate& chainstate, CDBWrapper& db, const uint256& scripthash)
{
    std::map<uint32_t, CBlock> block_cache;
    std::map<COutPoint, ScriptHashUtxo> funded_outpoints;
    std::set<COutPoint> spent_outpoints;
    std::map<TxPosition, Txid> history_entries;

    std::unique_ptr<CDBIterator> funding_it(db.NewIterator());
    FundingKey funding_key{};
    const DBPrefix scripthash_prefix{ScriptHashPrefix(scripthash)};
    funding_it->Seek(FundingKeyPrefix{scripthash_prefix});

    while (funding_it->Valid() && funding_it->GetKey(funding_key) && funding_key.scripthash_prefix == scripthash_prefix) {
        const TxPosition tx_pos{funding_key.height, funding_key.tx_pos};
        CTransactionRef tx;
        if (ReadTxByPosition(chainstate, tx_pos, block_cache, tx)) {
            const Txid txid{tx->GetHash()};
            bool matched{false};
            for (uint32_t i = 0; i < tx->vout.size(); ++i) {
                const CTxOut& tx_out{tx->vout[i]};
                if (tx_out.scriptPubKey.IsUnspendable()) continue;
                if (ComputeElectrumScriptHash(tx_out.scriptPubKey) != scripthash) continue;
                funded_outpoints[COutPoint{txid, i}] = {COutPoint{txid, i}, static_cast<int>(funding_key.height), tx_out.nValue};
                matched = true;
            }
            if (matched) history_entries.try_emplace(tx_pos, txid);
        }
        funding_it->Next();
    }

    std::unique_ptr<CDBIterator> spending_it(db.NewIterator());
    for (const auto& [outpoint, _] : funded_outpoints) {
        const uint64_t outpoint_prefix{OutpointPrefix(outpoint)};
        SpendingKey spending_key{};
        spending_it->Seek(SpendingKeyPrefix{outpoint_prefix});
        while (spending_it->Valid() && spending_it->GetKey(spending_key) && spending_key.outpoint_prefix == outpoint_prefix) {
            const TxPosition tx_pos{spending_key.height, spending_key.tx_pos};
            CTransactionRef tx;
            if (ReadTxByPosition(chainstate, tx_pos, block_cache, tx)) {
                for (const auto& txin : tx->vin) {
                    if (txin.prevout != outpoint) continue;
                    spent_outpoints.insert(outpoint);
                    history_entries.try_emplace(tx_pos, tx->GetHash());
                    break;
                }
            }
            spending_it->Next();
        }
    }

    ScriptHashScanResult result;
    result.history_entries = std::move(history_entries);
    for (const auto& [outpoint, utxo] : funded_outpoints) {
        if (spent_outpoints.contains(outpoint)) continue;
        result.balance += utxo.value;
        result.utxos.push_back(utxo);
    }
    return result;
}

} // namespace

ScriptHashIndex::CacheEntry ScriptHashIndex::GetOrCreateCacheEntry(const uint256& scripthash) const
{
    {
        LOCK(m_cache_mutex);
        const auto it{m_cache.find(scripthash)};
        if (it != m_cache.end()) return it->second;
    }

    const auto scan_result{ScanScriptHash(*m_chainstate, *m_db, scripthash)};
    CacheEntry entry;
    entry.history.reserve(scan_result.history_entries.size());
    for (const auto& [pos, txid] : scan_result.history_entries) {
        entry.history.push_back({txid, static_cast<int>(pos.height)});
    }
    entry.utxos = scan_result.utxos;
    entry.balance = scan_result.balance;

    LOCK(m_cache_mutex);
    return m_cache.try_emplace(scripthash, entry).first->second;
}

ScriptHashIndex::ScriptHashIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory, bool f_wipe)
    : BaseIndex(std::move(chain), "scripthashindex"),
      m_db{std::make_unique<DB>(gArgs.GetDataDirNet() / "indexes" / "scripthashindex" / "db", n_cache_size, f_memory, f_wipe)}
{
}

interfaces::Chain::NotifyOptions ScriptHashIndex::CustomOptions()
{
    interfaces::Chain::NotifyOptions options;
    options.disconnect_data = true;
    return options;
}

bool ScriptHashIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    {
        LOCK(m_cache_mutex);
        m_cache.clear();
    }

    CDBBatch batch(*m_db);
    const auto& txs = block.data->vtx;

    for (size_t i = 0; i < txs.size(); ++i) {
        const auto& tx = txs[i];
        const uint32_t height{static_cast<uint32_t>(block.height)};
        const uint32_t tx_pos{static_cast<uint32_t>(i)};

        for (const CTxOut& tx_out : tx->vout) {
            if (tx_out.scriptPubKey.IsUnspendable()) continue;
            const auto sh_prefix{ScriptHashPrefix(ComputeElectrumScriptHash(tx_out.scriptPubKey))};
            batch.Write(FundingKey{sh_prefix, height, tx_pos}, "");
        }

        if (tx->IsCoinBase()) continue;
        for (const CTxIn& txin : tx->vin) {
            batch.Write(SpendingKey{OutpointPrefix(txin.prevout), height, tx_pos}, "");
        }
    }

    m_db->WriteBatch(batch);
    return true;
}

bool ScriptHashIndex::CustomRemove(const interfaces::BlockInfo& block)
{
    {
        LOCK(m_cache_mutex);
        m_cache.clear();
    }

    CDBBatch batch(*m_db);
    assert(block.data);
    const auto& txs = block.data->vtx;

    for (size_t i = 0; i < txs.size(); ++i) {
        const auto& tx = txs[i];
        const uint32_t height{static_cast<uint32_t>(block.height)};
        const uint32_t tx_pos{static_cast<uint32_t>(i)};

        for (const CTxOut& tx_out : tx->vout) {
            if (tx_out.scriptPubKey.IsUnspendable()) continue;
            const auto sh_prefix{ScriptHashPrefix(ComputeElectrumScriptHash(tx_out.scriptPubKey))};
            batch.Erase(FundingKey{sh_prefix, height, tx_pos});
        }

        if (tx->IsCoinBase()) continue;
        for (const CTxIn& txin : tx->vin) {
            batch.Erase(SpendingKey{OutpointPrefix(txin.prevout), height, tx_pos});
        }
    }

    m_db->WriteBatch(batch);
    return true;
}

BaseIndex::DB& ScriptHashIndex::GetDB() const { return *m_db; }

std::vector<ScriptHashHistory> ScriptHashIndex::GetHistory(const uint256& scripthash) const
{
    return GetOrCreateCacheEntry(scripthash).history;
}

std::vector<ScriptHashUtxo> ScriptHashIndex::GetUtxos(const uint256& scripthash) const
{
    return GetOrCreateCacheEntry(scripthash).utxos;
}

CAmount ScriptHashIndex::GetBalance(const uint256& scripthash) const
{
    return GetOrCreateCacheEntry(scripthash).balance;
}

void ScriptHashIndex::CacheScriptHash(const uint256& scripthash)
{
    {
        LOCK(m_cache_mutex);
        ++m_pinned_refs[scripthash];
    }
    (void)GetOrCreateCacheEntry(scripthash);
}

void ScriptHashIndex::UncacheScriptHash(const uint256& scripthash)
{
    LOCK(m_cache_mutex);
    const auto ref_it{m_pinned_refs.find(scripthash)};
    if (ref_it == m_pinned_refs.end()) return;
    if (ref_it->second > 1) {
        --ref_it->second;
        return;
    }
    m_pinned_refs.erase(ref_it);
    m_cache.erase(scripthash);
}
