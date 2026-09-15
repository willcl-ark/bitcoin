// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bip324.h>
#include <compat/compat.h>
#include <consensus/consensus.h>
#include <netaddress.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <primitives/transaction.h>
#include <protocol.h>
#include <script/script.h>
#include <send.h>
#include <serialize.h>
#include <streams.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <test/util/time.h>
#include <transport.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/threadinterrupt.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

CTransaction TestTransaction(bool witness = true)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{1}), 0});
    tx.vout.emplace_back(1, CScript{});
    if (witness) tx.vin[0].scriptWitness.stack = {{1, 2, 3}};
    return CTransaction{tx};
}

CSerializedNetMsg PeerVersion(int32_t version, uint64_t services, bool relay, std::string user_agent = "/test-peer/")
{
    return NetMsg::Make(NetMsgType::VERSION, version, services, int64_t{123},
                        uint64_t{0}, CNetAddr::V1(CService{}),
                        uint64_t{0}, CNetAddr::V1(CService{}), uint64_t{42},
                        std::move(user_agent), int32_t{10}, relay);
}

class PeerSock : public ZeroSock
{
    const bool m_v2;
    mutable std::unique_ptr<Transport> m_peer;
    mutable std::deque<CSerializedNetMsg> m_outgoing;
    const CTransaction& m_tx;
    FakeSteadyClock& m_clock;

    static void WouldBlock()
    {
#ifdef WIN32
        WSASetLastError(WSAEWOULDBLOCK);
#else
        errno = EWOULDBLOCK;
#endif
    }

    void Process(CNetMessage& msg) const
    {
        commands.push_back(msg.m_type);
        if (msg.m_type == NetMsgType::VERSION) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(msg.m_recv.data());
            version_bytes.assign(bytes, bytes + msg.m_recv.size());
            if (verack_first) m_outgoing.push_back(NetMsg::Make(NetMsgType::VERACK));
            auto version = PeerVersion(peer_version, services, relay, std::string(user_agent_size, 'a'));
            if (omit_relay) version.data.pop_back();
            if (version_fault == 1) version.data.resize(84);
            if (version_fault == 2) version.data.resize(version.data.size() - 2);
            if (version_fault == 3) version.data.back() = 2;
            if (version_fault == 4) version.data.push_back(0);
            m_outgoing.push_back(std::move(version));
            if (duplicate_version) m_outgoing.push_back(PeerVersion(peer_version, services, relay));
            auto negotiation = NetMsg::Make(NetMsgType::WTXIDRELAY);
            if (malformed_negotiation) negotiation.data.push_back(1);
            m_outgoing.push_back(std::move(negotiation));
            m_outgoing.push_back(NetMsg::Make(NetMsgType::SENDADDRV2));
            m_outgoing.push_back(NetMsg::Make(NetMsgType::VERACK));
            if (duplicate_verack) m_outgoing.push_back(NetMsg::Make(NetMsgType::VERACK));
            // These are valid pongs but cannot confirm a transaction handoff.
            m_outgoing.push_back(NetMsg::Make(NetMsgType::PONG, uint64_t{0}));
            for (size_t i = 0; i < flood_messages; ++i)
                m_outgoing.push_back(NetMsg::Make("unknown"));
            for (size_t i = 0; i < flood_bytes; ++i) {
                auto padding = NetMsg::Make("unknown");
                padding.data.resize(4'000'000);
                m_outgoing.push_back(std::move(padding));
            }
        } else if (msg.m_type == NetMsgType::INV && request) {
            const CInv inv{request_type, wrong_hash ? uint256{2} : m_tx.GetHash().ToUint256()};
            auto getdata = NetMsg::Make(NetMsgType::GETDATA, std::vector<CInv>{inv});
            if (bad_count) getdata.data[0] = 255;
            if (duplicate_request) m_outgoing.push_back(getdata.Copy());
            m_outgoing.push_back(std::move(getdata));
        } else if (msg.m_type == NetMsgType::TX) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(msg.m_recv.data());
            tx_bytes.assign(bytes, bytes + msg.m_recv.size());
        } else if (msg.m_type == NetMsgType::PING) {
            msg.m_recv >> ping_nonce;
            if (answer_ping) {
                if (wire_target != 0) {
                    const size_t overhead = m_v2 ? BIP324Cipher::EXPANSION + 13 : 24;
                    const size_t pong_size = m_v2 ? BIP324Cipher::EXPANSION + 9 : 32;
                    size_t padding = wire_target - wire_bytes - 2 * pong_size;
                    while (padding > overhead) {
                        auto msg = NetMsg::Make("unknown");
                        msg.data.resize(std::min(size_t{4'000'000}, padding - overhead));
                        padding -= msg.data.size() + overhead;
                        m_outgoing.push_back(std::move(msg));
                    }
                    BOOST_REQUIRE_EQUAL(padding, 0U);
                }
                m_outgoing.push_back(NetMsg::Make(NetMsgType::PONG, ping_nonce ^ 1));
                auto pong = NetMsg::Make(NetMsgType::PONG, ping_nonce);
                if (malformed_pong) pong.data.pop_back();
                m_outgoing.push_back(std::move(pong));
            }
        }
    }

