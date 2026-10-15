// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <holepunch_manager.h>
#include <key.h>
#include <netbase.h>
#include <test/util/setup_common.h>
#include <test/util/time.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <tuple>
#include <vector>

#include <boost/test/unit_test.hpp>

namespace {
using namespace std::chrono_literals;

CKey TestKey(uint32_t value)
{
    std::array<unsigned char, 32> secret{};
    for (size_t i{0}; i < 4; ++i) secret[31 - i] = value >> (8 * i);
    CKey key;
    key.Set(secret.begin(), secret.end(), true);
    return key;
}

struct ManagerTestingSetup : BasicTestingSetup {
    static constexpr int64_t INITIAL_SECONDS{101 * 3600 - 10};
    FakeNodeClock m_wall{std::chrono::seconds{INITIAL_SECONDS}};
    FakeSteadyClock m_steady;
    int64_t m_wall_ms{INITIAL_SECONDS * 1000};
    const uint256 m_genesis{};
    holepunch::Manager m_manager{m_genesis, true};
    uint32_t m_next_key{1};
    const CService m_endpoint{LookupNumeric("8.8.8.8", 8333)};

    holepunch::Manager::Actions Poll(bool active = true)
    {
        return m_manager.Poll(m_wall_ms, holepunch::Clock::now(), active);
    }

    holepunch::Manager::Actions Advance(std::chrono::milliseconds elapsed, bool active = true)
    {
        m_steady += elapsed;
        m_wall_ms += elapsed.count();
        m_wall.set(std::chrono::seconds{m_wall_ms / 1000});
        return Poll(active);
    }

    holepunch::Manager::Actions AdvanceTo(uint64_t wall_ms)
    {
        BOOST_REQUIRE(wall_ms >= static_cast<uint64_t>(m_wall_ms));
        return Advance(std::chrono::milliseconds{wall_ms - m_wall_ms});
    }

    void Observe(int64_t node, unsigned char group, const CService& endpoint)
    {
        m_manager.AddObserver(node, {group});
        m_manager.ReportObserver(node, endpoint);
        m_manager.ObserverHandshakeComplete(node);
    }

    void Ready()
    {
        Observe(1, 1, m_endpoint);
        Observe(2, 2, m_endpoint);
        Advance(15s);
        BOOST_REQUIRE(m_manager.LocalDescriptor());
    }

    struct Candidate {
        holepunch::Descriptor descriptor;
        holepunch::RoundSchedule schedule;
    };

    /** Derive expected schedules as the remote participant, without accessing local secrets. */
    std::vector<Candidate> Candidates(uint64_t round, size_t count, const char* ip = "8.8.4.4", uint16_t fixed_port = 0)
    {
        const auto local{m_manager.LocalDescriptor()};
        BOOST_REQUIRE(local);
        std::vector<Candidate> candidates;
        for (size_t i{0}; i < 8192 && candidates.size() < count; ++i) {
            const auto index{m_next_key++};
            const auto key{TestKey(index)};
            const uint16_t port{fixed_port ? fixed_port : static_cast<uint16_t>(10000 + index)};
            holepunch::Descriptor remote{LookupNumeric(ip, port), local->epoch, key.GetPubKey()};
            const auto pair{holepunch::DerivePair(key, remote, *local, m_genesis)};
            BOOST_REQUIRE(pair);
            const auto schedule{holepunch::Schedule(*pair, round)};
            BOOST_REQUIRE(schedule);
            if (!schedule->eligible || std::any_of(candidates.begin(), candidates.end(), [&](const auto& candidate) {
                    return candidate.schedule.start_ms == schedule->start_ms;
                })) continue;
            BOOST_REQUIRE(m_manager.AddDescriptor(remote));
            candidates.push_back({remote, *schedule});
        }
        BOOST_REQUIRE_EQUAL(candidates.size(), count);
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return std::tie(a.schedule.priority, a.schedule.tie_break) < std::tie(b.schedule.priority, b.schedule.tie_break);
        });
        return candidates;
    }

    uint64_t NextRound() const { return m_wall_ms / 60000 + 1; }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(holepunch_manager_tests, ManagerTestingSetup)

