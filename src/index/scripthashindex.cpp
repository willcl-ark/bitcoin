// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/scripthashindex.h>

#include <chain.h>
#include <common/args.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <dbwrapper.h>
#include <index/base.h>
#include <interfaces/chain.h>
#include <kernel/cs_main.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/hasher.h>
#include <validation.h>

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <ios>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Scripthash index marker keys are:
//
//     6-byte prefix || 3-byte big-endian block height
//
// The first prefix byte carries the row family in bit 6. Bit 7 is always
// cleared so LevelDB's signed-char std::string key handling does not trip the
// integer sanitizer while finding short successor keys. The remaining prefix
// bits are copied from the scripthash or outpoint-derived prefix.
constexpr unsigned char DB_FUNDING_TAG{0x00};
constexpr unsigned char DB_SPENDING_TAG{0x40};
constexpr unsigned char DB_TAG_MASK{0x40};
constexpr unsigned char DB_SIGN_BIT{0x80};
// Six-byte tagged prefixes keep expected false-positive scans negligible for
// mainnet: roughly 6.3B indexed rows / 2^46 ~= 0.00009 extra candidates.
constexpr size_t DB_PREFIX_SIZE{6};

std::unique_ptr<ScriptHashIndex> g_scripthashindex;

uint256 ComputeScriptHashIndexHash(const CScript& script)
{
    uint256 hash;
    CSHA256().Write(script.data(), script.size()).Finalize(hash.begin());
    // Keep the digest in Bitcoin Core's uint256/RPC hex convention. Electrum
    // protocol scripthashes are the byte-reversed hex form of this value.
    std::reverse(hash.begin(), hash.end());
    return hash;
}

namespace {

using DBPrefix = std::array<unsigned char, DB_PREFIX_SIZE>;

struct EmptyValue {
    template <typename Stream>
    void Serialize(Stream&) const
    {
    }
};

constexpr EmptyValue EMPTY_VALUE{};

static DBPrefix ScriptHashPrefix(const uint256& scripthash)
{
    DBPrefix result{};
    std::copy_n(scripthash.begin(), DB_PREFIX_SIZE, result.begin());
    result.front() = (result.front() & ~(DB_TAG_MASK | DB_SIGN_BIT)) | DB_FUNDING_TAG;
    return result;
}

static DBPrefix OutpointPrefix(const COutPoint& outpoint)
{
    DBPrefix result{};
    std::array<unsigned char, 8> full_prefix{};
    std::copy_n(outpoint.hash.ToUint256().begin(), full_prefix.size(), full_prefix.begin());
    WriteBE64(full_prefix.data(), ReadBE64(full_prefix.data()) + outpoint.n);
    std::copy_n(full_prefix.begin(), DB_PREFIX_SIZE, result.begin());
    result.front() = (result.front() & ~(DB_TAG_MASK | DB_SIGN_BIT)) | DB_SPENDING_TAG;
    return result;
}

static bool HasFamilyTag(const DBPrefix& prefix, unsigned char tag)
{
    return (prefix.front() & DB_TAG_MASK) == tag;
}

struct FundingKey {
    DBPrefix scripthash_prefix;
    uint32_t height;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s.write(std::as_bytes(std::span{scripthash_prefix}));
        ser_writedata32be(s, height);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s.read(std::as_writable_bytes(std::span{scripthash_prefix}));
        if (!HasFamilyTag(scripthash_prefix, DB_FUNDING_TAG)) {
            throw std::ios_base::failure("Invalid format for scripthash index funding key");
        }
        height = ser_readdata32be(s);
    }
};

struct FundingKeyPrefix {
    DBPrefix scripthash_prefix;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s.write(std::as_bytes(std::span{scripthash_prefix}));
    }
};