public:
    PeerSock(bool v2, const CTransaction& tx, FakeSteadyClock& clock)
        : m_v2{v2}, m_tx{tx}, m_clock{clock}
    {
        if (v2)
            m_peer = std::make_unique<V2Transport>(1, false);
        else
            m_peer = std::make_unique<V1Transport>(1);
    }

    mutable std::vector<std::string> commands;
    mutable std::vector<uint8_t> version_bytes;
    mutable std::vector<uint8_t> tx_bytes;
    mutable uint64_t ping_nonce{0};
    mutable size_t wire_bytes{0};
    size_t wire_target{0};
    int32_t peer_version{70016};
    size_t user_agent_size{10};
    int version_fault{0};
    uint64_t services{NODE_NETWORK | NODE_WITNESS};
    bool relay{true};
    bool duplicate_version{false};
    bool omit_relay{false};
    bool verack_first{false};
    bool duplicate_verack{false};
    bool malformed_negotiation{false};
    bool request{true};
    uint32_t request_type{MSG_TX};
    bool wrong_hash{false};
    bool bad_count{false};
    bool duplicate_request{false};
    bool answer_ping{true};
    bool malformed_pong{false};
    size_t flood_messages{0};
    size_t flood_bytes{0};
    size_t max_write{std::numeric_limits<size_t>::max()};
    size_t max_read{std::numeric_limits<size_t>::max()};
    bool coalesce{false};
    bool hold_transport_version{false};
    std::chrono::milliseconds io_delay{0};
    const bool* disclosure{nullptr};
    bool fail_disclosure{false};
    bool throw_after_verack{false};
    bool block_disclosure{false};
    mutable size_t disclosure_writes{0};
    size_t fail_after_writes{0};
    CThreadInterrupt* cancel_on_wait{nullptr};

    ssize_t Send(const void* data, size_t len, int) const override
    {
        if (throw_after_verack && !commands.empty() && commands.back() == NetMsgType::VERACK) throw std::runtime_error{"Untrusted diagnostic"};
        if (disclosure && *disclosure) {
            ++disclosure_writes;
            if (fail_disclosure && disclosure_writes > fail_after_writes) return 0;
            if (block_disclosure) {
                WouldBlock();
                return -1;
            }
        }
        len = std::min(len, max_write);
        std::span<const uint8_t> input{static_cast<const uint8_t*>(data), len};
        while (!input.empty()) {
            BOOST_REQUIRE(m_peer->ReceivedBytes(input));
            if (m_peer->ReceivedMessageComplete()) {
                bool rejected{false};
                auto msg = m_peer->GetReceivedMessage(NodeClock::epoch, rejected);
                BOOST_REQUIRE(!rejected);
                Process(msg);
            }
        }
        m_clock += io_delay;
        return len;
    }

    ssize_t Recv(void* data, size_t len, int) const override
    {
        len = std::min(len, max_read);
        size_t copied{0};
        do {
            if (!m_outgoing.empty() && m_peer->SetMessageToSend(m_outgoing.front())) m_outgoing.pop_front();
            const auto [bytes, more, type] = m_peer->GetBytesToSend(!m_outgoing.empty());
            if (bytes.empty()) break;
            size_t available = bytes.size();
            if (hold_transport_version && type.empty()) {
                if (available <= BIP324Cipher::EXPANSION) break;
                available -= BIP324Cipher::EXPANSION;
            }
            const auto count = std::min(len - copied, available);
            std::memcpy(static_cast<uint8_t*>(data) + copied, bytes.data(), count);
            m_peer->MarkBytesSent(count);
            copied += count;
        } while (coalesce && copied < len);
        if (copied == 0) {
            WouldBlock();
            return -1;
        }
        m_clock += io_delay;
        wire_bytes += copied;
        return copied;
    }

    bool Wait(std::chrono::milliseconds timeout, Event requested, Event* occurred = nullptr) const override
    {
        m_clock += timeout;
        if (cancel_on_wait) (*cancel_on_wait)();
        if (occurred) *occurred = requested;
        return true;
    }

