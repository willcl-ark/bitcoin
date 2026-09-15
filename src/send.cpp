// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <send.h>

#include <compat/compat.h>
#include <consensus/consensus.h>
#include <consensus/tx_check.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <netaddress.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <primitives/transaction.h>
#include <protocol.h>
#include <random.h>
#include <serialize.h>
#include <streams.h>
#include <transport.h>
#include <util/sock.h>
#include <util/strencodings.h>
#include <util/threadinterrupt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <ios>
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace txsend {
namespace {

constexpr int32_t SEND_VERSION{70016};
constexpr size_t MAX_PEER_USER_AGENT{256};
constexpr size_t MAX_RECEIVED_BYTES{8 * 1024 * 1024};
constexpr size_t MAX_RECEIVED_MESSAGES{1024};
constexpr size_t IO_CHUNK{4096};

uint64_t RandomNonce()
{
    uint64_t nonce;
    do {
        nonce = GetRandHash().GetUint64(0);
    } while (nonce == 0);
    return nonce;
}

CSerializedNetMsg VersionMessage()
{
    return NetMsg::Make(NetMsgType::VERSION, SEND_VERSION, uint64_t{NODE_NONE}, int64_t{0},
                        uint64_t{NODE_NONE}, CNetAddr::V1(CService{}),
                        uint64_t{NODE_NONE}, CNetAddr::V1(CService{}), RandomNonce(),
                        std::string{"/pynode:0.0.1/"}, int32_t{0}, false);
}

bool EligibleVersion(DataStream& data)
{
    // All fields through starting height are required; only relay is optional.
    if (data.size() < 85 || data.size() > 85 + 3 + MAX_PEER_USER_AGENT + 1) return false;
    int32_t version;
    uint64_t services;
    int64_t timestamp;
    uint64_t nonce;
    int32_t height;
    std::string user_agent;
    data >> version >> services >> timestamp;
    data.ignore(52); // Two version addresses, including their service flags.
    data >> nonce >> LIMITED_STRING(user_agent, MAX_PEER_USER_AGENT) >> height;
    bool relay{true};
    if (!data.empty()) {
        uint8_t relay_byte;
        data >> relay_byte;
        if (relay_byte > 1) return false;
        relay = relay_byte != 0;
    }
    return data.empty() && version >= SEND_VERSION && (services & NODE_WITNESS) &&
           (services & (NODE_NETWORK | NODE_NETWORK_LIMITED)) && relay;
}

enum class Stage {
    SEND_VERSION,
    WAIT_VERSION,
    SEND_VERACK,
    WAIT_VERACK,
    SEND_INV,
    WAIT_GETDATA,
    SEND_TX,
    SEND_PING,
    WAIT_PONG,
    DONE,
};

class Session
{
    const CTransaction& m_tx;
    Stage m_stage{Stage::SEND_VERSION};
    bool m_remote_version{false};
    bool m_remote_verack{false};
    bool m_wtxidrelay{false};
    bool m_sendaddrv2{false};
    uint64_t m_ping_nonce{0};

public:
    explicit Session(const CTransaction& tx) : m_tx{tx} {}

    std::optional<CSerializedNetMsg> NextMessage()
    {
        if (m_stage == Stage::WAIT_VERSION && m_remote_version) m_stage = Stage::SEND_VERACK;
        if (m_stage == Stage::WAIT_VERACK && m_remote_verack) m_stage = Stage::SEND_INV;
        switch (m_stage) {
        case Stage::SEND_VERSION: return VersionMessage();
        case Stage::SEND_VERACK: return NetMsg::Make(NetMsgType::VERACK);
        case Stage::SEND_INV: return NetMsg::Make(NetMsgType::INV, std::vector<CInv>{CInv{MSG_TX, m_tx.GetHash().ToUint256()}});
        case Stage::SEND_TX: return NetMsg::Make(NetMsgType::TX, TX_WITH_WITNESS(m_tx));
        case Stage::SEND_PING: return NetMsg::Make(NetMsgType::PING, m_ping_nonce);
        default: return std::nullopt;
        }
    }

    void Flushed()
    {
        switch (m_stage) {
        case Stage::SEND_VERSION: m_stage = Stage::WAIT_VERSION; break;
        case Stage::SEND_VERACK: m_stage = Stage::WAIT_VERACK; break;
        case Stage::SEND_INV: m_stage = Stage::WAIT_GETDATA; break;
        case Stage::SEND_TX:
            m_ping_nonce = RandomNonce(); // Only after the entire tx reaches the socket.
            m_stage = Stage::SEND_PING;
            break;
        case Stage::SEND_PING: m_stage = Stage::WAIT_PONG; break;
        default: break;
        }
    }

    bool Process(CNetMessage& msg)
    {
        auto& data = msg.m_recv;
        if (msg.m_type == NetMsgType::VERSION) {
            if (m_remote_version || !EligibleVersion(data)) return false;
            m_remote_version = true;
            return true;
        }
        if (!m_remote_version) return false;
        if (msg.m_type == NetMsgType::VERACK) {
            if (!data.empty() || m_remote_verack) return false;
            m_remote_verack = true;
        } else if (msg.m_type == NetMsgType::WTXIDRELAY || msg.m_type == NetMsgType::SENDADDRV2) {
            bool& seen = msg.m_type == NetMsgType::WTXIDRELAY ? m_wtxidrelay : m_sendaddrv2;
            if (!data.empty() || m_remote_verack || seen) return false;
            seen = true;
        } else if (msg.m_type == NetMsgType::GETDATA) {
            // Validate the count and size before deserializing any container.
            if (m_stage != Stage::WAIT_GETDATA || data.size() != 37) return false;
            uint8_t count;
            CInv inv;
            data >> count >> inv;
            if (count != 1 || inv.type != MSG_TX || inv.hash != m_tx.GetHash().ToUint256()) return false;
            m_stage = Stage::SEND_TX;
        } else if (msg.m_type == NetMsgType::PONG) {
            if (data.size() != sizeof(uint64_t)) return false;
            uint64_t nonce;
            data >> nonce;
            if (m_stage == Stage::WAIT_PONG && nonce == m_ping_nonce) m_stage = Stage::DONE;
        }
        return true;
    }

    bool Done() const { return m_stage == Stage::DONE; }
};

} // namespace

std::optional<std::vector<CService>> ParseDestinations(std::span<const std::string> entries, uint16_t default_port)
{
    if (entries.empty()) return std::nullopt;
    std::vector<CService> destinations;
    for (const auto& entry : entries) {
        const auto colon = entry.find(':');
        const std::string_view host{entry.data(), colon == std::string::npos ? entry.size() : colon};
        uint16_t port{default_port};
        if (colon != std::string::npos) {
            const auto parsed = ToIntegral<uint16_t>(std::string_view{entry}.substr(colon + 1));
            if (!parsed) return std::nullopt;
            port = *parsed;
        }
        CNetAddr address;
        if (port == 0 || !address.SetSpecial(host) || !address.IsTor()) return std::nullopt;
        const CService destination{address, port};
        if (std::find(destinations.begin(), destinations.end(), destination) == destinations.end()) destinations.push_back(destination);
    }
    return destinations;
}

std::optional<CService> ParseProxy(std::string_view entry)
{
    // Split strictly: IPv6 must be bracketed and every proxy needs a port.
    std::string_view host, port_string;
    bool ipv6{false};
    if (entry.starts_with('[')) {
        const auto close = entry.find(']');
        if (close == std::string_view::npos || close + 1 >= entry.size() || entry[close + 1] != ':') return std::nullopt;
        host = entry.substr(1, close - 1);
        port_string = entry.substr(close + 2);
        ipv6 = true;
    } else {
        const auto colon = entry.find(':');
        if (colon == std::string_view::npos) return std::nullopt;
        host = entry.substr(0, colon);
        port_string = entry.substr(colon + 1);
    }
    const auto port = ToIntegral<uint16_t>(port_string);
    if (!port || *port == 0) return std::nullopt;
    const auto address = LookupHost(std::string{host}, false);
    if (!address || !address->IsValid() || !address->IsLocal()) return std::nullopt;
    if (ipv6 ? !address->IsIPv6() : !address->IsIPv4()) return std::nullopt;
    // IsLocal also includes 0.0.0.0/8, which is not IPv4 loopback.
    if (address->IsIPv4() && (address->GetLinkedIPv4() >> 24) != 127) return std::nullopt;
    return CService{*address, *port};
}

std::optional<std::chrono::milliseconds> ParseTimeout(std::string_view seconds)
{
    const auto parsed = ToIntegral<uint64_t>(seconds);
    const auto max_seconds = std::min(std::chrono::duration_cast<std::chrono::seconds>(MockableSteadyClock::time_point::max() - MockableSteadyClock::now()),
                                      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds::max()))
                                 .count();
    if (!parsed || *parsed == 0 || *parsed > static_cast<uint64_t>(max_seconds)) return std::nullopt;
    const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds{*parsed});
    return timeout;
}

