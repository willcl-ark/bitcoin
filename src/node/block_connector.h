// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_BLOCK_CONNECTOR_H
#define BITCOIN_NODE_BLOCK_CONNECTOR_H

#include <primitives/block.h>
#include <sync.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace node {

/** Opaque monotonic ticket returned by BlockConnector::Latest() to check whether
 *  the active-chain activation pass covering a handed-off block has finished. */
struct HandOffTicket {
    uint64_t value{0};
};

/**
 * Single background connector thread that activates the best chain for blocks
 * handed off asynchronously by the P2P receive path during IBD.
 *
 * The worker thread is started lazily on the first Signal() call and can be
 * stopped and joined cleanly on IBD exit via StopForIdle() (and restarted if
 * Signal() is ever called again before permanent shutdown via Stop()).
 * Callers that never hand off blocks (such as libbitcoinkernel or synchronous
 * tests) spawn zero extra threads.
 */
class BlockConnector
{
public:
    using ActivateActiveFn = std::function<void(std::shared_ptr<const CBlock>)>;
    using ActivateHistoricalFn = std::function<void()>;
    using IsInterruptedFn = std::function<bool()>;

    BlockConnector(ActivateActiveFn activate_active,
                   ActivateHistoricalFn activate_historical,
                   IsInterruptedFn is_interrupted);
    ~BlockConnector();

    BlockConnector(const BlockConnector&) = delete;
    BlockConnector& operator=(const BlockConnector&) = delete;

    /** Hand a newly stored block to the connector thread.
     *  `for_active` is false for blocks targeting only a historical assumeutxo chainstate. */
    void Signal(std::shared_ptr<const CBlock> block, bool for_active) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Ticket covering all active-chain hand-offs signaled so far. */
    HandOffTicket Latest() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** True once the active-chain pass covering `ticket` has completed, or after Stop(). */
    bool Done(const HandOffTicket& ticket) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Lock-free check whether any active-chain hand-off has not yet been attempted. */
    bool AnyActivePending() const noexcept { return m_active_pending.load(std::memory_order_acquire); }

    /** True if called from the background connector thread itself. */
    bool IsConnectorThread() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Wait until all active-chain hand-offs signaled so far have been attempted (or Stop() finishes). */
    void WaitForPending() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Register an optional callback invoked after each active-chain activation pass completes. */
    void SetOnActiveDone(std::function<void()> cb) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Join the connector thread when idle (e.g. on IBD exit) while allowing future Signal() calls to restart it. */
    void StopForIdle() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Permanently stop accepting new hand-offs and join the connector thread. Idempotent and thread-safe. */
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    enum class State {
        Idle,
        Running,
        DrainingForIdle,
        DrainingForShutdown,
        Shutdown,
    };

    void UpdatePendingLocked() EXCLUSIVE_LOCKS_REQUIRED(m_mutex)
    {
        m_active_pending.store(m_state == State::Running && m_active_attempted < m_active_handed,
                               std::memory_order_release);
    }

    void Loop();

    ActivateActiveFn m_activate_active;
    ActivateHistoricalFn m_activate_historical;
    IsInterruptedFn m_is_interrupted;

    mutable Mutex m_mutex;
    std::condition_variable m_cv;
    State m_state GUARDED_BY(m_mutex){State::Idle};
    uint64_t m_any_handed GUARDED_BY(m_mutex){0};
    uint64_t m_any_taken GUARDED_BY(m_mutex){0};
    uint64_t m_active_handed GUARDED_BY(m_mutex){0};
    uint64_t m_active_attempted GUARDED_BY(m_mutex){0};
    std::atomic<bool> m_active_pending{false};
    std::shared_ptr<const CBlock> m_latest_block GUARDED_BY(m_mutex);
    std::function<void()> m_on_active_done GUARDED_BY(m_mutex);
    std::thread m_thread GUARDED_BY(m_mutex);
    std::thread::id m_thread_id GUARDED_BY(m_mutex){};
};

} // namespace node

#endif // BITCOIN_NODE_BLOCK_CONNECTOR_H