BOOST_AUTO_TEST_CASE(local_descriptor_lifecycle)
{
    Observe(1, 1, m_endpoint);
    Observe(2, 2, m_endpoint);
    BOOST_CHECK(!m_manager.LocalDescriptor()); // A restart must wait for the next epoch.
    Advance(10s);
    BOOST_CHECK(!m_manager.LocalDescriptor());
    const holepunch::Descriptor premature{m_endpoint, 101, TestKey(1).GetPubKey()};
    BOOST_REQUIRE(m_manager.AddDescriptor(premature));
    Advance(4999ms);
    BOOST_CHECK(!m_manager.LocalDescriptor());
    Advance(1ms);
    const auto original{m_manager.LocalDescriptor()};
    BOOST_REQUIRE(original);
    BOOST_CHECK(m_manager.IsRetained(*original));
    BOOST_CHECK(!m_manager.IsRetained(premature));
    BOOST_CHECK(m_manager.GetDescriptors().empty());

    auto conflict{*original};
    conflict.pubkey = TestKey(1).GetPubKey();
    BOOST_CHECK(!m_manager.AddDescriptor(conflict));
    m_manager.RemoveNode(2);
    BOOST_CHECK(!m_manager.LocalDescriptor());
    BOOST_CHECK(!m_manager.IsRetained(*original));
    BOOST_CHECK(!m_manager.AddDescriptor(conflict));
    Observe(2, 2, m_endpoint);
    BOOST_CHECK(m_manager.LocalDescriptor() == original);

    const auto changed_endpoint{LookupNumeric("9.9.9.9", 18444)};
    m_manager.RemoveNode(1);
    m_manager.RemoveNode(2);
    Observe(3, 3, changed_endpoint);
    Observe(4, 4, changed_endpoint);
    const auto changed{m_manager.LocalDescriptor()};
    BOOST_REQUIRE(changed);
    BOOST_CHECK(changed->endpoint == changed_endpoint);
    BOOST_CHECK(changed->pubkey != original->pubkey);
    m_manager.RemoveNode(3);
    m_manager.RemoveNode(4);
    Observe(1, 1, m_endpoint);
    Observe(2, 2, m_endpoint);
    BOOST_CHECK(m_manager.LocalDescriptor() == original);

    Advance(1h);
    Observe(1, 1, m_endpoint);
    Observe(2, 2, m_endpoint);
    const auto next_epoch{m_manager.LocalDescriptor()};
    BOOST_REQUIRE(next_epoch);
    BOOST_CHECK_EQUAL(next_epoch->epoch, original->epoch + 1);
    BOOST_CHECK(next_epoch->pubkey != original->pubkey);
    BOOST_CHECK(!m_manager.IsRetained(*original));

    // A clock rollback must not recreate a forgotten key for an endpoint already used this epoch.
    const int64_t next_epoch_wall{m_wall_ms};
    m_wall_ms -= 3600000;
    m_wall.set(std::chrono::seconds{m_wall_ms / 1000});
    BOOST_CHECK(!m_manager.LocalDescriptor());
    m_wall_ms = next_epoch_wall;
    m_wall.set(std::chrono::seconds{m_wall_ms / 1000});
    BOOST_CHECK(m_manager.LocalDescriptor() == next_epoch);
}

