// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <send.h>

#include <compat/compat.h>
#include <netaddress.h>
#include <netmessagemaker.h>
#include <primitives/transaction.h>
#include <protocol.h>
#include <script/script.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/util/net.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <test/util/time.h>
#include <transport.h>
#include <uint256.h>
#include <util/threadinterrupt.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {
void initialize_send()
{
    static const auto setup = MakeNoLogFileContext<const BasicTestingSetup>();
}

class SenderSock : public ZeroSock
{
    FuzzedDataProvider& m_provider;
    FakeSteadyClock& m_clock;
    Transport& m_sender;
    const CTransaction& m_tx;
    const bool m_mutate;
    const bool m_fragment;
    const bool m_fail_write;
    CThreadInterrupt& m_interrupt;
    const MockableSteadyClock::time_point m_cancel_at;
    mutable std::unique_ptr<Transport> m_peer;
    mutable std::deque<CSerializedNetMsg> m_messages;
    mutable size_t m_command_count{0};

    SenderSock& operator=(Sock&&) override { std::abort(); }

    void Queue(CSerializedNetMsg msg) const
    {
        if (m_mutate) {
            switch (m_provider.ConsumeIntegralInRange<int>(0, 3)) {
            case 0: break;
            case 1: msg.data = m_provider.ConsumeBytes<uint8_t>(512); break;
            case 2: msg.m_type = m_provider.PickValueInArray(ALL_NET_MESSAGE_TYPES); break;
            case 3: m_messages.push_back(msg.Copy()); break;
            }
        }
        m_messages.push_back(std::move(msg));
    }

public:
    mutable bool inv_attempted{false};

    SenderSock(FuzzedDataProvider& provider, FakeSteadyClock& clock, Transport& sender, const CTransaction& tx, bool v2, CThreadInterrupt& interrupt)
        : m_provider{provider}, m_clock{clock}, m_sender{sender}, m_tx{tx}, m_mutate{provider.ConsumeBool()}, m_fragment{provider.ConsumeBool()}, m_fail_write{provider.ConsumeBool()}, m_interrupt{interrupt}, m_cancel_at{MockableSteadyClock::now() + (1101 - provider.ConsumeIntegralInRange<int>(0, 1100)) * 1ms}, m_peer{v2 ? std::unique_ptr<Transport>{std::make_unique<V2Transport>(0, false)} : std::make_unique<V1Transport>(0)}
    {
    }

    bool CleanExchange() const { return !m_mutate && !m_fragment && !m_fail_write; }

    ssize_t Send(const void* buf, size_t len, int) const override
    {
        m_clock += 1ms;
        if (MockableSteadyClock::now() >= m_cancel_at) m_interrupt();
        const auto [bytes, more, type] = m_sender.GetBytesToSend(false);
        if (type == NetMsgType::INV) inv_attempted = true;
        if (m_fail_write && m_provider.ConsumeBool()) return 0;
        const size_t count = m_fragment ? m_provider.ConsumeIntegralInRange<size_t>(1, len) : len;
        std::span<const uint8_t> input{static_cast<const uint8_t*>(buf), count};
        while (!input.empty()) {
            if (!m_peer->ReceivedBytes(input)) return 0;
            if (!m_peer->ReceivedMessageComplete()) continue;
            bool rejected{false};
            auto msg = m_peer->GetReceivedMessage(NodeClock::epoch, rejected);
            assert(!rejected);
            constexpr std::array<std::string_view, 5> expected{
                NetMsgType::VERSION, NetMsgType::VERACK, NetMsgType::INV, NetMsgType::TX, NetMsgType::PING};
            assert(m_command_count < expected.size());
            assert(msg.m_type == expected[m_command_count++]);
            if (msg.m_type == NetMsgType::VERSION) {
                Queue(NetMsg::Make(NetMsgType::VERSION, int32_t{70016}, uint64_t{NODE_NETWORK | NODE_WITNESS}, int64_t{0},
                                   uint64_t{0}, CNetAddr::V1(CService{}), uint64_t{0}, CNetAddr::V1(CService{}),
                                   uint64_t{1}, std::string{"/fuzz/"}, int32_t{0}, true));
                Queue(NetMsg::Make(NetMsgType::VERACK));
            } else if (msg.m_type == NetMsgType::INV) {
                Queue(NetMsg::Make(NetMsgType::GETDATA, std::vector<CInv>{CInv{MSG_TX, m_tx.GetHash().ToUint256()}}));
            } else if (msg.m_type == NetMsgType::TX) {
                DataStream exact;
                exact << TX_WITH_WITNESS(m_tx);
                assert(std::ranges::equal(msg.m_recv, exact));
            } else if (msg.m_type == NetMsgType::PING) {
                Queue(NetMsg::Make(NetMsgType::PONG, std::span{msg.m_recv}));
            }
        }
        return count;
    }

    ssize_t Recv(void* buf, size_t len, int) const override
    {
        m_clock += 1ms;
        if (MockableSteadyClock::now() >= m_cancel_at) m_interrupt();
        if (!m_messages.empty() && m_peer->SetMessageToSend(m_messages.front())) m_messages.pop_front();
        const auto [bytes, more, type] = m_peer->GetBytesToSend(!m_messages.empty());
        if (bytes.empty()) {
#ifdef WIN32
            WSASetLastError(WSAEWOULDBLOCK);
#else
            errno = EWOULDBLOCK;
#endif
            return -1;
        }
        size_t count = std::min(len, bytes.size());
        if (m_fragment) count = m_provider.ConsumeIntegralInRange<size_t>(1, count);
        std::memcpy(buf, bytes.data(), count);
        m_peer->MarkBytesSent(count);
        return count;
    }

    bool Wait(std::chrono::milliseconds timeout, Event requested, Event* occurred) const override
    {
        m_clock += timeout;
        if (occurred) *occurred = requested;
        return true;
    }
};
} // namespace

FUZZ_TARGET(send, .init = initialize_send)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider provider{buffer.data(), buffer.size()};
    FakeSteadyClock clock;
    const bool v2 = provider.ConsumeBool();
    CMutableTransaction mutable_tx;
    mutable_tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{1}), 0});
    mutable_tx.vout.emplace_back(1, CScript{});
    if (provider.ConsumeBool()) mutable_tx.vin[0].scriptWitness.stack = {{1}};
    const CTransaction tx{mutable_tx};
    std::unique_ptr<Transport> sender = v2 ? std::unique_ptr<Transport>{std::make_unique<V2Transport>(0, true)} : std::make_unique<V1Transport>(0);
    CThreadInterrupt interrupt;
    SenderSock sock{provider, clock, *sender, tx, v2, interrupt};
    if (provider.ConsumeBool()) interrupt();
    bool disclosed{false};
    const auto result = txsend::RunSession(sock, *sender, tx, MockableSteadyClock::now() + 1s, interrupt, disclosed);
    assert(disclosed == sock.inv_attempted);
    if (sock.CleanExchange() && !interrupt) assert(result == txsend::SessionResult::SUCCESS);
    if (result == txsend::SessionResult::SUCCESS) assert(disclosed);
    if (disclosed) assert(!sender->ShouldReconnectV1());
}
