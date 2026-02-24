// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <electrum/methods.h>
#include <electrum/server.h>

#include <chain.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <core_io.h>
#include <crypto/sha256.h>
#include <index/scripthashindex.h>
#include <kernel/cs_main.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <node/transaction.h>
#include <policy/fees/block_policy_estimator.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>
#include <txmempool.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <validation.h>
#include <validationinterface.h>

#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

std::string SerializeHeaderHex(const CBlockHeader& header)
{
    DataStream ss{};
    ss << header;
    return HexStr(ss);
}

std::optional<std::string> ComputeScripthashStatusFromParts(
    const std::vector<ScriptHashHistory>& confirmed,
    const std::vector<MempoolScriptHashEntry>& mempool)
{
    if (confirmed.empty() && mempool.empty()) return std::nullopt;

    std::string status_data;
    for (const auto& entry : confirmed) {
        status_data += entry.txid.GetHex() + ":" + util::ToString(entry.height) + ":";
    }
    for (const auto& entry : mempool) {
        status_data += entry.txid.GetHex() + ":" + util::ToString(entry.height) + ":";
    }

    uint256 hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(status_data.data()), status_data.size()).Finalize(hash.begin());
    return hash.GetHex();
}

std::optional<std::string> GetScripthashStatus(MempoolScriptHashIndex* mempool_index, const uint256& scripthash)
{
    if (!g_scripthashindex) return std::nullopt;
    g_scripthashindex->BlockUntilSyncedToCurrentChain();
    auto confirmed = g_scripthashindex->GetHistory(scripthash);
    auto mempool = mempool_index ? mempool_index->GetEntries(scripthash) : std::vector<MempoolScriptHashEntry>{};
    return ComputeScripthashStatusFromParts(confirmed, mempool);
}

void ElectrumServer::StartMempoolIndex()
{
    m_mempool_index = std::make_shared<MempoolScriptHashIndex>();
    m_node.validation_signals->RegisterSharedValidationInterface(
        std::static_pointer_cast<CValidationInterface>(m_mempool_index));
    if (m_node.mempool) {
        m_mempool_index->LoadFromMempool(*m_node.mempool);
    }
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
}

void ElectrumServer::StopMempoolIndex()
{
    if (m_mempool_index) {
        m_node.validation_signals->UnregisterSharedValidationInterface(
            std::static_pointer_cast<CValidationInterface>(m_mempool_index));
        m_mempool_index.reset();
    }
}

void ElectrumServer::NotifyTimerCb(int /*fd*/, short /*events*/, void* ctx)
{
    static_cast<ElectrumServer*>(ctx)->ProcessNotifications();
}