BOOST_AUTO_TEST_CASE(relay_cache)
{
    holepunch::Manager relay{m_genesis, false};
    BOOST_CHECK(!relay.NeedsObserver());
    const holepunch::Descriptor descriptor{m_endpoint, 100, TestKey(1).GetPubKey()};
    BOOST_CHECK(relay.AddDescriptor(descriptor));
    BOOST_CHECK(relay.AddDescriptor(descriptor));
    auto conflict{descriptor};
    conflict.pubkey = TestKey(2).GetPubKey();
    BOOST_CHECK(!relay.AddDescriptor(conflict));
    BOOST_CHECK(relay.IsRetained(descriptor));
    BOOST_CHECK(!relay.IsRetained(conflict));
    BOOST_REQUIRE_EQUAL(relay.GetDescriptors().size(), 1);
    BOOST_CHECK(relay.GetDescriptors().front() == descriptor);
    ++conflict.epoch;
    BOOST_CHECK(!relay.AddDescriptor(conflict));
    Advance(10s);
    BOOST_CHECK(relay.GetDescriptors().empty());
    BOOST_CHECK(!relay.IsRetained(descriptor));
    BOOST_CHECK(!relay.AddDescriptor(descriptor));
    BOOST_CHECK(relay.AddDescriptor(conflict));
    relay.AddObserver(1, {1});
    relay.AddObserver(2, {2});
    relay.ReportObserver(1, m_endpoint);
    relay.ReportObserver(2, m_endpoint);
    relay.ObserverHandshakeComplete(1);
    relay.ObserverHandshakeComplete(2);
    Advance(5s);
    BOOST_CHECK(!relay.LocalDescriptor());
}

BOOST_AUTO_TEST_CASE(manual_endpoint)
{
    holepunch::Manager manual{m_genesis, true, m_endpoint};
    holepunch::Manager private_address{m_genesis, true, LookupNumeric("192.168.1.1", 8333)};
    holepunch::Manager ipv6_address{m_genesis, true, LookupNumeric("2606:4700::1111", 8333)};
    holepunch::Manager zero_port{m_genesis, true, LookupNumeric("8.8.8.8", 0)};
    holepunch::Manager disabled{m_genesis, false, m_endpoint};
    BOOST_CHECK(!manual.NeedsObserver());
    BOOST_CHECK(!manual.LocalDescriptor());
    Advance(10s);
    BOOST_CHECK(!manual.LocalDescriptor());
    Advance(4999ms);
    BOOST_CHECK(!manual.LocalDescriptor());
    Advance(1ms);
    const auto descriptor{manual.LocalDescriptor()};
    BOOST_REQUIRE(descriptor);
    BOOST_CHECK(descriptor->endpoint == m_endpoint);
    BOOST_CHECK(manual.IsRetained(*descriptor));
    BOOST_CHECK(manual.ObserverGroups().empty());
    BOOST_CHECK(!private_address.LocalDescriptor());
    BOOST_CHECK(!ipv6_address.LocalDescriptor());
    BOOST_CHECK(!zero_port.LocalDescriptor());
    BOOST_CHECK(!disabled.LocalDescriptor());
    BOOST_CHECK(!disabled.NeedsObserver());

    auto conflict{*descriptor};
    conflict.pubkey = TestKey(1).GetPubKey();
    BOOST_CHECK(!manual.AddDescriptor(conflict));
    Advance(30min);
    BOOST_CHECK(manual.LocalDescriptor() == descriptor);
    BOOST_CHECK(!manual.NeedsObserver());
    Advance(30min);
    const auto next_epoch{manual.LocalDescriptor()};
    BOOST_REQUIRE(next_epoch);
    BOOST_CHECK_EQUAL(next_epoch->epoch, descriptor->epoch + 1);
    BOOST_CHECK(next_epoch->pubkey != descriptor->pubkey);
    BOOST_CHECK(!manual.IsRetained(*descriptor));
}