private:
    PeerSock& operator=(Sock&&) override
    {
        assert(false && "Moving into a mock socket is not allowed.");
        return *this;
    }
};

std::unique_ptr<Transport> SenderTransport(bool v2)
{
    if (v2) return std::make_unique<V2Transport>(0, true);
    return std::make_unique<V1Transport>(0);
}

struct ProxyTranscript {
    std::vector<std::string> passwords;
    std::vector<std::string> destinations;
    size_t open{0};
    size_t peak_open{0};
};

class ProxyPeerSock : public PeerSock
{
    ProxyTranscript& m_transcript;
    const CService& m_proxy;
    mutable int m_socks_stage{0};
    mutable std::deque<uint8_t> m_reply;

public:
    ProxyPeerSock(bool v2, const CTransaction& tx, FakeSteadyClock& clock,
                  ProxyTranscript& transcript, const CService& proxy)
        : PeerSock{v2, tx, clock}, m_transcript{transcript}, m_proxy{proxy}
    {
        ++m_transcript.open;
        m_transcript.peak_open = std::max(m_transcript.peak_open, m_transcript.open);
    }

    ~ProxyPeerSock() override { --m_transcript.open; }

    bool fallback{false};
    bool reject_auth{false};
    uint8_t destination_status{0};

    int Connect(const sockaddr* address, socklen_t len) const override
    {
        CService target;
        BOOST_REQUIRE(target.SetSockAddr(address, len));
        BOOST_CHECK(target == m_proxy);
        return 0;
    }

    ssize_t Send(const void* data, size_t len, int flags) const override
    {
        if (m_socks_stage == 3) return fallback ? len : PeerSock::Send(data, len, flags);
        const auto* bytes = static_cast<const uint8_t*>(data);
        if (m_socks_stage == 0) {
            BOOST_REQUIRE_EQUAL(len, 3U);
            BOOST_CHECK_EQUAL(bytes[0], 5U);
            BOOST_CHECK_EQUAL(bytes[1], 1U);
            BOOST_CHECK_EQUAL(bytes[2], 2U);
            m_reply = {5, reject_auth ? uint8_t{0} : uint8_t{2}};
        } else if (m_socks_stage == 1) {
            BOOST_REQUIRE_EQUAL(len, 76U);
            BOOST_CHECK_EQUAL(bytes[0], 1U);
            BOOST_CHECK_EQUAL(bytes[1], 9U);
            BOOST_CHECK(std::string(reinterpret_cast<const char*>(bytes + 2), 9) == "<torS0X>0");
            BOOST_CHECK_EQUAL(bytes[11], 64U);
            m_transcript.passwords.emplace_back(reinterpret_cast<const char*>(bytes + 12), 64);
            m_reply = {1, 0};
        } else {
            BOOST_REQUIRE_GE(len, 7U);
            BOOST_CHECK_EQUAL(bytes[0], 5U);
            BOOST_CHECK_EQUAL(bytes[1], 1U);
            BOOST_CHECK_EQUAL(bytes[2], 0U);
            BOOST_CHECK_EQUAL(bytes[3], 3U); // Domain-name CONNECT, never an IP.
            BOOST_REQUIRE_EQUAL(len, 7U + bytes[4]);
            m_transcript.destinations.emplace_back(reinterpret_cast<const char*>(bytes + 5), bytes[4]);
            const uint16_t port = (uint16_t{bytes[len - 2]} << 8) | bytes[len - 1];
            m_transcript.destinations.back() += ":" + std::to_string(port);
            m_reply = {5, destination_status, 0, 1, 127, 0, 0, 1, 0, 1};
        }
        ++m_socks_stage;
        return len;
    }

