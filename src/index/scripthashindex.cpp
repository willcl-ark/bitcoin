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
#include <index/txospenderindex.h>
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
// Three-byte heights cover blocks up to 16,777,215 while saving a byte in every
// marker row.
constexpr uint32_t DB_MAX_HEIGHT{0x00ffffff};

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

template <typename Stream>
void WriteDBHeight(Stream& s, uint32_t height)
{
    Assume(height <= DB_MAX_HEIGHT);
    ser_writedata8(s, height >> 16);
    ser_writedata8(s, height >> 8);
    ser_writedata8(s, height);
}

template <typename Stream>
uint32_t ReadDBHeight(Stream& s)
{
    return (uint32_t{ser_readdata8(s)} << 16) | (uint32_t{ser_readdata8(s)} << 8) | uint32_t{ser_readdata8(s)};
}

struct FundingKey {
    DBPrefix scripthash_prefix;
    uint32_t height;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s.write(std::as_bytes(std::span{scripthash_prefix}));
        WriteDBHeight(s, height);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s.read(std::as_writable_bytes(std::span{scripthash_prefix}));
        if (!HasFamilyTag(scripthash_prefix, DB_FUNDING_TAG)) {
            throw std::ios_base::failure("Invalid format for scripthash index funding key");
        }
        height = ReadDBHeight(s);
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
        WriteDBHeight(s, height);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s.read(std::as_writable_bytes(std::span{outpoint_prefix}));
        if (!HasFamilyTag(outpoint_prefix, DB_SPENDING_TAG)) {
            throw std::ios_base::failure("Invalid format for scripthash index spending key");
        }
        height = ReadDBHeight(s);
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
    uint32_t tx_order;

