// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <holepunch.h>
#include <holepunch_manager.h>
#include <net.h>
#include <net_processing.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <node/protocol_version.h>
#include <protocol.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <test/util/time.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
struct HolePunchPeerSetup : TestingSetup {
    FakeNodeClock m_clock{3600000s};
    FakeSteadyClock m_steady;
    ConnmanTestMsg& m_connman{static_cast<ConnmanTestMsg&>(*m_node.connman)};
    PeerManager& m_peerman{*m_node.peerman};
    std::unique_ptr<CNode> m_peer;
    std::unique_ptr<V2Transport> m_remote_transport;
    std::shared_ptr<const holepunch::Attempt> m_attempt;
    std::optional<holepunch::Pair> m_remote_pair;

    ~HolePunchPeerSetup()
    {
        if (m_peer) m_peerman.FinalizeNode(*m_peer);
    }

    void Advance(std::chrono::seconds delta)
    {
        m_clock += delta;
        m_steady += delta;
    }

    holepunch::Manager::Actions Poll()
    {
        return m_connman.GetHolePunchManager()->Poll(GetTime<std::chrono::milliseconds>().count(), holepunch::Clock::now(), true);
    }

    /** Move actual encrypted bytes from the remote transport into the node. */
    void ReceiveBytes()
    {
        while (true) {
            auto [bytes, more, type] = m_remote_transport->GetBytesToSend(false);
            if (bytes.empty()) break;
            bool complete{false};
            m_connman.NodeReceiveMsgBytes(*m_peer, bytes, complete);
            m_remote_transport->MarkBytesSent(bytes.size());
        }
    }

    /** Drain queued node messages through the remote BIP324 decoder. */
    std::vector<CNetMessage> SentMessages()
    {
        std::vector<CNetMessage> messages;
        LOCK(m_peer->cs_vSend);
        while (true) {
            if (!m_peer->vSendMsg.empty()) {
                const size_t memory{m_peer->vSendMsg.front().GetMemoryUsage()};
                if (m_peer->m_transport->SetMessageToSend(m_peer->vSendMsg.front())) {
                    m_peer->m_send_memusage -= memory;
                    m_peer->vSendMsg.pop_front();
                }
            }
            auto [bytes, more, type] = m_peer->m_transport->GetBytesToSend(false);
            if (bytes.empty()) break;
            const size_t size{bytes.size()};
            while (!bytes.empty()) {
                BOOST_REQUIRE(m_remote_transport->ReceivedBytes(bytes));
                if (m_remote_transport->ReceivedMessageComplete()) {
                    bool reject{false};
                    messages.push_back(m_remote_transport->GetReceivedMessage(NodeClock::now(), reject));
                    BOOST_REQUIRE(!reject);
                }
            }
            m_peer->m_transport->MarkBytesSent(size);
        }
        return messages;
    }

    void Receive(CSerializedNetMsg message) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
    {
        BOOST_REQUIRE(m_remote_transport->SetMessageToSend(message));
        ReceiveBytes();
        m_peer->fPauseSend = false;
        m_connman.ProcessMessagesOnce(*m_peer);
    }