void ElectrumServer::ProcessNotifications()
{
    {
        LOCK(m_connections_mutex);
        bool any_subs{false};
        for (const auto& [bev, state] : m_connection_state) {
            if (state.subscribed_headers || !state.scripthash_subs.empty()) {
                any_subs = true;
                break;
            }
        }
        if (!any_subs) return;
    }

    ChainstateManager& chainman = *m_node.chainman;
    if (chainman.IsInitialBlockDownload()) return;

    uint256 current_tip;
    int tip_height;
    std::string tip_header_hex;
    {
        LOCK(cs_main);
        const CBlockIndex* tip = chainman.ActiveChain().Tip();
        if (!tip) return;
        current_tip = tip->GetBlockHash();
        tip_height = tip->nHeight;
        tip_header_hex = SerializeHeaderHex(tip->GetBlockHeader());
    }

    const bool tip_changed = (current_tip != m_last_tip_hash);
    bool dirty_all{false};
    std::vector<uint256> dirty_scripthashes = m_mempool_index
        ? m_mempool_index->TakeDirtyScripthashes(dirty_all)
        : std::vector<uint256>{};
    if (!tip_changed && dirty_scripthashes.empty() && !dirty_all) return;

    m_last_tip_hash = current_tip;

    if (tip_changed) {
        UniValue header_notification(UniValue::VOBJ);
        header_notification.pushKV("jsonrpc", "2.0");
        header_notification.pushKV("method", "blockchain.headers.subscribe");
        UniValue header_params(UniValue::VARR);
        UniValue header_obj(UniValue::VOBJ);
        header_obj.pushKV("hex", tip_header_hex);
        header_obj.pushKV("height", tip_height);
        header_params.push_back(header_obj);
        header_notification.pushKV("params", header_params);

        LOCK(m_connections_mutex);
        for (const auto& [bev, state] : m_connection_state) {
            if (state.subscribed_headers) {
                SendResponse(bev, header_notification);
            }
        }
    }

    if (!g_scripthashindex) return;

    std::set<uint256> subscribed_scripthashes;
    {
        LOCK(m_connections_mutex);
        for (const auto& [bev, state] : m_connection_state) {
            for (const auto& [sh, status] : state.scripthash_subs) {
                subscribed_scripthashes.insert(sh);
            }
        }
    }
    if (subscribed_scripthashes.empty()) return;

    std::set<uint256> scripthashes_to_update;
    if (tip_changed || dirty_all) {
        scripthashes_to_update = subscribed_scripthashes;
    } else {
        for (const uint256& sh : dirty_scripthashes) {
            if (subscribed_scripthashes.contains(sh)) {
                scripthashes_to_update.insert(sh);
            }
        }
    }
    if (scripthashes_to_update.empty()) return;

    if (!g_scripthashindex->IsSyncedToCurrentChain()) return;

    std::map<uint256, std::optional<std::string>> new_statuses;
    for (const auto& sh : scripthashes_to_update) {
        auto confirmed = g_scripthashindex->GetHistory(sh);
        auto mempool = m_mempool_index ? m_mempool_index->GetEntries(sh)
                                       : std::vector<MempoolScriptHashEntry>{};
        new_statuses[sh] = ComputeScripthashStatusFromParts(confirmed, mempool);
    }

    LOCK(m_connections_mutex);
    for (auto& [bev, state] : m_connection_state) {
        for (auto& [sh, old_status] : state.scripthash_subs) {
            const auto ns_it = new_statuses.find(sh);
            if (ns_it == new_statuses.end()) continue;
            if (ns_it->second == old_status) continue;

            old_status = ns_it->second;

            UniValue notification(UniValue::VOBJ);
            notification.pushKV("jsonrpc", "2.0");
            notification.pushKV("method", "blockchain.scripthash.subscribe");
            UniValue params(UniValue::VARR);
            params.push_back(sh.GetHex());
            if (ns_it->second.has_value()) {
                params.push_back(*ns_it->second);
            } else {
                params.push_back(UniValue());
            }
            notification.pushKV("params", params);
            SendResponse(bev, notification);
        }
    }
}

