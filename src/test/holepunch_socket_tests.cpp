// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <addrman.h>
#include <bip324.h>
#include <chainparams.h>
#include <compat/compat.h>
#include <holepunch_manager.h>
#include <net.h>
#include <netbase.h>
#include <netgroup.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace {
enum class Failure { NONE, CREATE, REUSEADDR, REUSEPORT, BIND, NONBLOCK, CONNECT };

struct SocketState {
    Failure failure{Failure::NONE};
    int created{0};
    int destroyed{0};
    int connects{0};
    bool nonblocking{false};
    bool reuseaddr{false};
    bool reuseport{false};
    std::vector<CService> bindings;
    CService remote;
    Sock::Event events{0};
    int error{0};
    bool wait_success{true};
    bool getsockopt_success{true};
};

class PunchSock : public ZeroSock
{
    SocketState& m_state;

    PunchSock& operator=(Sock&&) override
    {
        assert(false && "Move of Sock into PunchSock not allowed.");
        return *this;
    }

public:
    explicit PunchSock(SocketState& state) : m_state{state} {}
    ~PunchSock() override { ++m_state.destroyed; }

    int SetSockOpt(int level, int name, const void* value, socklen_t size) const override
    {
        BOOST_CHECK_EQUAL(level, SOL_SOCKET);
        BOOST_REQUIRE_EQUAL(size, sizeof(int));
        BOOST_CHECK_EQUAL(*static_cast<const int*>(value), 1);
        BOOST_CHECK(m_state.bindings.empty());
        if (name == SO_REUSEADDR) {
            m_state.reuseaddr = true;
            if (m_state.failure == Failure::REUSEADDR) return SOCKET_ERROR;
        }
#ifdef SO_REUSEPORT
        if (name == SO_REUSEPORT) {
            m_state.reuseport = true;
            if (m_state.failure == Failure::REUSEPORT) return SOCKET_ERROR;
        }
#endif
        return 0;
    }

    int Bind(const sockaddr* address, socklen_t length) const override
    {
        CService binding;
        BOOST_REQUIRE(binding.SetSockAddr(address, length));
        m_state.bindings.push_back(binding);
        return m_state.failure == Failure::BIND ? SOCKET_ERROR : 0;
    }

    bool SetNonBlocking() const override
    {
        m_state.nonblocking = m_state.failure != Failure::NONBLOCK;
        return m_state.nonblocking;
    }

    int Connect(const sockaddr* address, socklen_t length) const override
    {
        ++m_state.connects;
        BOOST_CHECK(m_state.nonblocking);
        BOOST_REQUIRE(m_state.remote.SetSockAddr(address, length));
        const int error{m_state.failure == Failure::CONNECT ? WSAEADDRINUSE : WSAEINPROGRESS};
#ifdef WIN32
        WSASetLastError(error);
#else
        errno = error;
#endif
        return SOCKET_ERROR;
    }

    bool Wait(std::chrono::milliseconds timeout, Event requested, Event* occurred) const override
    {
        BOOST_CHECK(timeout == 0ms);
        BOOST_CHECK_EQUAL(requested, SendEvent | RecvEvent);
        *occurred = m_state.events;
        return m_state.wait_success;
    }

    int GetSockOpt(int level, int name, void* value, socklen_t* size) const override
    {
        BOOST_CHECK_EQUAL(level, SOL_SOCKET);
        BOOST_CHECK_EQUAL(name, SO_ERROR);
        BOOST_REQUIRE_EQUAL(*size, sizeof(int));
        *static_cast<int*>(value) = m_state.error;
        return m_state.getsockopt_success ? 0 : SOCKET_ERROR;
    }
};

struct PunchSocketSetup : BasicTestingSetup {
    const decltype(CreateSock) m_saved_factory{CreateSock};
    SocketState m_state;
    const CService m_binding{LookupNumeric("127.0.0.1", 18444)};
    const CService m_remote{LookupNumeric("127.0.0.1", 18445)};

    PunchSocketSetup()
    {
        CreateSock = [this](int domain, int type, int protocol) -> std::unique_ptr<Sock> {
            BOOST_CHECK_EQUAL(domain, AF_INET);
            BOOST_CHECK_EQUAL(type, SOCK_STREAM);
            BOOST_CHECK_EQUAL(protocol, IPPROTO_TCP);
            ++m_state.created;
            if (m_state.failure == Failure::CREATE) return {};
            return std::make_unique<PunchSock>(m_state);
        };
    }