    friend bool operator<(const TxPosition& a, const TxPosition& b)
    {
        if (a.height != b.height) return a.height < b.height;
        if (a.tx_order != b.tx_order) return a.tx_order < b.tx_order;
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

static bool SpenderInActiveChain(const TxoSpender& spender, int height, const CBlockIndex* scan_tip)
{
    if (!scan_tip || height < 0 || height > scan_tip->nHeight) return false;
    const CBlockIndex* spender_block{scan_tip->GetAncestor(height)};
    return spender_block && spender_block->GetBlockHash() == spender.block_hash;
}

static void ResolveSpendsWithTxoSpender(const TxoSpenderIndex& txospender_index,
                                        const Chainstate& chainstate,
                                        const CBlockIndex* scan_tip,
                                        ScriptHashColdScan& result)
{
    std::vector<COutPoint> outpoints;
    outpoints.reserve(result.funded_utxos.size());
    for (const auto& [outpoint, _] : result.funded_utxos) {
        outpoints.push_back(outpoint);
    }

    const std::span<const COutPoint> outpoint_span{outpoints.data(), outpoints.size()};
    const auto spenders{txospender_index.FindSpenders(outpoint_span)};
    if (!spenders) {
        throw std::runtime_error(spenders.error());
    }
    if (spenders->size() != outpoints.size()) {
        throw std::runtime_error("txospender result size mismatch");
    }

    std::unordered_map<uint256, std::optional<int>, BlockHasher> spender_heights;
    {
        LOCK(::cs_main);
        for (const auto& spender : *spenders) {
            if (!spender) continue;
            const auto [it, inserted]{spender_heights.try_emplace(spender->block_hash)};
            if (!inserted) continue;
            if (const CBlockIndex* block_index{chainstate.m_blockman.LookupBlockIndex(spender->block_hash)}) {
                it->second = block_index->nHeight;
            }
        }
    }

    for (size_t i{0}; i < spenders->size(); ++i) {
        const auto& spender{spenders->at(i)};
        if (!spender) continue;
        const auto height{spender_heights.at(spender->block_hash)};
        if (!height || !SpenderInActiveChain(*spender, *height, scan_tip)) continue;
        result.history.try_emplace(TxPosition{static_cast<uint32_t>(*height), spender->tx_order},
                                   spender->tx->GetHash());
        result.spent_outpoints.insert(outpoints.at(i));
    }
}

static void ResolveSpendsWithLocalMarkers(const Chainstate& chainstate,
                                          CDBWrapper& db,
                                          const CBlockIndex* scan_tip,
                                          ScriptHashColdScan& result,
                                          BlockCache& block_cache)
{
    std::unique_ptr<CDBIterator> spending_it(db.NewIterator());
    SpendingKey spending_key{};
    for (const auto& [outpoint, _] : result.funded_utxos) {
        const DBPrefix outpoint_prefix{OutpointPrefix(outpoint)};
        spending_it->Seek(SpendingKeyPrefix{outpoint_prefix});
        while (spending_it->Valid() && spending_it->GetKey(spending_key) && spending_key.outpoint_prefix == outpoint_prefix) {
            const CBlock* block{GetBlockAtHeight(chainstate, scan_tip, spending_key.height, block_cache)};
            if (block) {
                uint32_t tx_order{static_cast<uint32_t>(GetSizeOfCompactSize(block->vtx.size()))};
                for (size_t tx_pos{0}; tx_pos < block->vtx.size(); ++tx_pos) {
                    const CTransactionRef& tx{block->vtx[tx_pos]};
                    const uint32_t this_tx_order{tx_order};
                    tx_order += static_cast<uint32_t>(::GetSerializeSize(TX_WITH_WITNESS(*tx)));
                    for (const auto& txin : tx->vin) {
                        if (txin.prevout != outpoint) continue;
                        result.history.try_emplace(TxPosition{spending_key.height, this_tx_order}, tx->GetHash());
                        result.spent_outpoints.insert(outpoint);
                        break;
                    }
                }
            }
            spending_it->Next();
        }
    }
}

static ScriptHashColdScan ScanScriptHash(const Chainstate& chainstate,
                                         CDBWrapper& db,
                                         const uint256& scripthash,
                                         const CBlockIndex* scan_tip,
                                         const TxoSpenderIndex* txospender_index)
{
    BlockCache block_cache;
    ScriptHashColdScan result;

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
        uint32_t tx_order{static_cast<uint32_t>(GetSizeOfCompactSize(block->vtx.size()))};
        for (size_t tx_pos{0}; tx_pos < block->vtx.size(); ++tx_pos) {
            const CTransactionRef& tx{block->vtx[tx_pos]};
            const uint32_t this_tx_order{tx_order};
            tx_order += static_cast<uint32_t>(::GetSerializeSize(TX_WITH_WITNESS(*tx)));
            if (bip30_unspendable && tx->IsCoinBase()) continue;
            const Txid txid{tx->GetHash()};
            bool matched{false};
            for (uint32_t i = 0; i < tx->vout.size(); ++i) {
                const CTxOut& tx_out{tx->vout[i]};
                if (tx_out.scriptPubKey.IsUnspendable()) continue;
                if (ComputeScriptHashIndexHash(tx_out.scriptPubKey) != scripthash) continue;
                const COutPoint outpoint{txid, i};
                result.funded_utxos.emplace(outpoint, ScriptHashUtxo{outpoint, static_cast<int>(funding_key.height), this_tx_order, tx_out.nValue});
                matched = true;
            }
            if (matched) result.history.try_emplace(TxPosition{funding_key.height, this_tx_order}, txid);
        }
        funding_it->Next();
    }

    if (txospender_index) {
        ResolveSpendsWithTxoSpender(*txospender_index, chainstate, scan_tip, result);
    } else {
        ResolveSpendsWithLocalMarkers(chainstate, db, scan_tip, result, block_cache);
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
        if (a.tx_order != b.tx_order) return a.tx_order < b.tx_order;
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

ScriptHashIndex::ScriptHashIndex(std::unique_ptr<interfaces::Chain> chain,
                                 size_t n_cache_size,
                                 const TxoSpenderIndex& txospender_index,
                                 bool f_memory,
                                 bool f_wipe)
    : BaseIndex(std::move(chain), "scripthashindex"),
      m_db{std::make_unique<DB>(gArgs.GetDataDirNet() / "indexes" / "scripthashindex" / "db", n_cache_size, f_memory, f_wipe)},
      m_txospender_index{&txospender_index}
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
        if (m_txospender_index) continue;
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
        if (m_txospender_index) continue;
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
    if (m_txospender_index && !m_txospender_index->BlockUntilSyncedToCurrentChain()) return false;

    LOCK(::cs_main);
    const CBlockIndex* tip{m_chainstate->m_chain.Tip()};
    if (GetBestBlockIndex() != tip) return false;
    if (!m_txospender_index) return true;

    const IndexSummary txospender_summary{m_txospender_index->GetSummary()};
    return txospender_summary.best_block_height == tip->nHeight &&
           txospender_summary.best_block_hash == tip->GetBlockHash();
}

std::vector<ScriptHashHistory> ScriptHashIndex::GetHistory(const uint256& scripthash) const
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        {
            LOCK(m_scan_mutex);
            const CBlockIndex* scan_tip{GetBestBlockIndex()};
            const auto scan{ScanScriptHash(*m_chainstate, *m_db, scripthash, scan_tip, m_txospender_index)};
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
            const auto scan{ScanScriptHash(*m_chainstate, *m_db, scripthash, scan_tip, m_txospender_index)};
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
            const auto scan{ScanScriptHash(*m_chainstate, *m_db, scripthash, scan_tip, m_txospender_index)};
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
            const auto scan{ScanScriptHash(*m_chainstate, *m_db, scripthash, scan_tip, m_txospender_index)};
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
