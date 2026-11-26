// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_EFFECTS_H
#define BITCOIN_KERNEL_EFFECTS_H

#include <sync.h>
#include <util/byte_ledger.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <thread>
#include <utility>

namespace kernel {

/**
 * Runs ordered I/O effects (such as block undo writes) on a single worker thread and queues their
 * completions to be applied in FIFO order under the requester's lock.
 *
 * The worker never blocks on the requester's lock. The optional `on_completion` callback may try
 * the requester's lock non-blockingly and apply completions; anything it cannot apply is left for
 * ApplyCompletions(). WaitForIO() waits only for the I/O half and is therefore safe to call with
 * the requester's lock held.
 *
 * When constructed with `threaded = false` (e.g. libbitcoinkernel with zero worker threads), no
 * background thread is spawned and Enqueue() runs effects synchronously inline while preserving
 * FIFO completion delivery via ApplyCompletions().
 */
class EffectsExecutor
{
public:
    using Effect = std::function<bool()>;
    using Completion = std::function<void(bool ok)>;

    explicit EffectsExecutor(util::ByteAccount& backlog,
                             std::function<void()> on_completion = {},
                             bool threaded = true);
    ~EffectsExecutor() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    EffectsExecutor(const EffectsExecutor&) = delete;
    EffectsExecutor& operator=(const EffectsExecutor&) = delete;

    //! Start the worker thread (no-op if threaded == false). Must not be called twice without Stop().
    void Start() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Drain every enqueued effect's I/O and join the worker thread. Idempotent and safe for concurrent callers.
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Queue an effect and its completion. In threaded mode, the executor must be running.
    void Enqueue(Effect effect, Completion completion, size_t bytes = 0) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Wait until every enqueued effect's I/O has finished. Safe with the requester's lock held.
    void WaitForIO() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Wait until queued I/O bytes are at or below `limit_bytes`.
    void WaitForBacklogBelow(size_t limit_bytes) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Wait until a completion is ready, all I/O is idle, or `enough()` holds.
    void WaitForCompletionOr(const std::function<bool()>& enough) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Apply finished completions in FIFO order. Caller holds the lock required by completions.
    void ApplyCompletions() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    size_t PendingBytes() const;
    bool HasPendingCompletions() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool Idle() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    enum class State : uint8_t { Stopped, Running, Draining };

    struct Item {
        Effect effect;
        Completion completion;
        size_t bytes;
    };

    void Loop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    const bool m_threaded;
    mutable Mutex m_mutex;
    std::condition_variable m_cv_work;
    std::condition_variable m_cv_done;
    std::deque<Item> m_queue GUARDED_BY(m_mutex);
    std::deque<std::pair<Completion, bool>> m_done GUARDED_BY(m_mutex);
    util::ByteAccount& m_backlog;
    bool m_io_running GUARDED_BY(m_mutex){false};
    State m_state GUARDED_BY(m_mutex){State::Stopped};
    std::thread m_thread;
    const std::function<void()> m_on_completion;
};

} // namespace kernel

#endif // BITCOIN_KERNEL_EFFECTS_H
