// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_HOLEPUNCH_MANAGER_H
#define BITCOIN_HOLEPUNCH_MANAGER_H

#include <holepunch.h>
#include <key.h>
#include <sync.h>
#include <util/time.h>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace holepunch {
using Clock = MockableSteadyClock;

/** Inputs fixed when an attempt starts; gossip cannot modify them. */
struct Attempt {
    Pair pair;
    uint64_t round;
    Clock::time_point start;
};

/** Nonpersistent experiment state, shared by the message and connection threads. */
class Manager
{
    struct Observer {
        std::vector<unsigned char> group;
        std::optional<CService> endpoint;
        Clock::time_point connected;
        Clock::time_point reported;
        bool complete{false};
    };
    struct Active {
        std::shared_ptr<const Attempt> attempt;
        std::optional<int64_t> node;
        bool complete{false};
        CService remote;
    };
    const uint256 m_genesis;
    const bool m_participate;
    const std::optional<CService> m_manual_endpoint;
    const uint8_t m_format;
    const uint32_t m_startup_epoch;
    mutable Mutex m_mutex;
    DescriptorCache m_cache GUARDED_BY(m_mutex);
    std::map<CService, std::pair<Descriptor, CKey>> m_owned GUARDED_BY(m_mutex);
    std::map<int64_t, Observer> m_observers GUARDED_BY(m_mutex);
    std::vector<std::shared_ptr<const Attempt>> m_scheduled GUARDED_BY(m_mutex);
    std::vector<Active> m_active GUARDED_BY(m_mutex);
    std::vector<int64_t> m_disconnect GUARDED_BY(m_mutex);
    uint32_t m_key_epoch GUARDED_BY(m_mutex);
    uint64_t m_last_round GUARDED_BY(m_mutex);
    int64_t m_last_wall_ms GUARDED_BY(m_mutex);
    Clock::time_point m_last_tick GUARDED_BY(m_mutex);
    Clock::time_point m_last_refresh GUARDED_BY(m_mutex);
    uint32_t Epoch(int64_t wall_ms) const;
    std::optional<Descriptor> LocalDescriptorLocked(int64_t wall_ms, Clock::time_point now) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void CancelPending() EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

public:
    static constexpr size_t MAX_ESTABLISHED{4};
    static constexpr size_t MAX_PENDING{4};
    static constexpr size_t MAX_OBSERVERS{4};
    static constexpr size_t STEADY_OBSERVERS{3};

    explicit Manager(const uint256& genesis, bool participate, std::optional<CService> manual_endpoint = std::nullopt, uint8_t format = 0);
    uint8_t GetFormat() const { return m_format; }
    bool AddDescriptor(const Descriptor& descriptor) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool IsRetained(const Descriptor& descriptor) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::vector<Descriptor> GetDescriptors() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::optional<Descriptor> LocalDescriptor() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void AddObserver(int64_t node, std::vector<unsigned char> group) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void ReportObserver(int64_t node, const CService& endpoint) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void ObserverHandshakeComplete(int64_t node) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool NeedsObserver() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::set<std::vector<unsigned char>> ObserverGroups() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void RemoveNode(int64_t node) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool CompleteHandshake(int64_t node) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool AttachNode(const std::shared_ptr<const Attempt>& attempt, int64_t node) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void FailAttempt(const std::shared_ptr<const Attempt>& attempt) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool IsActive(const std::shared_ptr<const Attempt>& attempt) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    struct Actions {
        std::vector<std::shared_ptr<const Attempt>> start;
        std::vector<int64_t> disconnect;
    };
    /** Advance timers and obtain sockets to start/close. Does not perform network I/O. */
    Actions Poll(int64_t wall_ms, Clock::time_point now, bool network_active) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
};
} // namespace holepunch
#endif // BITCOIN_HOLEPUNCH_MANAGER_H