struct SpendingKey {
    DBPrefix outpoint_prefix;
    uint32_t height;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s.write(std::as_bytes(std::span{outpoint_prefix}));
        ser_writedata32be(s, height);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s.read(std::as_writable_bytes(std::span{outpoint_prefix}));
        if (!HasFamilyTag(outpoint_prefix, DB_SPENDING_TAG)) {
            throw std::ios_base::failure("Invalid format for scripthash index spending key");
        }
        height = ser_readdata32be(s);
    }
};

struct SpendingKeyPrefix {
    DBPrefix outpoint_prefix;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s.write(std::as_bytes(std::span{outpoint_prefix}));
    }
};

struct TxPosition {
    uint32_t height;
    uint32_t tx_pos;

    friend bool operator<(const TxPosition& a, const TxPosition& b)
    {
        if (a.height != b.height) return a.height < b.height;
        if (a.tx_pos != b.tx_pos) return a.tx_pos < b.tx_pos;
        return false;
    }
};

using BlockCache = std::map<uint32_t, CBlock>;

struct ScriptHashColdScan {
    std::map<TxPosition, Txid> history;
    std::unordered_map<COutPoint, ScriptHashUtxo, SaltedOutpointHasher> funded_utxos;
    std::unordered_set<COutPoint, SaltedOutpointHasher> spent_outpoints;
};

static const CBlock* GetBlockAtHeight(const Chainstate& chainstate, const CBlockIndex* scan_tip, uint32_t height, BlockCache& block_cache)
{
    if (!scan_tip || height > static_cast<uint32_t>(scan_tip->nHeight)) return nullptr;

    const auto found{block_cache.find(height)};
    if (found != block_cache.end()) return &found->second;

    const CBlockIndex* block_index{scan_tip->GetAncestor(height)};
    if (!block_index) return nullptr;

    CBlock block;
    if (!chainstate.m_blockman.ReadBlock(block, *block_index)) {
        throw std::runtime_error("Failed to read scripthash index block from disk");
    }
    return &block_cache.emplace(height, std::move(block)).first->second;
}

struct ScriptHashUtxoScanResult {
    std::vector<ScriptHashUtxo> utxos;
    CAmount balance{0};
};

static ScriptHashColdScan ScanScriptHash(const Chainstate& chainstate, CDBWrapper& db, const uint256& scripthash, const CBlockIndex* scan_tip)
{
    BlockCache block_cache;
    ScriptHashColdScan result;

    // One LevelDB iterator gives the whole cold query a stable DB snapshot.
    // Candidate rows are verified against active-chain block data, so stale rows
    // left behind after an unclean restart cannot create false positives.
    std::unique_ptr<CDBIterator> funding_it(db.NewIterator());
    FundingKey funding_key{};
    const DBPrefix scripthash_prefix{ScriptHashPrefix(scripthash)};
    funding_it->Seek(FundingKeyPrefix{scripthash_prefix});

    while (funding_it->Valid() && funding_it->GetKey(funding_key) && funding_key.scripthash_prefix == scripthash_prefix) {
        const CBlock* block{GetBlockAtHeight(chainstate, scan_tip, funding_key.height, block_cache)};
        if (!block) {
            funding_it->Next();
            continue;
        }
        const bool bip30_unspendable{IsBIP30Unspendable(block->GetHash(), funding_key.height)};
        for (size_t tx_pos{0}; tx_pos < block->vtx.size(); ++tx_pos) {
            const CTransactionRef& tx{block->vtx[tx_pos]};
            if (bip30_unspendable && tx->IsCoinBase()) continue;
            const Txid txid{tx->GetHash()};
            bool matched{false};
            for (uint32_t i = 0; i < tx->vout.size(); ++i) {
                const CTxOut& tx_out{tx->vout[i]};
                if (tx_out.scriptPubKey.IsUnspendable()) continue;
                if (ComputeScriptHashIndexHash(tx_out.scriptPubKey) != scripthash) continue;
                const COutPoint outpoint{txid, i};
                result.funded_utxos.emplace(outpoint, ScriptHashUtxo{outpoint, static_cast<int>(funding_key.height), static_cast<uint32_t>(tx_pos), tx_out.nValue});
                matched = true;
            }
            if (matched) result.history.try_emplace(TxPosition{funding_key.height, static_cast<uint32_t>(tx_pos)}, txid);
        }
        funding_it->Next();
    }

    SpendingKey spending_key{};
    for (const auto& [outpoint, _] : result.funded_utxos) {
        const DBPrefix outpoint_prefix{OutpointPrefix(outpoint)};
        funding_it->Seek(SpendingKeyPrefix{outpoint_prefix});
        while (funding_it->Valid() && funding_it->GetKey(spending_key) && spending_key.outpoint_prefix == outpoint_prefix) {
            const CBlock* block{GetBlockAtHeight(chainstate, scan_tip, spending_key.height, block_cache)};
            if (block) {
                for (size_t tx_pos{0}; tx_pos < block->vtx.size(); ++tx_pos) {
                    const CTransactionRef& tx{block->vtx[tx_pos]};
                    for (const auto& txin : tx->vin) {
                        if (txin.prevout != outpoint) continue;
                        result.history.try_emplace(TxPosition{spending_key.height, static_cast<uint32_t>(tx_pos)}, tx->GetHash());
                        result.spent_outpoints.insert(outpoint);
                        break;
                    }
                }
            }
            funding_it->Next();
        }
    }

    return result;
}