CTransactionRef ReadTransaction(std::istream& input)
{
    constexpr size_t MAX_HEX_RECORD{2 * MAX_BLOCK_WEIGHT + 2};
    std::string record;
    std::array<char, IO_CHUNK> buffer;
    while (!input.eof()) {
        const auto count = std::min(buffer.size(), MAX_HEX_RECORD - record.size() + 1);
        input.read(buffer.data(), count);
        const auto read = input.gcount();
        if (static_cast<size_t>(read) > MAX_HEX_RECORD - record.size()) return {};
        record.append(buffer.data(), read);
        if (input.bad() || (input.fail() && !input.eof())) return {};
    }
    if (record.ends_with('\n')) {
        record.pop_back();
        if (record.ends_with('\r')) record.pop_back();
    }
    if (record.empty() || record.size() % 2 != 0 || !IsHex(record)) return {};
    CMutableTransaction decoded;
    if (!DecodeHexTx(decoded, record, /*try_no_witness=*/false, /*try_witness=*/true)) return {};
    const auto bytes = ParseHex(record);
    DataStream round_trip;
    round_trip << TX_WITH_WITNESS(decoded);
    const auto byte_span = std::as_bytes(std::span{bytes});
    if (!std::equal(round_trip.begin(), round_trip.end(), byte_span.begin(), byte_span.end())) return {};
    auto tx = MakeTransactionRef(std::move(decoded));
    TxValidationState state;
    if (tx->IsCoinBase() || !CheckTransaction(*tx, state) || GetTransactionWeight(*tx) > static_cast<int32_t>(MAX_BLOCK_WEIGHT)) return {};
    return tx;
}

