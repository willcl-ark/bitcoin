// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <holepunch_manager.h>
#include <util/time.h>

#include <algorithm>
#include <cstdlib>
#include <tuple>

namespace holepunch {
namespace {
int64_t WallMillis() { return Ticks<std::chrono::milliseconds>(NodeClock::now().time_since_epoch()); }
}

Manager::Manager(const uint256& genesis, bool participate, std::optional<CService> manual_endpoint, uint8_t format)
    : m_genesis{genesis}, m_participate{participate}, m_manual_endpoint{std::move(manual_endpoint)}, m_format{format},
      m_startup_epoch{Epoch(WallMillis())},
      m_key_epoch{m_startup_epoch}, m_last_round{static_cast<uint64_t>(WallMillis() / (1000 * GetProfile(m_format).round_seconds))},
      m_last_wall_ms{WallMillis()}, m_last_tick{Clock::now()}, m_last_refresh{m_last_tick}
{
}

uint32_t Manager::Epoch(int64_t wall_ms) const
{
    return wall_ms / (1000 * GetProfile(m_format).epoch_seconds);
}

bool Manager::AddDescriptor(const Descriptor& descriptor, const std::vector<unsigned char>& source_group,
                            const std::vector<unsigned char>& destination_group)
{
    LOCK(m_mutex);
    if (descriptor.format != m_format) return false;
    const auto epoch{Epoch(WallMillis())};
    m_cache.Expire(epoch);
    // Reserve every endpoint owned in this epoch, including temporarily lost mappings.
    if (auto own = m_owned.find(descriptor.endpoint); own != m_owned.end() && own->second.first.epoch == epoch) {
        return own->second.first == descriptor;
    }
    return m_cache.Insert(descriptor, epoch, source_group, destination_group);
}

bool Manager::IsRetained(const Descriptor& descriptor)
{
    LOCK(m_mutex);
    if (descriptor.format != m_format) return false;
    const auto now{WallMillis()};
    m_cache.Expire(Epoch(now));
    if (auto own = m_owned.find(descriptor.endpoint); own != m_owned.end()) {
        return LocalDescriptorLocked(now, Clock::now()) == descriptor;
    }
    return m_cache.Contains(descriptor);
}

std::vector<Descriptor> Manager::GetDescriptors()
{
    LOCK(m_mutex);
    return m_cache.GetAll(Epoch(WallMillis()));
}

std::optional<Descriptor> Manager::LocalDescriptorLocked(int64_t wall_ms, Clock::time_point now)
{
    AssertLockHeld(m_mutex);
    const uint32_t epoch{Epoch(wall_ms)};
    // A clock reversal must never cause a second key to be created for an old epoch.
    if (epoch < m_key_epoch) return std::nullopt;
    m_key_epoch = epoch;
    std::erase_if(m_owned, [epoch](const auto& item) { return item.second.first.epoch != epoch; });
    if (!m_participate || epoch <= m_startup_epoch || wall_ms % (1000 * GetProfile(m_format).epoch_seconds) < 5000) return std::nullopt;
    std::vector<Observation> observations;
    for (const auto& [id, observer] : m_observers) {
        if (observer.complete && observer.endpoint && now - observer.reported < std::chrono::minutes{30}) {
            observations.push_back({*observer.endpoint, observer.group});
        }
    }
    // An explicit isolated-network experiment may use a mapping supplied by the operator.
    const auto endpoint{m_manual_endpoint ? m_manual_endpoint : FindObservedEndpoint(observations, m_format)};
    if (!endpoint || !IsValidEndpoint(*endpoint, m_format)) {
        m_cache.SetLocal(std::nullopt);
        return std::nullopt;
    }
    auto own{m_owned.find(*endpoint)};
    if (own == m_owned.end()) {
        // Bound retained epoch keys even if observations repeatedly change the mapping.
        if (m_owned.size() >= 1024) return std::nullopt;
        CKey key;
        key.MakeNewKey(true);
        Descriptor descriptor{*endpoint, epoch, key.GetPubKey(), m_format};
        own = m_owned.emplace(*endpoint, std::make_pair(descriptor, std::move(key))).first;
    }
    m_cache.SetLocal(own->second.first);
    return own->second.first;
}

std::optional<Descriptor> Manager::LocalDescriptor()
{
    LOCK(m_mutex);
    return LocalDescriptorLocked(WallMillis(), Clock::now());
}

void Manager::AddObserver(int64_t node, std::vector<unsigned char> group)
{
    LOCK(m_mutex);
    m_observers.emplace(node, Observer{std::move(group), std::nullopt, Clock::now(), {}, false});
}

void Manager::ReportObserver(int64_t node, const CService& endpoint)
{
    LOCK(m_mutex);
    if (auto it = m_observers.find(node); it != m_observers.end() && !it->second.endpoint && IsValidEndpoint(endpoint, m_format)) {
        it->second.endpoint = endpoint;
        it->second.reported = Clock::now();
    }
}

void Manager::ObserverHandshakeComplete(int64_t node)
{
    LOCK(m_mutex);
    auto observer = m_observers.find(node);
    if (observer == m_observers.end() || observer->second.complete) return;
    observer->second.complete = true;
    if (m_observers.size() > STEADY_OBSERVERS) {
        // Retire the oldest only once the replacement has completed its handshake.
        auto oldest = std::min_element(m_observers.begin(), m_observers.end(), [](const auto& a, const auto& b) {
            return a.second.connected < b.second.connected;
        });
        if (oldest->first != node) {
            m_disconnect.push_back(oldest->first);
            m_observers.erase(oldest);
            m_last_refresh = Clock::now();
        }
    }
}

bool Manager::NeedsObserver()
{
    LOCK(m_mutex);
    if (m_format == FAST_FORMAT) return m_participate && !m_manual_endpoint && m_observers.empty();
    return m_participate && !m_manual_endpoint && (m_observers.size() < STEADY_OBSERVERS ||
        (m_observers.size() < MAX_OBSERVERS && Clock::now() - m_last_refresh >= std::chrono::minutes{10}));
}

std::set<std::vector<unsigned char>> Manager::ObserverGroups()
{
    LOCK(m_mutex);
    std::set<std::vector<unsigned char>> groups;
    for (const auto& [id, observer] : m_observers) groups.insert(observer.group);
    return groups;
}

void Manager::RemoveNode(int64_t node)
{
    LOCK(m_mutex);
    m_observers.erase(node);
    std::erase_if(m_active, [node](const auto& active) { return active.node == node; });
}

void Manager::CancelPending()
{
    AssertLockHeld(m_mutex);
    m_scheduled.clear();
    std::erase_if(m_active, [this](const auto& active) EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
        if (active.complete) return false;
        if (active.node) m_disconnect.push_back(*active.node);
        return true;
    });
}