    ~PunchSocketSetup() { CreateSock = m_saved_factory; }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(holepunch_socket_tests, PunchSocketSetup)

BOOST_AUTO_TEST_CASE(fixed_binding_and_pending_connect)
{
    auto socket{ConnmanTestMsg::StartPunchConnect(m_binding, m_remote)};
    BOOST_REQUIRE(socket);
    BOOST_CHECK(m_state.reuseaddr);
#ifdef SO_REUSEPORT
    BOOST_CHECK(m_state.reuseport);
#endif
    BOOST_REQUIRE_EQUAL(m_state.bindings.size(), 1);
    BOOST_CHECK(m_state.bindings.front() == m_binding);
    BOOST_CHECK(m_state.remote == m_remote);
    // Polling a connect in progress must neither wait nor replace its socket.
    for (int i{0}; i < 10; ++i) BOOST_CHECK(!ConnmanTestMsg::PunchConnectReady(*socket).has_value());
    BOOST_CHECK_EQUAL(m_state.created, 1);
    BOOST_CHECK_EQUAL(m_state.connects, 1);
    BOOST_CHECK_EQUAL(m_state.destroyed, 0);
    m_state.events = Sock::SendEvent;
    BOOST_CHECK(ConnmanTestMsg::PunchConnectReady(*socket) == true);
    m_state.error = WSAEADDRINUSE;
    BOOST_CHECK(ConnmanTestMsg::PunchConnectReady(*socket) == false);
    m_state.error = 0;
    m_state.getsockopt_success = false;
    BOOST_CHECK(ConnmanTestMsg::PunchConnectReady(*socket) == false);
    m_state.wait_success = false;
    BOOST_CHECK(ConnmanTestMsg::PunchConnectReady(*socket) == false);
}

BOOST_AUTO_TEST_CASE(socket_failures_never_substitute_an_ephemeral_port)
{
    for (const auto failure : {Failure::CREATE, Failure::REUSEADDR,
#ifdef SO_REUSEPORT
                              Failure::REUSEPORT,
#endif
                              Failure::BIND, Failure::NONBLOCK, Failure::CONNECT}) {
        m_state = {};
        m_state.failure = failure;
        BOOST_CHECK(!ConnmanTestMsg::StartPunchConnect(m_binding, m_remote));
        BOOST_CHECK_EQUAL(m_state.created, 1);
        BOOST_CHECK_EQUAL(m_state.destroyed, failure == Failure::CREATE ? 0 : 1);
        BOOST_CHECK_EQUAL(m_state.connects, failure == Failure::CONNECT ? 1 : 0);
        BOOST_CHECK(m_state.bindings.size() <= 1);
        for (const auto& binding : m_state.bindings) BOOST_CHECK(binding == m_binding);
    }
}

BOOST_AUTO_TEST_CASE(strict_v2_transport)
{
    std::array<uint8_t, 16> v1_prefix{0, 0, 0, 0, 'v', 'e', 'r', 's', 'i', 'o', 'n', 0, 0, 0, 0, 0};
    std::ranges::copy(Params().MessageStart(), v1_prefix.begin());
    for (const bool initiating : {false, true}) {
        V2Transport transport{0, initiating, /*allow_v1_fallback=*/false};
        const auto bytes{std::get<0>(transport.GetBytesToSend(false))};
        BOOST_CHECK_EQUAL(bytes.empty(), !initiating);
        transport.MarkBytesSent(bytes.size());
        BOOST_CHECK(!transport.ShouldReconnectV1());
        // A responder rejects a v1 version immediately. An initiator treats it as
        // an invalid v2 handshake and rejects it without ever accepting v1 data.
        std::vector<uint8_t> input(initiating ? EllSwiftPubKey::size() + V2Transport::MAX_GARBAGE_LEN + BIP324Cipher::GARBAGE_TERMINATOR_LEN : v1_prefix.size());
        std::ranges::copy(v1_prefix, input.begin());
        std::span<const uint8_t> incoming{input};
        BOOST_CHECK(!transport.ReceivedBytes(incoming));
        BOOST_CHECK(!transport.ReceivedMessageComplete());
        BOOST_CHECK(transport.GetInfo().transport_type != TransportProtocolType::V1);
        BOOST_CHECK(!transport.ShouldReconnectV1());
    }
    // Ordinary v2 connections retain their existing fallback behavior.
    V2Transport responder{0, /*initiating=*/false};
    std::span<const uint8_t> incoming{v1_prefix};
    BOOST_CHECK(responder.ReceivedBytes(incoming));
    BOOST_CHECK(responder.GetInfo().transport_type == TransportProtocolType::V1);
    V2Transport initiator{0, /*initiating=*/true};
    initiator.MarkBytesSent(CMessageHeader::HEADER_SIZE);
    BOOST_CHECK(initiator.ShouldReconnectV1());
}

BOOST_AUTO_TEST_CASE(pinned_transport_role)
{
    for (const bool initiating : {false, true}) {
        auto attempt{std::make_shared<holepunch::Attempt>()};
        attempt->pair.initiating = initiating;
        CNode node{0, nullptr, CAddress{m_remote, NODE_P2P_V2}, 0, 0, m_binding, "",
                   ConnectionType::HOLEPUNCH, false, 0,
                   CNodeOptions{.use_v2transport = true, .holepunch_attempt = attempt}};
        BOOST_CHECK_EQUAL(node.IsTransportInitiator(), initiating);
        const auto bytes{std::get<0>(node.m_transport->GetBytesToSend(false))};
        BOOST_CHECK_EQUAL(bytes.empty(), !initiating);
        node.m_transport->MarkBytesSent(bytes.size());
        BOOST_CHECK(!node.m_transport->ShouldReconnectV1());
    }
}

BOOST_AUTO_TEST_CASE(auxiliary_connections_do_not_replace_outbounds)
{
    const auto groups{NetGroupManager::NoAsmap()};
    AddrMan addresses{groups, /*deterministic=*/true, /*consistency_check_ratio=*/0};
    ConnmanTestMsg connman{0, 0, addresses, groups, Params()};
    const std::string name{m_remote.ToStringAddrPort()};
    BOOST_REQUIRE(connman.AddNode({name, false}));
    for (const auto type : {ConnectionType::HOLEPUNCH, ConnectionType::HOLEPUNCH_OBSERVER}) {
        CNodeOptions options;
        options.use_v2transport = type == ConnectionType::HOLEPUNCH;
        if (options.use_v2transport) options.holepunch_attempt = std::make_shared<holepunch::Attempt>();
        auto* node = new CNode{0, nullptr, CAddress{m_remote, NODE_NONE}, 0, 0, m_binding, name,
                               type, false, 0, std::move(options)};
        connman.AddTestNode(*node);
        BOOST_CHECK(!node->IsFullOutboundConn());
        BOOST_CHECK(!node->IsManualOrFullOutboundConn());
        BOOST_CHECK(!node->IsOutboundOrBlockRelayConn());
        BOOST_CHECK(!connman.AlreadyConnectedToAddressPublic(m_remote));
        BOOST_CHECK(!connman.AlreadyConnectedToAddressPort(m_remote));
        BOOST_CHECK(!connman.AlreadyConnectedToHost(name));
        const auto added{connman.GetAddedNodeInfo(/*include_connected=*/false)};
        connman.ClearTestNodes();
        BOOST_REQUIRE_EQUAL(added.size(), 1);
        BOOST_CHECK(!added.front().fConnected);
    }
    auto* node = new CNode{0, nullptr, CAddress{m_remote, NODE_NONE}, 0, 0, m_binding, name,
                           ConnectionType::OUTBOUND_FULL_RELAY, false, 0};
    connman.AddTestNode(*node);
    BOOST_CHECK(connman.AlreadyConnectedToAddressPublic(m_remote));
    BOOST_CHECK(connman.AlreadyConnectedToAddressPort(m_remote));
    BOOST_CHECK(connman.AlreadyConnectedToHost(name));
    BOOST_CHECK(connman.GetAddedNodeInfo(/*include_connected=*/false).empty());
    connman.ClearTestNodes();
}

BOOST_AUTO_TEST_SUITE_END()