SessionResult RunSession(const Sock& sock, Transport& transport, const CTransaction& tx,
                         MockableSteadyClock::time_point deadline,
                         const CThreadInterrupt& interrupt, bool& disclosure_started)
{
    Session session{tx};
    std::optional<CSerializedNetMsg> pending;
    bool active_message{false};
    size_t received_bytes{0};
    size_t received_messages{0};
    const auto failure = [&] { return interrupt ? SessionResult::INTERRUPTED : SessionResult::PEER_ERROR; };
    const auto expired = [&] { return interrupt || MockableSteadyClock::now() >= deadline; };
    try {
        while (!expired()) {
            bool progress{false};
            if (!active_message) {
                if (!pending) pending = session.NextMessage();
                // A ready send cipher alone does not confirm v2 negotiation.
                if (pending && transport.GetInfo().transport_type != TransportProtocolType::DETECTING &&
                    transport.SetMessageToSend(*pending)) {
                    pending.reset();
                    active_message = true;
                }
            }
            const auto [bytes, more, type] = transport.GetBytesToSend(false);
            if (!bytes.empty()) {
                if (expired()) return failure();
                // This is the sole disclosure boundary, before even a failed write.
                if (type == NetMsgType::INV) disclosure_started = true;
                const ssize_t sent = sock.Send(bytes.data(), std::min(bytes.size(), IO_CHUNK), MSG_NOSIGNAL);
                if (sent > 0) {
                    transport.MarkBytesSent(sent);
                    progress = true;
                } else if (sent == 0 || IOErrorIsPermanent(WSAGetLastError())) {
                    return failure();
                }
            }
            const auto [remaining, more_remaining, remaining_type] = transport.GetBytesToSend(false);
            if (active_message && remaining.empty() && !more_remaining) {
                session.Flushed();
                active_message = false;
                progress = true;
            }
            if (expired()) return failure();
            std::array<uint8_t, IO_CHUNK> buffer;
            if (received_bytes == MAX_RECEIVED_BYTES) return failure();
            const ssize_t received = sock.Recv(buffer.data(), std::min(buffer.size(), MAX_RECEIVED_BYTES - received_bytes), 0);
            if (received > 0) {
                if (static_cast<size_t>(received) > MAX_RECEIVED_BYTES - received_bytes) return failure();
                received_bytes += received;
                progress = true;
                std::span<const uint8_t> input{buffer.data(), static_cast<size_t>(received)};
                while (!input.empty()) {
                    if (expired()) return failure();
                    const size_t before = input.size();
                    if (!transport.ReceivedBytes(input)) return failure();
                    if (transport.ReceivedMessageComplete()) {
                        if (++received_messages > MAX_RECEIVED_MESSAGES) return failure();
                        bool rejected{false};
                        auto msg = transport.GetReceivedMessage(NodeClock::epoch, rejected);
                        if (rejected || !session.Process(msg)) return failure();
                        if (session.Done()) return expired() ? failure() : SessionResult::SUCCESS;
                    } else if (input.size() == before) {
                        return failure();
                    }
                }
            } else if (received == 0 || IOErrorIsPermanent(WSAGetLastError())) {
                return failure();
            }
            if (!progress) {
                if (expired()) return failure();
                const auto wait = std::min(std::chrono::ceil<std::chrono::milliseconds>(deadline - MockableSteadyClock::now()), std::chrono::milliseconds{MAX_WAIT_FOR_IO});
                const auto events = remaining.empty() ? Sock::RecvEvent : Sock::RecvEvent | Sock::SendEvent;
                if (!sock.Wait(wait, events)) return failure();
            }
        }
    } catch (const std::ios_base::failure&) {
        return failure();
    }
    return failure();
}