bool Manager::CompleteHandshake(int64_t node)
{
    LOCK(m_mutex);
    const auto it = std::find_if(m_active.begin(), m_active.end(), [node](const auto& active) { return active.node == node; });
    if (it == m_active.end() || it->complete || Clock::now() >= it->attempt->start + std::chrono::seconds{20}) return false;
    const size_t completed{static_cast<size_t>(std::count_if(m_active.begin(), m_active.end(), [](const auto& active) { return active.complete; }))};
    if (completed >= MAX_ESTABLISHED || std::any_of(m_active.begin(), m_active.end(), [&](const auto& other) {
            return &other != &*it && static_cast<const CNetAddr&>(other.remote) == static_cast<const CNetAddr&>(it->remote);
        })) return false;
    it->complete = true;
    it->attempt.reset();
    if (completed + 1 == MAX_ESTABLISHED) CancelPending();
    return true;
}

bool Manager::AttachNode(const std::shared_ptr<const Attempt>& attempt, int64_t node)
{
    LOCK(m_mutex);
    for (auto& active : m_active) {
        if (active.attempt == attempt) {
            active.node = node;
            return true;
        }
    }
    return false;
}

void Manager::FailAttempt(const std::shared_ptr<const Attempt>& attempt)
{
    LOCK(m_mutex);
    std::erase_if(m_active, [&attempt](const auto& active) { return active.attempt == attempt; });
}

bool Manager::IsActive(const std::shared_ptr<const Attempt>& attempt)
{
    LOCK(m_mutex);
    return std::any_of(m_active.begin(), m_active.end(), [&attempt](const auto& active) { return active.attempt == attempt; });
}

