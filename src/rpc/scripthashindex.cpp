// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/scripthashindex.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <uint256.h>
#include <univalue.h>
#include <util/translation.h>

static constexpr auto SCRIPTHASH_ARG_DESCRIPTION{
    "The 32-byte script hash, as SHA256(scriptPubKey) in Bitcoin Core's "
    "uint256 hex byte order."};

// These RPCs intentionally keep Electrum-style scripthash payloads: history and
// UTXO queries return top-level arrays, and amounts are raw satoshis instead of
// Bitcoin Core's usual ValueFromAmount() BTC values.

static uint256 ParseScriptHash(const UniValue& value)
{
    if (!value.isStr()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "scripthash must be a hex string");
    }
    return ParseHashV(value, "scripthash");
}

static void EnsureScriptHashIndexReady()
{
    if (!g_scripthashindex) {
        throw JSONRPCError(RPC_MISC_ERROR, "scripthash index is not enabled");
    }
    if (!g_scripthashindex->BlockUntilSyncedToActiveChain()) {
        throw JSONRPCError(RPC_MISC_ERROR, "scripthash index is catching up");
    }
}

static UniValue ScriptHashActivityToUnivalue(const uint256& scripthash)
{
    EnsureScriptHashIndexReady();

    const auto activity{g_scripthashindex->GetActivity(scripthash)};

    UniValue history_result{UniValue::VARR};
    for (const auto& entry : activity.history) {
        UniValue item{UniValue::VOBJ};
        item.pushKV("txid", entry.txid.GetHex());
        item.pushKV("height", entry.height);
        history_result.push_back(item);
    }

    UniValue utxos_result{UniValue::VARR};
    for (const auto& utxo : activity.utxos) {
        UniValue item{UniValue::VOBJ};
        item.pushKV("txid", utxo.outpoint.hash.GetHex());
        item.pushKV("vout", static_cast<int>(utxo.outpoint.n));
        item.pushKV("height", utxo.height);
        item.pushKV("value", utxo.value);
        utxos_result.push_back(item);
    }

    UniValue result{UniValue::VOBJ};
    result.pushKV("history", std::move(history_result));
    result.pushKV("utxos", std::move(utxos_result));
    result.pushKV("balance", activity.balance);
    result.pushKV("bestblock", activity.bestblock.GetHex());
    result.pushKV("height", activity.height);
    return result;
}

static UniValue ScriptHashHistoryToUnivalue(const uint256& scripthash)
{
    EnsureScriptHashIndexReady();

    UniValue result{UniValue::VARR};
    for (const auto& entry : g_scripthashindex->GetHistory(scripthash)) {
        UniValue item{UniValue::VOBJ};
        item.pushKV("txid", entry.txid.GetHex());
        item.pushKV("height", entry.height);
        result.push_back(item);
    }
    return result;
}

static UniValue ScriptHashUtxosToUnivalue(const uint256& scripthash)
{
    EnsureScriptHashIndexReady();

    UniValue result{UniValue::VARR};
    for (const auto& utxo : g_scripthashindex->GetUtxos(scripthash)) {
        UniValue item{UniValue::VOBJ};
        item.pushKV("txid", utxo.outpoint.hash.GetHex());
        item.pushKV("vout", static_cast<int>(utxo.outpoint.n));
        item.pushKV("height", utxo.height);
        item.pushKV("value", utxo.value);
        result.push_back(item);
    }
    return result;
}

static UniValue ScriptHashBalanceToUnivalue(const uint256& scripthash)
{
    EnsureScriptHashIndexReady();
    UniValue result{UniValue::VOBJ};
    result.pushKV("confirmed", g_scripthashindex->GetBalance(scripthash));
    return result;
}

static RPCMethod getscripthashactivity()
{
    return RPCMethod{
        "getscripthashactivity",
        "Returns confirmed history, UTXOs, and balance for a script hash.\n",
        {
            {"scripthash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, SCRIPTHASH_ARG_DESCRIPTION},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::ARR, "history", "", {
                    {RPCResult::Type::OBJ, "", "", {
                        {RPCResult::Type::STR_HEX, "txid", "Transaction id."},
                        {RPCResult::Type::NUM, "height", "Confirmation height."},
                    }},
                }},
                {RPCResult::Type::ARR, "utxos", "", {
                    {RPCResult::Type::OBJ, "", "", {
                        {RPCResult::Type::STR_HEX, "txid", "Transaction id."},
                        {RPCResult::Type::NUM, "vout", "Output index."},
                        {RPCResult::Type::NUM, "height", "Confirmation height."},
                        {RPCResult::Type::NUM, "value", "Value in satoshis."},
                    }},
                }},
                {RPCResult::Type::NUM, "balance", "Confirmed balance in satoshis."},
                {RPCResult::Type::STR_HEX, "bestblock", "Current chain tip block hash."},
                {RPCResult::Type::NUM, "height", "Current chain tip height."},
            }},
        RPCExamples{HelpExampleCli("getscripthashactivity", "\"0000000000000000000000000000000000000000000000000000000000000000\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            return ScriptHashActivityToUnivalue(ParseScriptHash(request.params[0]));
        },
    };
}

static RPCMethod getscripthashhistory()
{
    return RPCMethod{
        "getscripthashhistory",
        "Returns confirmed history for a script hash.\n",
        {
            {"scripthash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, SCRIPTHASH_ARG_DESCRIPTION},
        },
        RPCResult{
            RPCResult::Type::ARR, "", "", {
                {RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR_HEX, "txid", "Transaction id."},
                    {RPCResult::Type::NUM, "height", "Confirmation height."},
                }},
            }},
        RPCExamples{HelpExampleCli("getscripthashhistory", "\"0000000000000000000000000000000000000000000000000000000000000000\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            return ScriptHashHistoryToUnivalue(ParseScriptHash(request.params[0]));
        },
    };
}

static RPCMethod getscripthashutxos()
{
    return RPCMethod{
        "getscripthashutxos",
        "Returns confirmed UTXOs for a script hash.\n",
        {
            {"scripthash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, SCRIPTHASH_ARG_DESCRIPTION},
        },
        RPCResult{
            RPCResult::Type::ARR, "", "", {
                {RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR_HEX, "txid", "Transaction id."},
                    {RPCResult::Type::NUM, "vout", "Output index."},
                    {RPCResult::Type::NUM, "height", "Confirmation height."},
                    {RPCResult::Type::NUM, "value", "Value in satoshis."},
                }},
            }},
        RPCExamples{HelpExampleCli("getscripthashutxos", "\"0000000000000000000000000000000000000000000000000000000000000000\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            return ScriptHashUtxosToUnivalue(ParseScriptHash(request.params[0]));
        },
    };
}

static RPCMethod getscripthashbalance()
{
    return RPCMethod{
        "getscripthashbalance",
        "Returns confirmed balance for a script hash.\n",
        {
            {"scripthash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, SCRIPTHASH_ARG_DESCRIPTION},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::NUM, "confirmed", "Confirmed balance in satoshis."},
            }},
        RPCExamples{HelpExampleCli("getscripthashbalance", "\"0000000000000000000000000000000000000000000000000000000000000000\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            return ScriptHashBalanceToUnivalue(ParseScriptHash(request.params[0]));
        },
    };
}

void RegisterScriptHashIndexRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"blockchain", &getscripthashactivity},
        {"blockchain", &getscripthashbalance},
        {"blockchain", &getscripthashhistory},
        {"blockchain", &getscripthashutxos},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
