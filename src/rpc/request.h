// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_RPC_REQUEST_H
#define BITCOIN_RPC_REQUEST_H

#include <any>
#include <chrono>
#include <string>

#include <univalue.h>

UniValue JSONRPCRequestObj(const std::string& strMethod, const UniValue& params, const UniValue& id);
UniValue JSONRPCReplyObj(const UniValue& result, const UniValue& error, const UniValue& id);
std::string JSONRPCReply(const UniValue& result, const UniValue& error, const UniValue& id);
UniValue JSONRPCError(int code, const std::string& message);

/** Generate a new RPC authentication cookie and write it to disk */
bool GenerateAuthCookie(std::string *cookie_out);
/** Read the RPC authentication cookie from disk */
bool GetAuthCookie(std::string *cookie_out);
/** Delete RPC authentication cookie from disk */
void DeleteAuthCookie();
/** Parse JSON-RPC batch reply into a vector */
std::vector<UniValue> JSONRPCProcessBatchReply(const UniValue& in);
//! HTTP RPC request timeout. Default to 0 for no expiry
static const unsigned int DEFAULT_HTTP_REQUEST_EXPIRY=0;

class JSONRPCRequest
{
public:
    std::chrono::system_clock::time_point arrival_time;
    unsigned int expire_seconds;
    UniValue id;
    std::string strMethod;
    UniValue params;
    enum Mode { EXECUTE, GET_HELP, GET_ARGS } mode = EXECUTE;
    std::string URI;
    std::string authUser;
    std::string peerAddr;
    std::any context;

    /**
    * Default constructor which initializes `arrival_time` to now()
    */
    JSONRPCRequest();

    /**
    * Parse the id, method, params and expiry of a `JSONRPCRequest`.
    * @param[in]  `valRequest` the request as a `UniValue`
    */
    void parse(const UniValue& valRequest);
};

#endif // BITCOIN_RPC_REQUEST_H