BOOST_AUTO_TEST_CASE(fast_profile)
{
    holepunch::Manager fast{m_genesis, true, LookupNumeric("127.0.0.1", 18333), holepunch::FAST_FORMAT};
    const auto poll_fast = [&] { return fast.Poll(m_wall_ms, holepunch::Clock::now(), true); };
    BOOST_CHECK_EQUAL(fast.GetFormat(), holepunch::FAST_FORMAT);
    BOOST_CHECK(!fast.NeedsObserver());
    BOOST_CHECK(!fast.LocalDescriptor());
    Advance(10s); // The accelerated epoch lasts 60 seconds and still has a five-second guard.
    BOOST_CHECK(!fast.LocalDescriptor());
    Advance(4999ms);
    BOOST_CHECK(!fast.LocalDescriptor());
    Advance(1ms);
    const auto local{fast.LocalDescriptor()};
    BOOST_REQUIRE(local);
    BOOST_CHECK_EQUAL(local->format, holepunch::FAST_FORMAT);
    BOOST_CHECK_EQUAL(local->epoch, m_wall_ms / 60000);

    const auto key{TestKey(1)};
    const holepunch::Descriptor remote{LookupNumeric("127.0.0.1", 18444), local->epoch, key.GetPubKey(), holepunch::FAST_FORMAT};
    BOOST_REQUIRE(fast.AddDescriptor(remote));
    BOOST_CHECK(fast.IsRetained(remote));
    auto version_zero{remote};
    version_zero.format = 0;
    version_zero.epoch = m_wall_ms / 3600000;
    BOOST_CHECK(!m_manager.AddDescriptor(version_zero));
    holepunch::Descriptor mixed{LookupNumeric("8.8.4.4", 8333), local->epoch, key.GetPubKey()};
    BOOST_CHECK(!fast.AddDescriptor(mixed));
    BOOST_CHECK(!fast.IsRetained(mixed));
    mixed.epoch = m_wall_ms / 3600000;
    mixed.format = holepunch::FAST_FORMAT;
    BOOST_CHECK(!m_manager.AddDescriptor(mixed));
    BOOST_CHECK(!m_manager.IsRetained(mixed));

    const uint64_t round{static_cast<uint64_t>(m_wall_ms / 30000 + 1)};
    const auto pair{holepunch::DerivePair(key, remote, *local, m_genesis)};
    BOOST_REQUIRE(pair);
    const auto schedule{holepunch::Schedule(*pair, round)};
    BOOST_REQUIRE(schedule);
    BOOST_CHECK(schedule->eligible);
    BOOST_CHECK(schedule->start_ms >= round * 30000 + 5000);
    BOOST_CHECK(schedule->start_ms < round * 30000 + 10000);
    BOOST_CHECK(poll_fast().start.empty());
    AdvanceTo(round * 30000);
    BOOST_CHECK(poll_fast().start.empty());
    AdvanceTo(schedule->start_ms);
    const auto actions{poll_fast()};
    BOOST_REQUIRE_EQUAL(actions.start.size(), 1);
    BOOST_CHECK(actions.start.front()->pair.remote == remote); // Same public IP is permitted in this profile.
    BOOST_REQUIRE(fast.AttachNode(actions.start.front(), 100));
    Advance(19999ms);
    BOOST_CHECK(poll_fast().disconnect.empty());
    Advance(1ms);
    BOOST_CHECK(!fast.CompleteHandshake(100));
    const auto expired{poll_fast()};
    BOOST_REQUIRE_EQUAL(expired.disconnect.size(), 1);
    BOOST_CHECK_EQUAL(expired.disconnect.front(), 100);
}