static std::vector<ScriptHashHistory> BuildScriptHashHistory(const ScriptHashColdScan& scan)
{
    std::vector<ScriptHashHistory> result;
    result.reserve(scan.history.size());
    for (const auto& [pos, txid] : scan.history) {
        result.push_back({txid, static_cast<int>(pos.height)});
    }
    return result;
}

static ScriptHashUtxoScanResult BuildScriptHashUtxos(const ScriptHashColdScan& scan)
{
    ScriptHashUtxoScanResult result;
    result.utxos.reserve(scan.funded_utxos.size());
    for (const auto& [outpoint, utxo] : scan.funded_utxos) {
        if (scan.spent_outpoints.contains(outpoint)) continue;
        result.balance += utxo.value;
        result.utxos.push_back(utxo);
    }
    std::sort(result.utxos.begin(), result.utxos.end(), [](const ScriptHashUtxo& a, const ScriptHashUtxo& b) {
        if (a.height != b.height) return a.height < b.height;
        if (a.tx_pos != b.tx_pos) return a.tx_pos < b.tx_pos;
        if (a.outpoint.n != b.outpoint.n) return a.outpoint.n < b.outpoint.n;
        return a.outpoint.hash < b.outpoint.hash;
    });
    return result;
}

} // namespace

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
    if (block.height == 0) return true;

    CDBBatch batch(*m_db);
    const auto& txs = block.data->vtx;

    for (const auto& tx : txs) {
        const uint32_t height{static_cast<uint32_t>(block.height)};
        if (tx->IsCoinBase() && IsBIP30Unspendable(block.hash, block.height)) continue;

        for (const CTxOut& tx_out : tx->vout) {
            if (tx_out.scriptPubKey.IsUnspendable()) continue;
            const auto sh_prefix{ScriptHashPrefix(ComputeScriptHashIndexHash(tx_out.scriptPubKey))};
            batch.Write(FundingKey{sh_prefix, height}, EMPTY_VALUE);
        }

        if (tx->IsCoinBase()) continue;
        for (const CTxIn& txin : tx->vin) {
            batch.Write(SpendingKey{OutpointPrefix(txin.prevout), height}, EMPTY_VALUE);
        }
    }

    {
        LOCK(m_scan_mutex);
        m_db->WriteBatch(batch);
    }
    return true;
}

