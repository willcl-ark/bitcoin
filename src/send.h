// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SEND_H
#define BITCOIN_SEND_H

#include <netaddress.h>
#include <primitives/transaction.h>
#include <util/time.h>

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class CThreadInterrupt;
class Sock;
class Transport;

namespace txsend {

enum class SessionResult {
    SUCCESS,
    PEER_ERROR,
    INTERRUPTED,
};
enum class SendResult {
    SUCCESS,
    PEER_ERROR,
    PROXY_ERROR,
    LOCAL_ERROR,
    DISCLOSED,
    INTERRUPTED,
};

/** Input checks do not create sockets, resolve names, or consult node state. */
std::optional<std::vector<CService>> ParseDestinations(std::span<const std::string> entries, uint16_t default_port);
std::optional<CService> ParseProxy(std::string_view entry);
std::optional<std::chrono::milliseconds> ParseTimeout(std::string_view seconds);
CTransactionRef ReadTransaction(std::istream& input);

/** Drive one already-proxied connection. The caller must select chain parameters
 * and keep an ECC context alive for the transport's lifetime. disclosure_started
 * is irreversible, including when the first announcement write fails. */
SessionResult RunSession(const Sock& sock, Transport& transport, const CTransaction& tx,
                         MockableSteadyClock::time_point deadline,
                         const CThreadInterrupt& interrupt, bool& disclosure_started);

/** Try validated onion destinations in order, connecting only to the proxy.
 * Each physical attempt gets fresh credentials, transport and deadline. */
SendResult SendTransaction(const CTransaction& tx, std::span<const CService> destinations,
                           const CService& proxy, std::chrono::milliseconds timeout,
                           const CThreadInterrupt& interrupt);

} // namespace txsend

#endif // BITCOIN_SEND_H