void RegisterElectrumMethods(ElectrumServer& server)
{
    auto& node = server.GetNode();
    server.SetScripthashSubscriptionCallbacks(
        [](const uint256& scripthash) {
            if (g_scripthashindex) g_scripthashindex->CacheScriptHash(scripthash);
        },
        [](const uint256& scripthash) {
            if (g_scripthashindex) g_scripthashindex->UncacheScriptHash(scripthash);
        });

    // server.ping
    server.RegisterMethod("server.ping", [](struct bufferevent*, const UniValue&) -> UniValue {
        return UniValue();
    });

    // server.version
    server.RegisterMethod("server.version", [](struct bufferevent*, const UniValue&) -> UniValue {
        UniValue result(UniValue::VARR);
        result.push_back("Bitcoin Core Electrum/0.1");
        result.push_back("1.4");
        return result;
    });

    // server.banner
    server.RegisterMethod("server.banner", [](struct bufferevent*, const UniValue&) -> UniValue {
        return "Bitcoin Core Electrum Server";
    });

    // server.donation_address
    server.RegisterMethod("server.donation_address", [](struct bufferevent*, const UniValue&) -> UniValue {
        return "";
    });

    // server.features
    server.RegisterMethod("server.features", [&node](struct bufferevent*, const UniValue&) -> UniValue {
        UniValue result(UniValue::VOBJ);
        ChainstateManager& chainman = *node.chainman;
        const CBlock& genesis = chainman.GetParams().GenesisBlock();
        result.pushKV("genesis_hash", genesis.GetHash().GetHex());
        result.pushKV("hash_function", "sha256");
        result.pushKV("server_version", "Bitcoin Core Electrum/0.1");
        result.pushKV("protocol_min", "1.4");
        result.pushKV("protocol_max", "1.4");
        result.pushKV("pruning", UniValue());
        return result;
    });

    // blockchain.block.header
    server.RegisterMethod("blockchain.block.header", [&node](struct bufferevent*, const UniValue& params) -> UniValue {
        if (params.size() < 1) throw std::runtime_error("missing height parameter");
        int height = params[0].getInt<int>();

        ChainstateManager& chainman = *node.chainman;
        LOCK(cs_main);
        CChain& active_chain = chainman.ActiveChain();

        if (height < 0 || height > active_chain.Height()) {
            throw std::runtime_error("height out of range");
        }

        const CBlockIndex* pindex = active_chain[height];
        return SerializeHeaderHex(pindex->GetBlockHeader());
    });

    // blockchain.block.headers
    server.RegisterMethod("blockchain.block.headers", [&node](struct bufferevent*, const UniValue& params) -> UniValue {
        if (params.size() < 2) throw std::runtime_error("missing start_height or count");
        int start_height = params[0].getInt<int>();
        int count = params[1].getInt<int>();
        if (count < 0) count = 0;
        if (count > 2016) count = 2016;

        ChainstateManager& chainman = *node.chainman;
        std::vector<CBlockHeader> headers;
        {
            LOCK(cs_main);
            CChain& active_chain = chainman.ActiveChain();

            if (start_height < 0 || start_height > active_chain.Height()) {
                throw std::runtime_error("start_height out of range");
            }

            int actual_count = std::min(count, active_chain.Height() - start_height + 1);
            headers.reserve(actual_count);
            for (int i = 0; i < actual_count; ++i) {
                headers.push_back(active_chain[start_height + i]->GetBlockHeader());
            }
        }

        DataStream ss{};
        for (const auto& header : headers) {
            ss << header;
        }

        UniValue result(UniValue::VOBJ);
        result.pushKV("count", static_cast<int>(headers.size()));
        result.pushKV("hex", HexStr(ss));
        result.pushKV("max", 2016);
        return result;
    });

    // blockchain.estimatefee
    server.RegisterMethod("blockchain.estimatefee", [&node](struct bufferevent*, const UniValue& params) -> UniValue {
        if (params.size() < 1) throw std::runtime_error("missing number parameter");
        int target = params[0].getInt<int>();

        if (!node.fee_estimator) {
            return -1;
        }

        CFeeRate fee_rate = node.fee_estimator->estimateSmartFee(target, nullptr, false);
        if (fee_rate == CFeeRate(0)) {
            return -1;
        }

        // Electrum expects BTC/kB as a float
        return ValueFromAmount(fee_rate.GetFeePerK());
    });

    // blockchain.relayfee
    server.RegisterMethod("blockchain.relayfee", [&node](struct bufferevent*, const UniValue&) -> UniValue {
        if (!node.mempool) throw std::runtime_error("mempool not available");
        return ValueFromAmount(node.mempool->m_opts.min_relay_feerate.GetFeePerK());
    });

    // blockchain.scripthash.get_balance
    server.RegisterMethod("blockchain.scripthash.get_balance", [&server](struct bufferevent*, const UniValue& params) -> UniValue {
        if (params.size() < 1) throw std::runtime_error("missing scripthash");
        if (!g_scripthashindex) throw std::runtime_error("electrum index not enabled");

        uint256 sh = uint256::FromHex(params[0].get_str()).value();
        g_scripthashindex->BlockUntilSyncedToCurrentChain();

        CAmount confirmed = g_scripthashindex->GetBalance(sh);
        CAmount unconfirmed = server.GetMempoolIndex()->GetBalanceDelta(sh);

        UniValue result(UniValue::VOBJ);
        result.pushKV("confirmed", confirmed);
        result.pushKV("unconfirmed", unconfirmed);
        return result;
    });

    // blockchain.scripthash.get_history
    server.RegisterMethod("blockchain.scripthash.get_history", [](struct bufferevent*, const UniValue& params) -> UniValue {
        if (params.size() < 1) throw std::runtime_error("missing scripthash");
        if (!g_scripthashindex) throw std::runtime_error("electrum index not enabled");

        uint256 sh = uint256::FromHex(params[0].get_str()).value();
        g_scripthashindex->BlockUntilSyncedToCurrentChain();

        auto history = g_scripthashindex->GetHistory(sh);

        UniValue result(UniValue::VARR);
        for (const auto& entry : history) {
            UniValue item(UniValue::VOBJ);
            item.pushKV("tx_hash", entry.txid.GetHex());
            item.pushKV("height", entry.height);
            result.push_back(item);
        }
        return result;
    });

    // blockchain.scripthash.get_mempool
    server.RegisterMethod("blockchain.scripthash.get_mempool", [&server](struct bufferevent*, const UniValue& params) -> UniValue {
        if (params.size() < 1) throw std::runtime_error("missing scripthash");
        if (!g_scripthashindex) throw std::runtime_error("electrum index not enabled");

        uint256 sh = uint256::FromHex(params[0].get_str()).value();
        UniValue result(UniValue::VARR);
        for (const auto& entry : server.GetMempoolIndex()->GetEntries(sh)) {
            UniValue item(UniValue::VOBJ);
            item.pushKV("tx_hash", entry.txid.GetHex());
            item.pushKV("height", entry.height);
            item.pushKV("fee", entry.fee);
            result.push_back(item);
        }
        return result;
    });

    // blockchain.headers.subscribe
    server.RegisterMethod("blockchain.headers.subscribe", [&node, &server](struct bufferevent* bev, const UniValue&) -> UniValue {
        ChainstateManager& chainman = *node.chainman;
        std::string tip_header_hex;
        int tip_height;
        {
            LOCK(cs_main);
            const CBlockIndex* tip = chainman.ActiveChain().Tip();
            if (!tip) throw std::runtime_error("no active chain");
            tip_header_hex = SerializeHeaderHex(tip->GetBlockHeader());
            tip_height = tip->nHeight;
        }

        server.SubscribeHeaders(bev);

        UniValue result(UniValue::VOBJ);
        result.pushKV("hex", tip_header_hex);
        result.pushKV("height", tip_height);
        return result;
    });

    // blockchain.scripthash.subscribe
    server.RegisterMethod("blockchain.scripthash.subscribe", [&server](struct bufferevent* bev, const UniValue& params) -> UniValue {
        if (params.size() < 1) throw std::runtime_error("missing scripthash");
        if (!g_scripthashindex) throw std::runtime_error("electrum index not enabled");

        uint256 sh = uint256::FromHex(params[0].get_str()).value();
        const auto status = GetScripthashStatus(server.GetMempoolIndex(), sh);

        server.SubscribeScripthash(bev, sh, status);

        if (!status.has_value()) return UniValue();
        return *status;
    });

    // blockchain.scripthashes.subscribe
    server.RegisterMethod("blockchain.scripthashes.subscribe", [&server](struct bufferevent* bev, const UniValue& params) -> UniValue {
        if (params.size() < 1) throw std::runtime_error("missing scripthashes");
        if (!g_scripthashindex) throw std::runtime_error("electrum index not enabled");

        const bool wrapped_list{params.size() == 1 && params[0].isArray()};
        const std::vector<UniValue> items{wrapped_list ? params[0].getValues() : params.getValues()};
        if (items.empty()) throw std::runtime_error("missing scripthashes");

        std::vector<std::pair<uint256, std::optional<std::string>>> subscriptions;
        subscriptions.reserve(items.size());
        UniValue result(UniValue::VARR);

        for (const UniValue& item : items) {
            if (!item.isStr()) throw std::runtime_error("scripthash must be string");
            const uint256 sh{uint256::FromHex(item.get_str()).value()};
            const auto status{GetScripthashStatus(server.GetMempoolIndex(), sh)};
            subscriptions.emplace_back(sh, status);
            if (status.has_value()) {
                result.push_back(*status);
            } else {
                result.push_back(UniValue());
            }
        }

        server.SubscribeScripthashes(bev, subscriptions);
        return result;
    });

    // blockchain.scripthash.unsubscribe
    server.RegisterMethod("blockchain.scripthash.unsubscribe", [&server](struct bufferevent* bev, const UniValue& params) -> UniValue {
        if (params.size() < 1) throw std::runtime_error("missing scripthash");
        uint256 sh = uint256::FromHex(params[0].get_str()).value();
        return server.UnsubscribeScripthash(bev, sh);
    });

    // blockchain.scripthash.listunspent
    server.RegisterMethod("blockchain.scripthash.listunspent", [](struct bufferevent*, const UniValue& params) -> UniValue {
        if (params.size() < 1) throw std::runtime_error("missing scripthash");
        if (!g_scripthashindex) throw std::runtime_error("electrum index not enabled");

        uint256 sh = uint256::FromHex(params[0].get_str()).value();
        g_scripthashindex->BlockUntilSyncedToCurrentChain();

        auto utxos = g_scripthashindex->GetUtxos(sh);

        UniValue result(UniValue::VARR);
        for (const auto& utxo : utxos) {
            UniValue item(UniValue::VOBJ);
            item.pushKV("tx_hash", utxo.outpoint.hash.GetHex());
            item.pushKV("tx_pos", static_cast<int>(utxo.outpoint.n));
            item.pushKV("height", utxo.height);
            item.pushKV("value", utxo.value);
            result.push_back(item);
        }
        return result;
    });

    // blockchain.transaction.broadcast
    server.RegisterMethod("blockchain.transaction.broadcast", [&node](struct bufferevent*, const UniValue& params) -> UniValue {
        if (params.size() < 1) throw std::runtime_error("missing raw_tx");

        auto tx_data = TryParseHex<unsigned char>(params[0].get_str());
        if (!tx_data) throw std::runtime_error("invalid hex");

        DataStream ss{*tx_data};
        CMutableTransaction mtx;
        ss >> TX_WITH_WITNESS(mtx);
        CTransactionRef tx = MakeTransactionRef(std::move(mtx));

        std::string err_string;
        node::TransactionError err = node::BroadcastTransaction(node, tx, err_string, 0, node::TxBroadcast::MEMPOOL_AND_BROADCAST_TO_ALL, /*wait_callback=*/false);
        if (err != node::TransactionError::OK) {
            throw std::runtime_error(err_string);
        }

        return tx->GetHash().GetHex();
    });

    // blockchain.transaction.get
    server.RegisterMethod("blockchain.transaction.get", [&node](struct bufferevent*, const UniValue& params) -> UniValue {
        if (params.size() < 1) throw std::runtime_error("missing tx_hash");

        Txid txid = Txid::FromUint256(uint256::FromHex(params[0].get_str()).value());
        uint256 hash_block;

        CTransactionRef tx = node::GetTransaction(
            /*block_index=*/nullptr,
            node.mempool.get(),
            txid,
            node.chainman->m_blockman,
            hash_block);

        if (!tx) throw std::runtime_error("transaction not found");

        DataStream ss{};
        ss << TX_WITH_WITNESS(*tx);
        return HexStr(ss);
    });

    // blockchain.transaction.get_merkle
    server.RegisterMethod("blockchain.transaction.get_merkle", [&node](struct bufferevent*, const UniValue& params) -> UniValue {
        if (params.size() < 2) throw std::runtime_error("missing tx_hash or height");

        Txid txid = Txid::FromUint256(uint256::FromHex(params[0].get_str()).value());
        int height = params[1].getInt<int>();

        ChainstateManager& chainman = *node.chainman;
        CBlock block;
        const CBlockIndex* pindex;
        {
            LOCK(cs_main);
            CChain& active_chain = chainman.ActiveChain();
            if (height < 0 || height > active_chain.Height()) {
                throw std::runtime_error("height out of range");
            }
            pindex = active_chain[height];
        }

        if (!chainman.m_blockman.ReadBlock(block, *pindex)) {
            throw std::runtime_error("failed to read block from disk");
        }

        int pos = -1;
        for (size_t i = 0; i < block.vtx.size(); ++i) {
            if (block.vtx[i]->GetHash() == txid) {
                pos = static_cast<int>(i);
                break;
            }
        }
        if (pos < 0) throw std::runtime_error("transaction not found in block");

        auto merkle_branch = TransactionMerklePath(block, pos);

        UniValue merkle_arr(UniValue::VARR);
        for (const auto& hash : merkle_branch) {
            merkle_arr.push_back(hash.GetHex());
        }

        UniValue result(UniValue::VOBJ);
        result.pushKV("merkle", merkle_arr);
        result.pushKV("block_height", height);
        result.pushKV("pos", pos);
        return result;
    });

    // blockchain.transaction.id_from_pos
    server.RegisterMethod("blockchain.transaction.id_from_pos", [&node](struct bufferevent*, const UniValue& params) -> UniValue {
        if (params.size() < 2) throw std::runtime_error("missing height or tx_pos");

        int height = params[0].getInt<int>();
        int tx_pos = params[1].getInt<int>();
        bool merkle = params.size() > 2 && params[2].get_bool();

        ChainstateManager& chainman = *node.chainman;
        CBlock block;
        const CBlockIndex* pindex;
        {
            LOCK(cs_main);
            CChain& active_chain = chainman.ActiveChain();
            if (height < 0 || height > active_chain.Height()) {
                throw std::runtime_error("height out of range");
            }
            pindex = active_chain[height];
        }

        if (!chainman.m_blockman.ReadBlock(block, *pindex)) {
            throw std::runtime_error("failed to read block from disk");
        }

        if (tx_pos < 0 || tx_pos >= static_cast<int>(block.vtx.size())) {
            throw std::runtime_error("tx_pos out of range");
        }

        std::string txid_hex = block.vtx[tx_pos]->GetHash().GetHex();

        if (!merkle) {
            return txid_hex;
        }

        auto merkle_branch = TransactionMerklePath(block, tx_pos);
        UniValue merkle_arr(UniValue::VARR);
        for (const auto& hash : merkle_branch) {
            merkle_arr.push_back(hash.GetHex());
        }

        UniValue result(UniValue::VOBJ);
        result.pushKV("tx_hash", txid_hex);
        result.pushKV("merkle", merkle_arr);
        return result;
    });

    // mempool.get_fee_histogram
    server.RegisterMethod("mempool.get_fee_histogram", [&server](struct bufferevent*, const UniValue&) -> UniValue {
        if (!server.GetMempoolIndex()) throw std::runtime_error("mempool index not available");

        UniValue result(UniValue::VARR);
        for (const auto& [label, vsize] : server.GetMempoolIndex()->GetFeeHistogram()) {
            UniValue pair(UniValue::VARR);
            pair.push_back(label);
            pair.push_back(vsize);
            result.push_back(pair);
        }
        return result;
    });
}