    ssize_t Recv(void* data, size_t len, int flags) const override
    {
        if (m_reply.empty()) return fallback ? 0 : PeerSock::Recv(data, len, flags);
        len = std::min(len, m_reply.size());
        auto* bytes = static_cast<uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
            bytes[i] = m_reply.front();
            m_reply.pop_front();
        }
        return len;
    }

private:
    ProxyPeerSock& operator=(Sock&&) override
    {
        assert(false && "Moving into a mock socket is not allowed.");
        return *this;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(send_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(destination_input)
{
    const std::string onion{"pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion"};
    const std::vector<std::string> entries{onion, onion + ":8333", onion + ":8334", onion};
    const auto parsed = txsend::ParseDestinations(entries, 8333);
    BOOST_REQUIRE(parsed);
    BOOST_REQUIRE_EQUAL(parsed->size(), 2U);
    BOOST_CHECK_EQUAL((*parsed)[0].GetPort(), 8333);
    BOOST_CHECK_EQUAL((*parsed)[1].GetPort(), 8334);
    BOOST_CHECK((*parsed)[0].IsTor());
    BOOST_CHECK(!txsend::ParseDestinations({}, 8333));
    for (const auto& bad : std::vector<std::string>{"", "localhost", "127.0.0.1", "[::1]", "http://" + onion,
                                                    "aaaaaaaaaaaaaaaa.onion", std::string(56, 'a') + ".onion",
                                                    onion + ":", onion + ":0", onion + ":65536", onion + ":-1",
                                                    onion + ":+1", onion + ":1:2", onion + "=ignored", onion + ":8333/path"}) {
        const std::vector<std::string> later_bad{onion, bad};
        BOOST_CHECK(!txsend::ParseDestinations(later_bad, 8333));
    }
}

BOOST_AUTO_TEST_CASE(proxy_and_timeout_input)
{
    for (const auto* good : {"127.0.0.1:9050", "127.1.2.3:65535", "[::1]:1"})
        BOOST_CHECK(txsend::ParseProxy(good));
    for (const auto* bad : {"localhost:9050", "example.com:9050", "0.0.0.0:9050", "0.1.2.3:9050", "192.168.1.1:9050",
                            "[::]:9050", "[2001:db8::1]:9050", "::1:9050", "[127.0.0.1]:9050", "[::ffff:127.0.0.1]:9050",
                            "127.0.0.1", "127.0.0.1:0", "127.0.0.1:65536", "[::1]", "[::1]:", "127.0.0.1:+1"}) {
        BOOST_CHECK(!txsend::ParseProxy(bad));
    }
    BOOST_CHECK(txsend::ParseTimeout("1") == 1s);
    BOOST_CHECK(txsend::ParseTimeout("60") == 60s);
    for (const auto* bad : {"", "0", "-1", "+1", "1.0", " 1", "1 ", "18446744073709551615", "9223372036854775807"}) {
        BOOST_CHECK(!txsend::ParseTimeout(bad));
    }
    FakeSteadyClock clock;
    const auto max_seconds = std::chrono::duration_cast<std::chrono::seconds>(MockableSteadyClock::time_point::max() - MockableSteadyClock::now()).count();
    BOOST_CHECK(txsend::ParseTimeout(std::to_string(max_seconds)));
    BOOST_CHECK(!txsend::ParseTimeout(std::to_string(max_seconds + 1)));
    clock += 1s;
    BOOST_CHECK(!txsend::ParseTimeout(std::to_string(max_seconds)));
}

BOOST_AUTO_TEST_CASE(transaction_input)
{
    for (bool witness : {false, true}) {
        const auto tx = TestTransaction(witness);
        const auto hex = HexStr(NetMsg::Make(NetMsgType::TX, TX_WITH_WITNESS(tx)).data);
        for (const auto* ending : {"", "\n", "\r\n"}) {
            std::istringstream input{hex + ending};
            const auto decoded = txsend::ReadTransaction(input);
            BOOST_REQUIRE(decoded);
            BOOST_CHECK(decoded->GetWitnessHash() == tx.GetWitnessHash());
        }
        std::string upper = hex;
        for (auto& c : upper)
            if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
        std::istringstream upper_input{upper};
        BOOST_CHECK(txsend::ReadTransaction(upper_input));
        for (const auto& bad : std::vector<std::string>{"", "0", "zz", hex + "00", hex + hex, hex + "\r",
                                                        hex + "\n\n", hex + " \n", " " + hex, hex + "\n00"}) {
            std::istringstream input{bad};
            BOOST_CHECK(!txsend::ReadTransaction(input));
        }
    }
    for (int scenario = 0; scenario < 4; ++scenario) {
        CMutableTransaction tx{TestTransaction()};
        if (scenario == 0) {
            tx.vin[0].prevout.SetNull();
            tx.vin[0].scriptSig = CScript{} << OP_0 << OP_0;
        }
        if (scenario == 1) tx.vin.push_back(tx.vin[0]);
        if (scenario == 2) tx.vout[0].nValue = -1;
        if (scenario == 3) tx.vout.clear();
        std::istringstream input{HexStr(NetMsg::Make(NetMsgType::TX, TX_WITH_WITNESS(tx)).data)};
        BOOST_CHECK(!txsend::ReadTransaction(input));
    }
    std::istringstream oversized{std::string(2 * MAX_BLOCK_WEIGHT + 3, '0')};
    BOOST_CHECK(!txsend::ReadTransaction(oversized));
    CMutableTransaction heavy{TestTransaction(false)};
    heavy.vout[0].scriptPubKey.resize(MAX_BLOCK_WEIGHT / 4 + 1);
    std::istringstream heavy_input{HexStr(NetMsg::Make(NetMsgType::TX, TX_WITH_WITNESS(heavy)).data)};
    BOOST_CHECK(!txsend::ReadTransaction(heavy_input));
}

BOOST_AUTO_TEST_CASE(handoff)
{
    for (bool v2 : {false, true}) {
        for (bool witness : {false, true}) {
            FakeSteadyClock clock;
            const auto tx = TestTransaction(witness);
            PeerSock sock{v2, tx, clock};
            sock.max_read = 3;
            sock.max_write = 5;
            auto transport = SenderTransport(v2);
            CThreadInterrupt interrupt;
            bool disclosed{false};
            BOOST_CHECK(txsend::RunSession(sock, *transport, tx, MockableSteadyClock::now() + 60s, interrupt, disclosed) == txsend::SessionResult::SUCCESS);
            BOOST_CHECK(disclosed);
            const std::vector<std::string> expected{NetMsgType::VERSION, NetMsgType::VERACK, NetMsgType::INV, NetMsgType::TX, NetMsgType::PING};
            BOOST_CHECK(sock.commands == expected);
            BOOST_CHECK_NE(sock.ping_nonce, 0U);
            BOOST_CHECK(sock.tx_bytes == NetMsg::Make(NetMsgType::TX, TX_WITH_WITNESS(tx)).data);
            DataStream profile{sock.version_bytes};
            int32_t version, height;
            uint64_t services, recv_services, send_services, nonce;
            int64_t timestamp;
            CService receiver, sender;
            std::string user_agent;
            bool relay;
            profile >> version >> services >> timestamp >> recv_services >> CNetAddr::V1(receiver) >> send_services >> CNetAddr::V1(sender) >> nonce >> user_agent >> height >> relay;
            BOOST_CHECK_EQUAL(version, 70016);
            BOOST_CHECK_EQUAL(services | recv_services | send_services, 0U);
            BOOST_CHECK_EQUAL(timestamp, 0);
            BOOST_CHECK(receiver == CService{});
            BOOST_CHECK(sender == CService{});
            BOOST_CHECK_NE(nonce, 0U);
            BOOST_CHECK_EQUAL(user_agent, "/pynode:0.0.1/");
            BOOST_CHECK_EQUAL(height, 0);
            BOOST_CHECK(!relay);
            BOOST_CHECK(profile.empty());
        }
    }
}

BOOST_AUTO_TEST_CASE(transport_before_profile)
{
    FakeSteadyClock clock;
    const auto tx = TestTransaction();
    PeerSock sock{true, tx, clock};
    sock.hold_transport_version = true;
    auto transport = SenderTransport(true);
    CThreadInterrupt interrupt;
    bool disclosed{false};
    BOOST_CHECK(txsend::RunSession(sock, *transport, tx, MockableSteadyClock::now() + 1s, interrupt, disclosed) == txsend::SessionResult::PEER_ERROR);
    BOOST_CHECK(sock.commands.empty());
    BOOST_CHECK(!disclosed);
    BOOST_CHECK(!transport->ShouldReconnectV1());
}

BOOST_AUTO_TEST_CASE(ineligible_peer)
{
    for (bool v2 : {false, true}) {
        for (int scenario = 0; scenario < 8; ++scenario) {
            FakeSteadyClock clock;
            const auto tx = TestTransaction();
            PeerSock sock{v2, tx, clock};
            if (scenario == 0) sock.peer_version = 70015;
            if (scenario == 1) sock.services = NODE_NETWORK;
            if (scenario == 2) sock.services = NODE_WITNESS;
            if (scenario == 3) sock.relay = false;
            if (scenario == 4) sock.duplicate_version = true;
            if (scenario == 5) sock.verack_first = true;
            if (scenario == 6) {
                sock.duplicate_verack = true;
                sock.coalesce = true;
            }
            if (scenario == 7) sock.malformed_negotiation = true;
            auto transport = SenderTransport(v2);
            CThreadInterrupt interrupt;
            bool disclosed{false};
            BOOST_CHECK(txsend::RunSession(sock, *transport, tx, MockableSteadyClock::now() + 1s, interrupt, disclosed) == txsend::SessionResult::PEER_ERROR);
            BOOST_CHECK(!disclosed);
        }
    }
}

BOOST_AUTO_TEST_CASE(optional_relay_and_early_verack)
{
    for (bool v2 : {false, true}) {
        FakeSteadyClock clock;
        const auto tx = TestTransaction();
        PeerSock sock{v2, tx, clock};
        sock.omit_relay = true;
        sock.services = NODE_NETWORK_LIMITED | NODE_WITNESS;
        sock.max_write = 1; // The remote verack arrives before our verack flushes.
        sock.coalesce = true;
        auto transport = SenderTransport(v2);
        CThreadInterrupt interrupt;
        bool disclosed{false};
        BOOST_CHECK(txsend::RunSession(sock, *transport, tx, MockableSteadyClock::now() + 1s, interrupt, disclosed) == txsend::SessionResult::SUCCESS);
        BOOST_CHECK(disclosed);
    }
}

BOOST_AUTO_TEST_CASE(version_validation)
{
    for (bool v2 : {false, true}) {
        for (int scenario = 0; scenario < 6; ++scenario) {
            FakeSteadyClock clock;
            const auto tx = TestTransaction();
            PeerSock sock{v2, tx, clock};
            if (scenario < 4)
                sock.version_fault = scenario + 1;
            else
                sock.user_agent_size = scenario == 4 ? 256 : 257;
            auto transport = SenderTransport(v2);
            CThreadInterrupt interrupt;
            bool disclosed{false};
            const auto result = txsend::RunSession(sock, *transport, tx, MockableSteadyClock::now() + 1s, interrupt, disclosed);
            BOOST_CHECK(result == (scenario == 4 ? txsend::SessionResult::SUCCESS : txsend::SessionResult::PEER_ERROR));
            BOOST_CHECK_EQUAL(disclosed, scenario == 4);
        }
    }
}

BOOST_AUTO_TEST_CASE(failed_exchange)
{
    for (bool v2 : {false, true}) {
        for (int scenario = 0; scenario < 8; ++scenario) {
            FakeSteadyClock clock;
            const auto tx = TestTransaction();
            PeerSock sock{v2, tx, clock};
            if (scenario == 0) sock.request = false;
            if (scenario == 1) sock.wrong_hash = true;
            if (scenario == 2) sock.request_type = MSG_WITNESS_TX;
            if (scenario == 3) sock.bad_count = true;
            if (scenario == 4) sock.duplicate_request = true;
            if (scenario == 5) sock.answer_ping = false;
            if (scenario == 6) sock.malformed_pong = true;
            if (scenario == 7) sock.request_type = MSG_WTX;
            auto transport = SenderTransport(v2);
            CThreadInterrupt interrupt;
            bool disclosed{false};
            BOOST_CHECK(txsend::RunSession(sock, *transport, tx, MockableSteadyClock::now() + 1s, interrupt, disclosed) == txsend::SessionResult::PEER_ERROR);
            BOOST_CHECK(disclosed);
            BOOST_CHECK_LE(std::count(sock.commands.begin(), sock.commands.end(), NetMsgType::INV), 1);
            BOOST_CHECK_LE(std::count(sock.commands.begin(), sock.commands.end(), NetMsgType::TX), 1);
        }
    }
}

BOOST_AUTO_TEST_CASE(disclosure_write)
{
    for (bool v2 : {false, true}) {
        for (size_t partial : {0U, 1U, 25U}) {
            FakeSteadyClock clock;
            const auto tx = TestTransaction();
            PeerSock sock{v2, tx, clock};
            sock.max_write = 1;
            sock.fail_disclosure = true;
            sock.fail_after_writes = partial;
            auto transport = SenderTransport(v2);
            CThreadInterrupt interrupt;
            bool disclosed{false};
            sock.disclosure = &disclosed;
            BOOST_CHECK(txsend::RunSession(sock, *transport, tx, MockableSteadyClock::now() + 1s, interrupt, disclosed) == txsend::SessionResult::PEER_ERROR);
            BOOST_CHECK(disclosed);
            BOOST_CHECK_EQUAL(sock.disclosure_writes, partial + 1);
            BOOST_CHECK(sock.tx_bytes.empty());
        }
    }
}

BOOST_AUTO_TEST_CASE(deadline_and_cancellation)
{
    for (bool v2 : {false, true}) {
        for (int scenario = 0; scenario < 4; ++scenario) {
            FakeSteadyClock clock;
            const auto tx = TestTransaction();
            PeerSock sock{v2, tx, clock};
            auto transport = SenderTransport(v2);
            CThreadInterrupt interrupt;
            bool disclosed{false};
            if (scenario == 0) interrupt();
            if (scenario == 1) {
                sock.request = false;
                sock.cancel_on_wait = &interrupt;
            }
            if (scenario == 2) {
                sock.max_read = 1;
                sock.io_delay = 10ms;
            }
            if (scenario == 3) {
                sock.disclosure = &disclosed;
                sock.block_disclosure = true;
            }
            const auto deadline = MockableSteadyClock::now() + 100ms;
            const auto result = txsend::RunSession(sock, *transport, tx, deadline, interrupt, disclosed);
            BOOST_CHECK(result == (scenario < 2 ? txsend::SessionResult::INTERRUPTED : txsend::SessionResult::PEER_ERROR));
            if (scenario == 0 || scenario == 2) BOOST_CHECK(!disclosed);
            if (scenario == 1 || scenario == 3) BOOST_CHECK(disclosed);
            if (scenario >= 2) BOOST_CHECK(MockableSteadyClock::now() >= deadline);
        }
    }
}

BOOST_AUTO_TEST_CASE(resource_limits)
{
    for (bool v2 : {false, true}) {
        for (bool bytes : {false, true}) {
            FakeSteadyClock clock;
            const auto tx = TestTransaction();
            PeerSock sock{v2, tx, clock};
            sock.request = false;
            if (bytes)
                sock.flood_bytes = 3;
            else
                sock.flood_messages = 1024;
            auto transport = SenderTransport(v2);
            CThreadInterrupt interrupt;
            bool disclosed{false};
            const auto start = MockableSteadyClock::now();
            BOOST_CHECK(txsend::RunSession(sock, *transport, tx, MockableSteadyClock::now() + 60s, interrupt, disclosed) == txsend::SessionResult::PEER_ERROR);
            // No mocked time passes while processing a busy peer.
            BOOST_CHECK(MockableSteadyClock::now() == start);
        }
    }
}

BOOST_AUTO_TEST_CASE(message_limit_boundary)
{
    for (bool v2 : {false, true}) {
        for (size_t padding : {1016U, 1017U}) {
            FakeSteadyClock clock;
            const auto tx = TestTransaction();
            PeerSock sock{v2, tx, clock};
            // Eight useful messages plus padding. The matching pong is last.
            sock.flood_messages = padding;
            auto transport = SenderTransport(v2);
            CThreadInterrupt interrupt;
            bool disclosed{false};
            const auto result = txsend::RunSession(sock, *transport, tx, MockableSteadyClock::now() + 1s, interrupt, disclosed);
            BOOST_CHECK(result == (padding == 1016 ? txsend::SessionResult::SUCCESS : txsend::SessionResult::PEER_ERROR));
            BOOST_CHECK(disclosed);
        }
    }
}

BOOST_AUTO_TEST_CASE(wire_limit_boundary)
{
    for (bool v2 : {false, true}) {
        for (size_t target : {8U * 1024U * 1024U, 8U * 1024U * 1024U + 1U}) {
            FakeSteadyClock clock;
            const auto tx = TestTransaction();
            PeerSock sock{v2, tx, clock};
            sock.wire_target = target;
            auto transport = SenderTransport(v2);
            CThreadInterrupt interrupt;
            bool disclosed{false};
            const auto result = txsend::RunSession(sock, *transport, tx, MockableSteadyClock::now() + 1s, interrupt, disclosed);
            BOOST_CHECK(result == (target == 8U * 1024U * 1024U ? txsend::SessionResult::SUCCESS : txsend::SessionResult::PEER_ERROR));
            BOOST_CHECK_EQUAL(sock.wire_bytes, 8U * 1024U * 1024U);
            BOOST_CHECK(disclosed);
        }
    }
}

BOOST_AUTO_TEST_CASE(attempt_policy)
{
    struct RestoreFactory {
        decltype(CreateSock) saved{CreateSock};
        ~RestoreFactory() { CreateSock = saved; }
    } restore;
    CNetAddr onion;
    BOOST_REQUIRE(onion.SetSpecial("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion"));
    const std::vector<CService> destinations{
        CService{onion, 8333},
        CService{onion, 8334},
        CService{onion, 8335},
    };
    const CService proxy = LookupNumeric("127.0.0.1:9050");
    for (int scenario = 0; scenario < 9; ++scenario) {
        FakeSteadyClock clock;
        const auto tx = TestTransaction();
        ProxyTranscript transcript;
        size_t attempts{0};
        CThreadInterrupt interrupt;
        CreateSock = [&](int domain, int type, int protocol) -> std::unique_ptr<Sock> {
            BOOST_CHECK_EQUAL(domain, AF_INET);
            BOOST_CHECK_EQUAL(type, SOCK_STREAM);
            BOOST_CHECK_EQUAL(protocol, IPPROTO_TCP);
            const size_t attempt = attempts++;
            if (scenario == 8) throw std::runtime_error{"Local failure"};
            if (scenario == 6) return {};
            auto sock = std::make_unique<ProxyPeerSock>(!(scenario == 1 && attempt == 1), tx, clock, transcript, proxy);
            if (scenario == 0 && attempt == 0) sock->services = NODE_NONE;
            if (scenario == 1 && attempt == 0) sock->fallback = true;
            if (scenario == 2) sock->request = false;
            if (scenario == 3) sock->reject_auth = true;
            if (scenario == 4 && attempt == 0) sock->destination_status = 4;
            if (scenario == 5) {
                sock->request = false;
                sock->cancel_on_wait = &interrupt;
            }
            if (scenario == 7) sock->throw_after_verack = true;
            return sock;
        };
        const auto result = txsend::SendTransaction(tx, destinations, proxy, 1s, interrupt);
        const auto expected = scenario == 2                  ? txsend::SendResult::DISCLOSED :
                              scenario == 3 || scenario == 6 ? txsend::SendResult::PROXY_ERROR :
                              scenario == 5 || scenario == 7 ? txsend::SendResult::DISCLOSED :
                              scenario == 8                  ? txsend::SendResult::LOCAL_ERROR :
                                                               txsend::SendResult::SUCCESS;
        BOOST_CHECK(result == expected);
        BOOST_CHECK_EQUAL(attempts, scenario == 0 || scenario == 1 || scenario == 4 ? 2U : 1U);
        BOOST_CHECK_EQUAL(transcript.open, 0U);
        if (scenario != 6 && scenario != 8) BOOST_CHECK_EQUAL(transcript.peak_open, 1U);
        if (transcript.passwords.size() == 2) BOOST_CHECK(transcript.passwords[0] != transcript.passwords[1]);
        if (scenario == 1) BOOST_CHECK(transcript.destinations[0] == transcript.destinations[1]);
        if (scenario == 0 || scenario == 4) BOOST_CHECK(transcript.destinations[0] != transcript.destinations[1]);
    }
}

BOOST_AUTO_TEST_SUITE_END()
