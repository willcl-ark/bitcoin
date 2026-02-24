// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/scripthashindex.h>

#include <common/args.h>
#include <crypto/sha256.h>
#include <dbwrapper.h>
#include <index/base.h>
#include <interfaces/chain.h>
#include <logging.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>
#include <undo.h>
#include <util/check.h>
#include <util/fs.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

constexpr uint8_t DB_HISTORY{'H'};
constexpr uint8_t DB_UTXO{'U'};

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

struct HistoryKey {
    uint256 scripthash;
    uint32_t height;
    uint32_t tx_pos;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_HISTORY);
        s.write(std::as_bytes(std::span{scripthash.begin(), 32}));
        ser_writedata32be(s, height);
        ser_writedata32be(s, tx_pos);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        const uint8_t prefix{ser_readdata8(s)};
        if (prefix != DB_HISTORY) {
            throw std::ios_base::failure("Invalid format for scripthash index history key");
        }
        s.read(std::as_writable_bytes(std::span{scripthash.begin(), 32}));
        height = ser_readdata32be(s);
        tx_pos = ser_readdata32be(s);
    }
};

struct HistoryKeyPrefix {
    uint256 scripthash;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_HISTORY);
        s.write(std::as_bytes(std::span{scripthash.begin(), 32}));
    }
};

struct UtxoKey {
    uint256 scripthash;
    Txid txid;
    uint32_t vout;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_UTXO);
        s.write(std::as_bytes(std::span{scripthash.begin(), 32}));
        s << txid;
        ser_writedata32be(s, vout);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        const uint8_t prefix{ser_readdata8(s)};
        if (prefix != DB_UTXO) {
            throw std::ios_base::failure("Invalid format for scripthash index UTXO key");
        }
        s.read(std::as_writable_bytes(std::span{scripthash.begin(), 32}));
        s >> txid;
        vout = ser_readdata32be(s);
    }
};

struct UtxoKeyPrefix {
    uint256 scripthash;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_UTXO);
        s.write(std::as_bytes(std::span{scripthash.begin(), 32}));
    }
};

struct UtxoValue {
    uint32_t height;
    CAmount value;

    SERIALIZE_METHODS(UtxoValue, obj)
    {
        READWRITE(obj.height, obj.value);
    }
};

} // namespace

ScriptHashIndex::ScriptHashIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory, bool f_wipe)
    : BaseIndex(std::move(chain), "scripthashindex"),
      m_db{std::make_unique<DB>(gArgs.GetDataDirNet() / "indexes" / "scripthashindex" / "db", n_cache_size, f_memory, f_wipe)}
{
}

interfaces::Chain::NotifyOptions ScriptHashIndex::CustomOptions()
{
    interfaces::Chain::NotifyOptions options;
    options.connect_undo_data = true;
    options.disconnect_data = true;
    options.disconnect_undo_data = true;
    return options;
}

bool ScriptHashIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    CDBBatch batch(*m_db);
    const auto& txs = block.data->vtx;

    for (size_t i = 0; i < txs.size(); ++i) {
        const auto& tx = txs[i];
        const bool is_coinbase = tx->IsCoinBase();
        const Txid txid = tx->GetHash();

        for (uint32_t j = 0; j < tx->vout.size(); ++j) {
            const CTxOut& tx_out = tx->vout[j];
            if (tx_out.scriptPubKey.IsUnspendable()) continue;
            const uint256 sh = ComputeElectrumScriptHash(tx_out.scriptPubKey);

            HistoryKey hkey{sh, static_cast<uint32_t>(block.height), static_cast<uint32_t>(i)};
            batch.Write(hkey, txid);

            UtxoKey ukey{sh, txid, j};
            UtxoValue uval{static_cast<uint32_t>(block.height), tx_out.nValue};
            batch.Write(ukey, uval);
        }

        if (!is_coinbase) {
            const auto& tx_undo = Assert(block.undo_data)->vtxundo.at(i - 1);

            for (size_t j = 0; j < tx_undo.vprevout.size(); ++j) {
                const Coin& coin = tx_undo.vprevout[j];
                const COutPoint& prevout = tx->vin[j].prevout;
                const uint256 sh = ComputeElectrumScriptHash(coin.out.scriptPubKey);

                HistoryKey hkey{sh, static_cast<uint32_t>(block.height), static_cast<uint32_t>(i)};
                batch.Write(hkey, txid);

                UtxoKey ukey{sh, prevout.hash, prevout.n};
                batch.Erase(ukey);
            }
        }
    }

    m_db->WriteBatch(batch);
    return true;
}

