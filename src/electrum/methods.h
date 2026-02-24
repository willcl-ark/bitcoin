// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ELECTRUM_METHODS_H
#define BITCOIN_ELECTRUM_METHODS_H

#include <electrum/mempoolindex.h>
#include <index/scripthashindex.h>

#include <optional>
#include <string>
#include <vector>

class CBlockHeader;
class ElectrumServer;

std::string SerializeHeaderHex(const CBlockHeader& header);

std::optional<std::string> ComputeScripthashStatusFromParts(
    const std::vector<ScriptHashHistory>& confirmed,
    const std::vector<MempoolScriptHashEntry>& mempool);

std::optional<std::string> GetScripthashStatus(MempoolScriptHashIndex* mempool_index, const uint256& scripthash);

void RegisterElectrumMethods(ElectrumServer& server);

#endif // BITCOIN_ELECTRUM_METHODS_H