Manager::Actions Manager::Poll(int64_t wall_ms, Clock::time_point now, bool network_active,
                               const std::function<bool(const CService&)>& allowed)
{
    LOCK(m_mutex);
    Actions actions;
    const int64_t elapsed{Ticks<std::chrono::milliseconds>(now - m_last_tick)};
    const bool clock_jump{std::abs(wall_ms - m_last_wall_ms - elapsed) > 1000};
    m_last_wall_ms = wall_ms;
    m_last_tick = now;
    const uint64_t round{static_cast<uint64_t>(wall_ms / (1000 * GetProfile(m_format).round_seconds))};
    if (clock_jump || !network_active) {
        CancelPending();
        // A clock reversal must not allow a previously considered round to run again.
        m_last_round = std::max(m_last_round, round);
    }
    std::erase_if(m_active, [&](const auto& active) EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
        if (active.complete || now < active.attempt->start + std::chrono::seconds{20}) return false;
        if (active.node) m_disconnect.push_back(*active.node);
        return true;
    });
    std::erase_if(m_observers, [&](const auto& entry) EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
        const auto& observer = entry.second;
        const bool stale = observer.endpoint && now - observer.reported >= std::chrono::minutes{30};
        const bool failed = (!observer.complete || (m_format == FAST_FORMAT && !observer.endpoint)) &&
            now - observer.connected >= std::chrono::seconds{20};
        if (!stale && !failed) return false;
        m_disconnect.push_back(entry.first);
        return true;
    });
    std::erase_if(m_recent, [now](const auto& entry) { return now >= entry.second; });
    const auto local{LocalDescriptorLocked(wall_ms, now)};
    m_cache.Expire(Epoch(wall_ms));
    std::erase_if(m_pairs, [&](const auto& entry) EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
        return local != entry.second.local || !m_cache.Contains(entry.second.remote);
    });
    const auto connected_ip = [&](const CNetAddr& ip) EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
        return std::any_of(m_active.begin(), m_active.end(), [&](const auto& active) {
            return static_cast<const CNetAddr&>(active.remote) == ip;
        });
    };
    if (network_active && !clock_jump && round > m_last_round) {
        m_scheduled.clear();
        m_last_round = round;
        const size_t pending{static_cast<size_t>(std::count_if(m_active.begin(), m_active.end(), [](const auto& active) { return !active.complete; }))};
        if (local && m_active.size() < MAX_ESTABLISHED && pending < MAX_PENDING && m_recent.size() < MAX_RECENT) {
            struct Candidate { Descriptor remote; PublicSchedule rank; };
            std::map<CNetAddr, Candidate> groups;
            for (const auto& remote : m_cache.GetAll(local->epoch)) {
                const CNetAddr& ip{remote.endpoint};
                if (m_recent.contains(ip) || connected_ip(ip) || (allowed && !allowed(remote.endpoint))) continue;
                const auto rank{RankPair(*local, remote, m_genesis, round)};
                if (!rank || !rank->eligible) continue;
                const auto it{groups.find(ip)};
                if (it == groups.end()) {
                    groups.emplace(ip, Candidate{remote, *rank});
                } else if (std::tie(rank->endpoint_priority, rank->endpoint_tie_break) <
                           std::tie(it->second.rank.endpoint_priority, it->second.rank.endpoint_tie_break)) {
                    it->second = {remote, *rank};
                }
            }
            std::vector<Candidate> candidates;
            for (auto& [ip, candidate] : groups) candidates.push_back(std::move(candidate));
            std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
                return std::tie(a.rank.priority, a.rank.tie_break) < std::tie(b.rank.priority, b.rank.tie_break);
            });
            // Reserve all three resources in priority order before learning start times.
            const size_t capacity{std::min({MAX_ROUND_ATTEMPTS, MAX_PENDING - pending,
                                           MAX_ESTABLISHED - m_active.size(), MAX_RECENT - m_recent.size()})};
            if (candidates.size() > capacity) candidates.resize(capacity);
            size_t ecdh_work{0};
            std::map<std::vector<unsigned char>, size_t> source_work;
            for (const auto& candidate : candidates) {
                auto cached{m_pairs.find(candidate.remote.endpoint)};
                if (cached == m_pairs.end()) {
                    const auto source{m_cache.SourceGroup(candidate.remote.endpoint)};
                    // Charge unsuccessful derivations too; gossip cannot bypass these budgets.
                    if (ecdh_work >= MAX_ROUND_ECDH || source_work[source] >= MAX_SOURCE_ROUND_ECDH) continue;
                    ++ecdh_work;
                    ++source_work[source];
                    cached = m_pairs.emplace(candidate.remote.endpoint, CachedPair{*local, candidate.remote,
                        DerivePair(m_owned.at(local->endpoint).second, *local, candidate.remote, m_genesis)}).first;
                }
                if (!cached->second.pair) continue;
                const auto schedule{Schedule(*cached->second.pair, round)};
                if (!schedule || !schedule->eligible || schedule->start_ms <= static_cast<uint64_t>(wall_ms)) continue;
                m_scheduled.push_back(std::make_shared<Attempt>(*cached->second.pair, round,
                    now + std::chrono::milliseconds{schedule->start_ms - wall_ms}));
            }
        }
    }
    std::erase_if(m_scheduled, [&](const auto& attempt) EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
        if (now < attempt->start) return false;
        const auto& remote{attempt->pair.remote};
        const CNetAddr& ip{remote.endpoint};
        const size_t pending{static_cast<size_t>(std::count_if(m_active.begin(), m_active.end(), [](const auto& active) { return !active.complete; }))};
        if (network_active && local == attempt->pair.local && m_cache.Contains(remote) &&
            m_active.size() < MAX_ESTABLISHED && pending < MAX_PENDING && !connected_ip(ip) &&
            !m_recent.contains(ip) && m_recent.size() < MAX_RECENT && (!allowed || allowed(remote.endpoint)) &&
            now < attempt->start + std::chrono::seconds{10}) {
            // A reservation or skipped selection never creates or extends this fixed expiry.
            const auto cooldown{m_format == FAST_FORMAT ? std::chrono::seconds{30} : std::chrono::seconds{600}};
            m_recent.emplace(ip, now + cooldown);
            m_active.push_back({attempt, std::nullopt, false, remote.endpoint});
            actions.start.push_back(attempt);
        }
        return true;
    });
    actions.disconnect.swap(m_disconnect);
    return actions;
}
} // namespace holepunch