bool ScriptHashIndex::CustomRemove(const interfaces::BlockInfo& block)
{
    if (block.height == 0) return true;

    CDBBatch batch(*m_db);
    assert(block.data);
    const auto& txs = block.data->vtx;

    for (const auto& tx : txs) {
        const uint32_t height{static_cast<uint32_t>(block.height)};
        if (tx->IsCoinBase() && IsBIP30Unspendable(block.hash, block.height)) continue;

        for (const CTxOut& tx_out : tx->vout) {
            if (tx_out.scriptPubKey.IsUnspendable()) continue;
            const auto sh_prefix{ScriptHashPrefix(ComputeScriptHashIndexHash(tx_out.scriptPubKey))};
            batch.Erase(FundingKey{sh_prefix, height});
        }

        if (tx->IsCoinBase()) continue;
        for (const CTxIn& txin : tx->vin) {
            batch.Erase(SpendingKey{OutpointPrefix(txin.prevout), height});
        }
    }

    {
        LOCK(m_scan_mutex);
        m_db->WriteBatch(batch);
    }
    return true;
}

BaseIndex::DB& ScriptHashIndex::GetDB() const { return *m_db; }

bool ScriptHashIndex::BlockUntilSyncedToActiveChain() const
{
    if (!BaseIndex::BlockUntilSyncedToCurrentChain()) return false;

    LOCK(::cs_main);
    return GetBestBlockIndex() == m_chainstate->m_chain.Tip();
}

std::vector<ScriptHashHistory> ScriptHashIndex::GetHistory(const uint256& scripthash) const
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        {
            LOCK(m_scan_mutex);
            const CBlockIndex* scan_tip{GetBestBlockIndex()};
            const auto scan{ScanScriptHash(*m_chainstate, *m_db, scripthash, scan_tip)};
            auto history{BuildScriptHashHistory(scan)};

            if (GetBestBlockIndex() != scan_tip) {
                if (attempt == 0) continue;
            }
            return history;
        }
    }

    Assume(false);
    return {};
}

std::vector<ScriptHashUtxo> ScriptHashIndex::GetUtxos(const uint256& scripthash) const
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        {
            LOCK(m_scan_mutex);
            const CBlockIndex* scan_tip{GetBestBlockIndex()};
            const auto scan{ScanScriptHash(*m_chainstate, *m_db, scripthash, scan_tip)};
            auto scan_result{BuildScriptHashUtxos(scan)};

            if (GetBestBlockIndex() != scan_tip) {
                if (attempt == 0) continue;
            }
            return scan_result.utxos;
        }
    }

    Assume(false);
    return {};
}

CAmount ScriptHashIndex::GetBalance(const uint256& scripthash) const
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        {
            LOCK(m_scan_mutex);
            const CBlockIndex* scan_tip{GetBestBlockIndex()};
            const auto scan{ScanScriptHash(*m_chainstate, *m_db, scripthash, scan_tip)};
            auto scan_result{BuildScriptHashUtxos(scan)};

            if (GetBestBlockIndex() != scan_tip) {
                if (attempt == 0) continue;
            }
            return scan_result.balance;
        }
    }

    Assume(false);
    return 0;
}

ScriptHashActivity ScriptHashIndex::GetActivity(const uint256& scripthash) const
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        {
            LOCK(m_scan_mutex);
            const CBlockIndex* scan_tip{GetBestBlockIndex()};
            const auto scan{ScanScriptHash(*m_chainstate, *m_db, scripthash, scan_tip)};
            auto history{BuildScriptHashHistory(scan)};
            auto scan_result{BuildScriptHashUtxos(scan)};

            if (GetBestBlockIndex() != scan_tip) {
                if (attempt == 0) continue;
            }
            return {std::move(history), std::move(scan_result.utxos), scan_result.balance, scan_tip ? scan_tip->GetBlockHash() : uint256{}, scan_tip ? scan_tip->nHeight : -1};
        }
    }

    Assume(false);
    return {};
}