    void Start(bool initiating, uint8_t format = 0) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
    {
        m_connman.SetHolePunchManager(std::make_unique<holepunch::Manager>(Params().GenesisBlock().GetHash(), true, std::nullopt, format));
        auto& manager{*m_connman.GetHolePunchManager()};
        const auto& profile{holepunch::GetProfile(format)};
        Advance(std::chrono::seconds{profile.epoch_seconds + 5}); // Startup quarantine and guard interval.
        std::optional<holepunch::Descriptor> local;
        // Opposite compressed-key prefixes give either role without a biased key search.
        for (uint16_t port = 8333; port < 8461; ++port) {
            const CService endpoint{LookupNumeric("8.8.8.8", port)};
            for (int id = 1; id <= 2; ++id) {
                manager.RemoveNode(id);
                manager.AddObserver(id, {static_cast<unsigned char>(id)});
                manager.ReportObserver(id, endpoint);
                manager.ObserverHandshakeComplete(id);
            }
            local = manager.LocalDescriptor();
            BOOST_REQUIRE(local);
            if (local->pubkey[0] == (initiating ? 2 : 3)) break;
        }
        BOOST_REQUIRE_EQUAL(local->pubkey[0], initiating ? 2 : 3);
        Poll();
        const uint64_t round{GetTime() / profile.round_seconds + 1};
        std::optional<holepunch::RoundSchedule> schedule;
        // Select an eligible remote descriptor with the requested local transport role.
        for (uint32_t i = 1; i <= 10000; ++i) {
            std::array<unsigned char, 32> secret{};
            secret[30] = i >> 8;
            secret[31] = i;
            CKey key;
            key.Set(secret.begin(), secret.end(), true);
            if (key.GetPubKey()[0] == local->pubkey[0]) continue;
            holepunch::Descriptor remote{LookupNumeric("9.9.9.9", 8333), local->epoch, key.GetPubKey(), format};
            auto pair{holepunch::DerivePair(key, remote, *local, Params().GenesisBlock().GetHash())};
            BOOST_REQUIRE(pair);
            if (pair->initiating == initiating) continue;
            auto candidate{holepunch::Schedule(*pair, round)};
            BOOST_REQUIRE(candidate);
            if (!candidate->eligible) continue;
            BOOST_REQUIRE(manager.AddDescriptor(remote));
            m_remote_pair = std::move(pair);
            schedule = candidate;
            break;
        }
        BOOST_REQUIRE(schedule);
        Advance(std::chrono::seconds{round * profile.round_seconds - GetTime()});
        BOOST_CHECK(Poll().start.empty());
        Advance(std::chrono::seconds{(schedule->start_ms % (profile.round_seconds * 1000) + 999) / 1000});
        auto actions{Poll()};
        BOOST_REQUIRE_EQUAL(actions.start.size(), 1U);
        m_attempt = actions.start.front();
        BOOST_REQUIRE_EQUAL(m_attempt->pair.initiating, initiating);
        m_peer = std::make_unique<CNode>(10, nullptr, CAddress{m_attempt->pair.remote.endpoint, ServiceFlags{NODE_NETWORK | NODE_WITNESS}},
                                       0, 0, local->endpoint, "", ConnectionType::HOLEPUNCH, false, 0,
                                       CNodeOptions{.use_v2transport = true, .holepunch_attempt = m_attempt});
        BOOST_REQUIRE(manager.AttachNode(m_attempt, m_peer->GetId()));
        m_remote_transport = std::make_unique<V2Transport>(11, !initiating, false);
        m_peerman.InitializeNode(*m_peer, ServiceFlags{NODE_NETWORK | NODE_WITNESS});
        // The transport handshake itself must not produce application messages.
        for (int i = 0; i < 4; ++i) {
            BOOST_CHECK(SentMessages().empty());
            ReceiveBytes();
        }
        BOOST_REQUIRE(m_peer->m_transport->GetInfo().session_id);
        BOOST_REQUIRE(m_remote_transport->GetInfo().session_id);
        BOOST_CHECK(m_peer->m_transport->GetInfo().session_id == m_remote_transport->GetInfo().session_id);
        m_peerman.SendMessages(*m_peer);
        const auto initial{SentMessages()};
        if (initiating) {
            BOOST_REQUIRE_EQUAL(initial.size(), 1U);
            BOOST_CHECK_EQUAL(initial.front().m_type, NetMsgType::VERSION);
        } else {
            BOOST_CHECK(initial.empty()); // A responder waits for the remote version.
        }
    }

    void Version(int version = PROTOCOL_VERSION) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
    {
        Receive(NetMsg::Make(NetMsgType::VERSION, version,
                            Using<CustomUintFormatter<8>>(ServiceFlags{NODE_NETWORK | NODE_WITNESS}), int64_t{},
                            int64_t{}, CNetAddr::V1(CService{}), int64_t{}, CNetAddr::V1(CService{}),
                            uint64_t{1}, std::string{}, int32_t{}, true));
    }

    std::vector<unsigned char> Proof()
    {
        const auto proof{holepunch::MakeProof(*m_remote_pair, m_attempt->round, *m_remote_transport->GetInfo().session_id)};
        return {proof.begin(), proof.end()};
    }

    void ReceiveProof(const std::vector<unsigned char>& proof) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
    {
        Receive(NetMsg::Make(NetMsgType::FEATURE, std::string{holepunch::GetProfile(m_attempt->pair.local.format).feature_id}, proof));
    }

    void CheckSuccess(bool initiating, uint8_t format = 0) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
    {
        Start(initiating, format);
        Version();
        BOOST_REQUIRE(!m_peer->fDisconnect);
        auto sent{SentMessages()};
        if (!initiating) {
            BOOST_REQUIRE(!sent.empty());
            BOOST_CHECK_EQUAL(sent.front().m_type, NetMsgType::VERSION);
        }
        auto proof_msg{std::find_if(sent.begin(), sent.end(), [](const auto& msg) { return msg.m_type == NetMsgType::FEATURE; })};
        const auto verack{std::find_if(sent.begin(), sent.end(), [](const auto& msg) { return msg.m_type == NetMsgType::VERACK; })};
        BOOST_REQUIRE(proof_msg != sent.end());
        BOOST_REQUIRE(verack != sent.end());
        BOOST_CHECK(proof_msg < verack);
        std::string feature;
        std::vector<unsigned char> proof;
        proof_msg->m_recv >> feature >> proof;
        BOOST_CHECK_EQUAL(feature, holepunch::GetProfile(format).feature_id);
        BOOST_CHECK(holepunch::CheckProof(proof, *m_remote_pair, m_attempt->round, *m_remote_transport->GetInfo().session_id));
        ReceiveProof(Proof());
        BOOST_CHECK(!m_peer->fDisconnect);
        BOOST_CHECK(!m_peer->fSuccessfullyConnected);
        Receive(NetMsg::Make(NetMsgType::VERACK));
        BOOST_CHECK(!m_peer->fDisconnect);
        BOOST_CHECK(m_peer->fSuccessfullyConnected);
        BOOST_CHECK(!m_peer->m_holepunch_attempt);
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(holepunch_peer_tests, HolePunchPeerSetup)

BOOST_AUTO_TEST_CASE(initiator_handshake)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    CheckSuccess(true);
}

BOOST_AUTO_TEST_CASE(responder_handshake)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    CheckSuccess(false);
}