BOOST_AUTO_TEST_CASE(observer_agreement_and_rotation)
{
    Advance(15s);
    Observe(1, 1, m_endpoint);
    Observe(2, 1, m_endpoint);
    BOOST_CHECK(!m_manager.LocalDescriptor()); // Same network group cannot establish agreement.
    m_manager.AddObserver(3, {3});
    m_manager.ReportObserver(3, LookupNumeric("127.0.0.1", 8333));
    m_manager.ReportObserver(3, LookupNumeric("8.8.8.8", 0));
    m_manager.ReportObserver(3, LookupNumeric("2606:4700::1111", 8333));
    m_manager.ObserverHandshakeComplete(3);
    BOOST_CHECK(!m_manager.LocalDescriptor());
    m_manager.ReportObserver(3, m_endpoint);
    BOOST_REQUIRE(m_manager.LocalDescriptor());
    BOOST_CHECK(!m_manager.NeedsObserver());

    Advance(10min);
    BOOST_CHECK(m_manager.NeedsObserver());
    m_manager.AddObserver(4, {4});
    m_manager.ReportObserver(4, m_endpoint);
    BOOST_CHECK(!m_manager.NeedsObserver());
    BOOST_CHECK(Poll().disconnect.empty()); // Keep old observers until replacement completes.
    m_manager.ObserverHandshakeComplete(4);
    auto actions{Poll()};
    BOOST_REQUIRE_EQUAL(actions.disconnect.size(), 1);
    BOOST_CHECK_EQUAL(actions.disconnect.front(), 1);
    BOOST_CHECK(m_manager.ObserverGroups().contains({1})); // Observer 2 still uses group 1.
    BOOST_CHECK_EQUAL(m_manager.ObserverGroups().size(), 3);
    BOOST_CHECK(!m_manager.NeedsObserver());

    actions = Advance(20min);
    BOOST_CHECK_EQUAL(actions.disconnect.size(), 2);
    BOOST_CHECK(!m_manager.LocalDescriptor());
    BOOST_CHECK(m_manager.NeedsObserver());
    m_manager.AddObserver(5, {5});
    BOOST_CHECK(Advance(19999ms).disconnect.empty());
    actions = Advance(1ms);
    BOOST_REQUIRE_EQUAL(actions.disconnect.size(), 1);
    BOOST_CHECK_EQUAL(actions.disconnect.front(), 5);
}

BOOST_AUTO_TEST_CASE(priority_capacity_and_endpoint_tuples)
{
    Ready();
    const uint64_t round{NextRound()};
    auto candidates{Candidates(round, 6)};
    Candidates(round, 1, "8.8.8.8"); // Exclude the local public IP even on a different port.
    candidates.resize(holepunch::Manager::MAX_PENDING);
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.schedule.start_ms < b.schedule.start_ms;
    });
    BOOST_CHECK(AdvanceTo(round * 60000).start.empty());
    for (size_t i{0}; i < candidates.size(); ++i) {
        auto actions{AdvanceTo(candidates[i].schedule.start_ms)};
        BOOST_REQUIRE_EQUAL(actions.start.size(), 1);
        const auto& attempt{actions.start.front()};
        BOOST_CHECK(attempt->pair.remote == candidates[i].descriptor);
        BOOST_CHECK_EQUAL(attempt->round, round);
        BOOST_CHECK(m_manager.AttachNode(attempt, 100 + i));
        BOOST_CHECK(m_manager.CompleteHandshake(100 + i));
        BOOST_CHECK(!m_manager.CompleteHandshake(100 + i));
        BOOST_CHECK(Poll().start.empty());
    }
    const uint64_t full_round{NextRound()};
    Candidates(full_round, 2);
    BOOST_CHECK(AdvanceTo(full_round * 60000).start.empty());
    BOOST_CHECK(AdvanceTo(full_round * 60000 + 34999).start.empty());

    // Established tuples survive epoch changes and forbid a second connection with a new key.
    AdvanceTo(102 * 3600000 + 5000);
    Observe(1, 1, m_endpoint);
    Observe(2, 2, m_endpoint);
    m_manager.RemoveNode(103);
    const uint64_t duplicate_round{NextRound()};
    const auto duplicate{Candidates(duplicate_round, 1, "8.8.4.4", candidates[0].descriptor.endpoint.GetPort()).front()};
    AdvanceTo(duplicate_round * 60000);
    BOOST_CHECK(AdvanceTo(duplicate.schedule.start_ms).start.empty());
    BOOST_CHECK(Poll().disconnect.empty());
}