bool ScriptHashIndex::CustomRemove(const interfaces::BlockInfo& block)
{
    CDBBatch batch(*m_db);
    assert(block.data);
    assert(block.undo_data);
    const auto& txs = block.data->vtx;

    for (size_t i = 0; i < txs.size(); ++i) {
        const auto& tx = txs[i];
        const bool is_coinbase = tx->IsCoinBase();
        const Txid txid = tx->GetHash();

        for (uint32_t j = 0; j < tx->vout.size(); ++j) {
            const CTxOut& tx_out = tx->vout[j];
            if (tx_out.scriptPubKey.IsUnspendable()) continue;
            const uint256 sh = ComputeElectrumScriptHash(tx_out.scriptPubKey);

            HistoryKey hkey{sh, static_cast<uint32_t>(block.height), static_cast<uint32_t>(i)};
            batch.Erase(hkey);

            UtxoKey ukey{sh, txid, j};
            batch.Erase(ukey);
        }

        if (!is_coinbase) {
            const auto& tx_undo = block.undo_data->vtxundo.at(i - 1);

            for (size_t j = 0; j < tx_undo.vprevout.size(); ++j) {
                const Coin& coin = tx_undo.vprevout[j];
                const COutPoint& prevout = tx->vin[j].prevout;
                const uint256 sh = ComputeElectrumScriptHash(coin.out.scriptPubKey);

                HistoryKey hkey{sh, static_cast<uint32_t>(block.height), static_cast<uint32_t>(i)};
                batch.Erase(hkey);

                UtxoKey ukey{sh, prevout.hash, prevout.n};
                UtxoValue uval{static_cast<uint32_t>(coin.nHeight), coin.out.nValue};
                batch.Write(ukey, uval);
            }
        }
    }

    m_db->WriteBatch(batch);
    return true;
}

BaseIndex::DB& ScriptHashIndex::GetDB() const { return *m_db; }

std::vector<ScriptHashHistory> ScriptHashIndex::GetHistory(const uint256& scripthash) const
{
    std::vector<ScriptHashHistory> result;
    std::unique_ptr<CDBIterator> it(m_db->NewIterator());

    HistoryKey key{};
    it->Seek(HistoryKeyPrefix{scripthash});

    while (it->Valid() && it->GetKey(key) && key.scripthash == scripthash) {
        Txid txid;
        if (it->GetValue(txid)) {
            result.push_back({txid, static_cast<int>(key.height)});
        }
        it->Next();
    }

    return result;
}

std::vector<ScriptHashUtxo> ScriptHashIndex::GetUtxos(const uint256& scripthash) const
{
    std::vector<ScriptHashUtxo> result;
    std::unique_ptr<CDBIterator> it(m_db->NewIterator());

    UtxoKey key{};
    it->Seek(UtxoKeyPrefix{scripthash});

    while (it->Valid() && it->GetKey(key) && key.scripthash == scripthash) {
        UtxoValue val;
        if (it->GetValue(val)) {
            result.push_back({COutPoint{key.txid, key.vout}, static_cast<int>(val.height), val.value});
        }
        it->Next();
    }

    return result;
}

CAmount ScriptHashIndex::GetBalance(const uint256& scripthash) const
{
    CAmount balance = 0;
    std::unique_ptr<CDBIterator> it(m_db->NewIterator());

    UtxoKey key{};
    it->Seek(UtxoKeyPrefix{scripthash});

    while (it->Valid() && it->GetKey(key) && key.scripthash == scripthash) {
        UtxoValue val;
        if (it->GetValue(val)) {
            balance += val.value;
        }
        it->Next();
    }

    return balance;
}