BOOST_AUTO_TEST_CASE(fast_initiator_handshake)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    CheckSuccess(true, holepunch::FAST_FORMAT);
}

BOOST_AUTO_TEST_CASE(fast_responder_handshake)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    CheckSuccess(false, holepunch::FAST_FORMAT);
}

BOOST_AUTO_TEST_CASE(default_rejects_fast_feature)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    Start(true);
    Version();
    Receive(NetMsg::Make(NetMsgType::FEATURE, std::string{holepunch::GetProfile(holepunch::FAST_FORMAT).feature_id}, Proof()));
    BOOST_REQUIRE(!m_peer->fDisconnect); // An unknown feature cannot authenticate this attempt.
    Receive(NetMsg::Make(NetMsgType::VERACK));
    BOOST_CHECK(m_peer->fDisconnect);
    BOOST_CHECK(!m_peer->fSuccessfullyConnected);
}

BOOST_AUTO_TEST_CASE(fast_rejects_default_feature)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    Start(false, holepunch::FAST_FORMAT);
    Version();
    Receive(NetMsg::Make(NetMsgType::FEATURE, std::string{holepunch::GetProfile(0).feature_id}, Proof()));
    BOOST_REQUIRE(!m_peer->fDisconnect);
    Receive(NetMsg::Make(NetMsgType::VERACK));
    BOOST_CHECK(m_peer->fDisconnect);
    BOOST_CHECK(!m_peer->fSuccessfullyConnected);
}

BOOST_AUTO_TEST_CASE(missing_proof)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    Start(true);
    Version();
    Receive(NetMsg::Make(NetMsgType::VERACK));
    BOOST_CHECK(m_peer->fDisconnect);
    BOOST_CHECK(!m_peer->fSuccessfullyConnected);
}

BOOST_AUTO_TEST_CASE(duplicate_proof)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    Start(true);
    Version();
    ReceiveProof(Proof());
    BOOST_REQUIRE(!m_peer->fDisconnect);
    ReceiveProof(Proof());
    BOOST_CHECK(m_peer->fDisconnect);
    BOOST_CHECK(!m_peer->fSuccessfullyConnected);
}

BOOST_AUTO_TEST_CASE(invalid_mac)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    Start(true);
    Version();
    auto proof{Proof()};
    proof.back() ^= 1;
    ReceiveProof(proof);
    BOOST_CHECK(m_peer->fDisconnect);
    BOOST_CHECK(!m_peer->fSuccessfullyConnected);
}

BOOST_AUTO_TEST_CASE(wrong_session)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    Start(true);
    Version();
    const auto proof{holepunch::MakeProof(*m_remote_pair, m_attempt->round, uint256{})};
    ReceiveProof({proof.begin(), proof.end()});
    BOOST_CHECK(m_peer->fDisconnect);
    BOOST_CHECK(!m_peer->fSuccessfullyConnected);
}

BOOST_AUTO_TEST_CASE(wrong_role)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    Start(false);
    Version();
    // Reflect our own proof: it authenticates the wrong transport role and descriptor order.
    const auto proof{holepunch::MakeProof(m_attempt->pair, m_attempt->round, *m_remote_transport->GetInfo().session_id)};
    ReceiveProof({proof.begin(), proof.end()});
    BOOST_CHECK(m_peer->fDisconnect);
    BOOST_CHECK(!m_peer->fSuccessfullyConnected);
}

BOOST_AUTO_TEST_CASE(short_proof)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    Start(true);
    Version();
    auto proof{Proof()};
    proof.pop_back();
    ReceiveProof(proof);
    BOOST_CHECK(m_peer->fDisconnect);
    BOOST_CHECK(!m_peer->fSuccessfullyConnected);
}

BOOST_AUTO_TEST_CASE(incompatible_version)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    Start(true);
    Version(FEATURE_VERSION - 1);
    BOOST_CHECK(m_peer->fDisconnect);
    BOOST_CHECK(!m_peer->fSuccessfullyConnected);
}

BOOST_AUTO_TEST_CASE(no_v1_fallback)
{
    V1Transport v1{0};
    auto version{NetMsg::Make(NetMsgType::VERSION)};
    BOOST_REQUIRE(v1.SetMessageToSend(version));
    auto [prefix, more, type] = v1.GetBytesToSend(false);
    V2Transport responder{1, false, false};
    bool accepted{true};
    while (accepted && !prefix.empty()) accepted = responder.ReceivedBytes(prefix);
    BOOST_CHECK(!accepted);
    V2Transport initiator{2, true, false};
    const auto [key, key_more, key_type] = initiator.GetBytesToSend(false);
    BOOST_REQUIRE(key.size() >= 24);
    initiator.MarkBytesSent(key.size());
    BOOST_CHECK(!initiator.ShouldReconnectV1());
}

BOOST_AUTO_TEST_SUITE_END()