BOOST_AUTO_TEST_CASE(pinned_attempt_and_handshake_deadline)
{
    Ready();
    const uint64_t round{NextRound()};
    const auto candidate{Candidates(round, 1).front()};
    AdvanceTo(round * 60000);
    const auto actions{AdvanceTo(candidate.schedule.start_ms)};
    BOOST_REQUIRE_EQUAL(actions.start.size(), 1);
    const auto attempt{actions.start.front()};
    BOOST_REQUIRE(m_manager.AttachNode(attempt, 100));
    m_manager.RemoveNode(2);
    BOOST_CHECK(!m_manager.LocalDescriptor());

    // Force ordinary cache eviction while the attempt retains its original cryptographic inputs.
    auto filler{candidate.descriptor};
    for (uint32_t port{1}; port < 65536 && m_manager.IsRetained(candidate.descriptor); ++port) {
        filler.endpoint = LookupNumeric("9.9.9.9", port);
        BOOST_REQUIRE(m_manager.AddDescriptor(filler));
    }
    BOOST_CHECK(!m_manager.IsRetained(candidate.descriptor));
    BOOST_CHECK_EQUAL(m_manager.GetDescriptors().size(), 1024);
    BOOST_CHECK(m_manager.IsActive(attempt));
    BOOST_CHECK(attempt->pair.remote == candidate.descriptor);
    for (size_t retry{0}; retry < 4; ++retry) {
        const auto waiting{Advance(2s)};
        BOOST_CHECK(waiting.start.empty());
        BOOST_CHECK(waiting.disconnect.empty());
        BOOST_CHECK(m_manager.IsActive(attempt));
    }
    BOOST_CHECK(Advance(11999ms).disconnect.empty());
    BOOST_CHECK(m_manager.IsActive(attempt));
    m_steady += 1ms;
    ++m_wall_ms;
    m_wall.set(std::chrono::seconds{m_wall_ms / 1000});
    BOOST_CHECK(!m_manager.CompleteHandshake(100));
    const auto expired{Poll()};
    BOOST_REQUIRE_EQUAL(expired.disconnect.size(), 1);
    BOOST_CHECK_EQUAL(expired.disconnect.front(), 100);
    BOOST_CHECK(!m_manager.IsActive(attempt));
    BOOST_CHECK(!m_manager.AttachNode(attempt, 101));
}

BOOST_AUTO_TEST_CASE(filling_capacity_cancels_pending)
{
    Ready();
    uint64_t round{NextRound()};
    auto candidates{Candidates(round, 3)};
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.schedule.start_ms < b.schedule.start_ms;
    });
    AdvanceTo(round * 60000);
    for (size_t i{0}; i < candidates.size(); ++i) {
        const auto actions{AdvanceTo(candidates[i].schedule.start_ms)};
        BOOST_REQUIRE_EQUAL(actions.start.size(), 1);
        BOOST_REQUIRE(m_manager.AttachNode(actions.start.front(), 100 + i));
        BOOST_REQUIRE(m_manager.CompleteHandshake(100 + i));
    }

    // Start with fresh advertisements while preserving the three established connections.
    AdvanceTo(102 * 3600000 + 5000);
    Observe(1, 1, m_endpoint);
    Observe(2, 2, m_endpoint);
    round = NextRound();
    candidates = Candidates(round, 4);
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.schedule.start_ms < b.schedule.start_ms;
    });
    // Among four starts in a 30-second window, two are less than the handshake deadline apart.
    size_t closest{0};
    for (size_t i{1}; i + 1 < candidates.size(); ++i) {
        if (candidates[i + 1].schedule.start_ms - candidates[i].schedule.start_ms <
            candidates[closest + 1].schedule.start_ms - candidates[closest].schedule.start_ms) closest = i;
    }
    AdvanceTo(round * 60000);
    for (size_t i{0}; i < closest; ++i) {
        const auto actions{AdvanceTo(candidates[i].schedule.start_ms)};
        BOOST_REQUIRE_EQUAL(actions.start.size(), 1);
        m_manager.FailAttempt(actions.start.front());
        BOOST_CHECK(!m_manager.IsActive(actions.start.front()));
    }
    const auto first{AdvanceTo(candidates[closest].schedule.start_ms)};
    const auto second{AdvanceTo(candidates[closest + 1].schedule.start_ms)};
    BOOST_REQUIRE_EQUAL(first.start.size(), 1);
    BOOST_REQUIRE_EQUAL(second.start.size(), 1);
    BOOST_REQUIRE(m_manager.AttachNode(first.start.front(), 200));
    BOOST_REQUIRE(m_manager.AttachNode(second.start.front(), 201));
    BOOST_REQUIRE(m_manager.CompleteHandshake(200));
    const auto full{Poll()};
    BOOST_REQUIRE_EQUAL(full.disconnect.size(), 1);
    BOOST_CHECK_EQUAL(full.disconnect.front(), 201);
    BOOST_CHECK(!m_manager.IsActive(second.start.front()));
    BOOST_CHECK(AdvanceTo(round * 60000 + 34999).start.empty());
}