SendResult SendTransaction(const CTransaction& tx, std::span<const CService> destinations,
                           const CService& proxy, std::chrono::milliseconds timeout,
                           const CThreadInterrupt& interrupt)
{
    bool disclosure_started{false};
    try {
        for (const auto& destination : destinations) {
            for (bool v1 : {false, true}) {
                if (interrupt) return SendResult::INTERRUPTED;
                const auto now = MockableSteadyClock::now();
                if (timeout > std::chrono::duration_cast<std::chrono::milliseconds>(MockableSteadyClock::time_point::max() - now)) return SendResult::PEER_ERROR;
                const auto deadline = now + timeout;
                auto sock = ConnectDirectly(proxy, true, timeout, deadline, interrupt);
                if (interrupt) return SendResult::INTERRUPTED;
                if (!sock) return SendResult::PROXY_ERROR;
                std::array<unsigned char, 32> token;
                GetStrongRandBytes(token);
                const ProxyCredentials credentials{"<torS0X>0", HexStr(token)};
                const auto socks_result = Socks5(destination.ToStringAddr(), destination.GetPort(), &credentials,
                                                 *sock, Socks5AuthPolicy::REQUIRE_AUTH, deadline, interrupt);
                if (interrupt) return SendResult::INTERRUPTED;
                if (socks_result == Socks5Result::PROXY_ERROR) return SendResult::PROXY_ERROR;
                if (socks_result == Socks5Result::DESTINATION_ERROR) break;
                std::unique_ptr<Transport> transport;
                if (v1)
                    transport = std::make_unique<V1Transport>(0);
                else
                    transport = std::make_unique<V2Transport>(0, true);
                const auto result = RunSession(*sock, *transport, tx, deadline, interrupt, disclosure_started);
                if (result == SessionResult::SUCCESS) return SendResult::SUCCESS;
                if (disclosure_started) return SendResult::DISCLOSED;
                if (result == SessionResult::INTERRUPTED) return SendResult::INTERRUPTED;
                if (v1 || !transport->ShouldReconnectV1()) break;
            }
        }
    } catch (const std::exception&) {
        if (disclosure_started) return SendResult::DISCLOSED;
        return interrupt ? SendResult::INTERRUPTED : SendResult::LOCAL_ERROR;
    }
    return SendResult::PEER_ERROR;
}

} // namespace txsend