BOOST_AUTO_TEST_CASE(start_rechecks)
{
    Ready();
    uint64_t round{NextRound()};
    const auto hairpin{Candidates(round, 1, "8.8.8.8").front()};
    AdvanceTo(round * 60000);
    BOOST_CHECK(AdvanceTo(hairpin.schedule.start_ms).start.empty());

    round = NextRound();
    const auto unobserved{Candidates(round, 1).front()};
    AdvanceTo(round * 60000);
    m_manager.RemoveNode(2);
    BOOST_CHECK(AdvanceTo(unobserved.schedule.start_ms).start.empty());
    Observe(2, 2, m_endpoint);
    BOOST_CHECK(m_manager.LocalDescriptor());
    BOOST_CHECK(Advance(1ms).start.empty()); // Restoring agreement cannot shift the missed start.

    round = NextRound();
    Candidates(round, 1);
    AdvanceTo(round * 60000);
    // Every start in the round is at most 34,999 ms; none may start after its TCP deadline.
    BOOST_CHECK(AdvanceTo(round * 60000 + 44999).start.empty());
}

BOOST_AUTO_TEST_CASE(clock_discontinuity_and_network_disable)
{
    Ready();
    const uint64_t round{NextRound()};
    auto candidates{Candidates(round, 3)};
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.schedule.start_ms < b.schedule.start_ms;
    });
    AdvanceTo(round * 60000);
    const auto established{AdvanceTo(candidates[0].schedule.start_ms)};
    BOOST_REQUIRE_EQUAL(established.start.size(), 1);
    BOOST_REQUIRE(m_manager.AttachNode(established.start.front(), 100));
    BOOST_REQUIRE(m_manager.CompleteHandshake(100));
    const auto pending{AdvanceTo(candidates[1].schedule.start_ms)};
    BOOST_REQUIRE_EQUAL(pending.start.size(), 1);
    BOOST_REQUIRE(m_manager.AttachNode(pending.start.front(), 101));

    m_wall_ms += 2000; // A wall-clock jump without matching monotonic elapsed time.
    m_wall.set(std::chrono::seconds{m_wall_ms / 1000});
    const auto cancelled{Poll()};
    BOOST_REQUIRE_EQUAL(cancelled.disconnect.size(), 1);
    BOOST_CHECK_EQUAL(cancelled.disconnect.front(), 101);
    BOOST_CHECK(!m_manager.IsActive(pending.start.front()));
    BOOST_CHECK(AdvanceTo((round + 1) * 60000 - 1).start.empty());

    const uint64_t next_round{NextRound()};
    Candidates(next_round, 1);
    AdvanceTo(next_round * 60000);
    holepunch::Manager::Actions restarted;
    for (size_t second{0}; second < 35 && restarted.start.empty(); ++second) restarted = Advance(1s);
    BOOST_REQUIRE(!restarted.start.empty());
    for (size_t i{0}; i < restarted.start.size(); ++i) {
        BOOST_REQUIRE(m_manager.AttachNode(restarted.start[i], 102 + i));
    }
    const auto disabled{Poll(false)};
    BOOST_REQUIRE_EQUAL(disabled.disconnect.size(), restarted.start.size());
    BOOST_CHECK(std::find(disabled.disconnect.begin(), disabled.disconnect.end(), 100) == disabled.disconnect.end());
    for (const auto& attempt : restarted.start) BOOST_CHECK(!m_manager.IsActive(attempt));
    BOOST_CHECK(Poll(false).disconnect.empty());
}

BOOST_AUTO_TEST_SUITE_END()
