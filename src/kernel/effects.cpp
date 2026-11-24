// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/effects.h>

#include <chrono>
#include <sync.h>
#include <util/check.h>
#include <util/threadnames.h>

namespace kernel {

EffectsExecutor::EffectsExecutor(util::ByteAccount& backlog, std::function<void()> on_completion, bool inline_mode)
    : m_backlog{backlog}, m_on_completion{std::move(on_completion)}, m_inline{inline_mode} {}

EffectsExecutor::~EffectsExecutor()
{
    Stop();
}

void EffectsExecutor::Start()
{
    // The thread first: if it cannot be started the executor never ran, and Stop() has nothing to join. The
    // loop takes the mutex before it reads the lifecycle.
    LOCK(m_mutex);
    if (!m_inline) {
        m_thread = std::thread([this] { Loop(); });
    }
    m_life.Start();
}

void EffectsExecutor::Stop()
{
    // Idempotent: a second concurrent Stop() waits until the worker exits and lifecycle reaches Stopped.
    {
        WAIT_LOCK(m_mutex, lock);
        if (!m_life.DrainIfRunning()) {
            m_cv_done.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_life.Stopped(); });
            return;
        }
    }
    if (m_inline) {
        while (RunOneInline()) {}
    } else {
        m_cv_work.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }
    {
        LOCK(m_mutex);
        m_life.Finish();
    }
    m_cv_done.notify_all();
}

void EffectsExecutor::Enqueue(Effect effect, Completion completion, size_t bytes)
{
    {
        LOCK(m_mutex);
        // An effect queued before the start or behind the stop would wait forever.
        Assert(m_life.Running());
        m_queue.push_back(Item{std::move(effect), std::move(completion), bytes});
        m_backlog.Add(bytes);
    }
    if (!m_inline) m_cv_work.notify_one();
}

bool EffectsExecutor::RunOneInline()
{
    Item item;
    {
        WAIT_LOCK(m_mutex, lock);
        m_cv_done.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return !m_io_running; });
        if (m_queue.empty()) return false;
        item = std::move(m_queue.front());
        m_queue.pop_front();
        m_io_running = true;
    }
    bool ok{true};
    if (item.effect) {
        WAITED_FOR_UNDER_LOCK("an effect's I/O");
        ok = item.effect();
    }
    {
        LOCK(m_mutex);
        m_done.emplace_back(std::move(item.completion), ok);
        m_backlog.Sub(item.bytes);
        m_io_running = false;
    }
    m_cv_done.notify_all();
    return true;
}

void EffectsExecutor::WaitForIO()
{
    if (m_inline) {
        while (RunOneInline()) {}
        return;
    }
    WAIT_LOCK(m_mutex, lock);
    m_cv_done.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_queue.empty() && !m_io_running; });
}

void EffectsExecutor::WaitForBacklogBelow(size_t limit_bytes)
{
    if (m_inline) {
        while (m_backlog.Bytes() > limit_bytes && RunOneInline()) {}
        return;
    }
    WAIT_LOCK(m_mutex, lock);
    m_cv_done.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_backlog.Bytes() <= limit_bytes; });
}

void EffectsExecutor::WaitForCompletionOr(const std::function<bool()>& enough)
{
    if (m_inline) {
        while (true) {
            {
                LOCK(m_mutex);
                if (!m_done.empty() || (m_queue.empty() && !m_io_running) || enough()) return;
            }
            if (!RunOneInline()) return;
        }
    }
    WAIT_LOCK(m_mutex, lock);
    m_cv_done.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
        return !m_done.empty() || (m_queue.empty() && !m_io_running) || enough();
    });
}

void EffectsExecutor::ApplyCompletions()
{
    const auto self{std::this_thread::get_id()};
    {
        WAIT_LOCK(m_mutex, lock);
        // Re-entrant call on the same thread (e.g. recursive cs_main + ImmediateTaskRunner):
        // return immediately so completions stay strictly in FIFO order.
        if (m_applying_thread == self) return;
        m_cv_done.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return !m_applying_thread.has_value(); });
        if (m_done.empty()) return;
        m_applying_thread = self;
    }
    struct Guard {
        EffectsExecutor& exec;
        bool applied{false};
        ~Guard()
        {
            {
                LOCK(exec.m_mutex);
                exec.m_applying_thread.reset();
            }
            exec.m_cv_done.notify_all();
        }
    } guard{*this};
    while (true) {
        std::pair<Completion, bool> done;
        {
            LOCK(m_mutex);
            if (m_done.empty()) break;
            done = std::move(m_done.front());
            m_done.pop_front();
        }
        if (done.first) done.first(done.second);
        guard.applied = true;
    }
}

size_t EffectsExecutor::PendingBytes() const
{
    return m_backlog.Bytes();
}

bool EffectsExecutor::HasPendingCompletions() const
{
    LOCK(m_mutex);
    return !m_done.empty();
}

bool EffectsExecutor::Idle() const
{
    LOCK(m_mutex);
    return m_queue.empty() && !m_io_running && m_done.empty();
}

void EffectsExecutor::Loop()
{
    util::ThreadRename("effects");
    static constexpr auto MIN_RETRY_DELAY{std::chrono::milliseconds{10}};
    static constexpr auto MAX_RETRY_DELAY{std::chrono::milliseconds{100}};
    auto retry_delay{MIN_RETRY_DELAY};
    while (true) {
        Item item;
        {
            WAIT_LOCK(m_mutex, lock);
            while (m_life.Running() && m_queue.empty()) {
                if (!m_done.empty() && m_on_completion) {
                    // Completions are waiting for the requester's lock, which may be free by now: retry with
                    // capped backoff rather than spinning at a fixed 10ms during long cs_main holds.
                    m_cv_work.wait_for(lock, retry_delay);
                    if (m_life.Running() && m_queue.empty() && !m_done.empty()) {
                        REVERSE_LOCK(lock, m_mutex);
                        m_on_completion();
                    }
                    retry_delay = std::min(retry_delay * 2, MAX_RETRY_DELAY);
                } else {
                    retry_delay = MIN_RETRY_DELAY;
                    m_cv_work.wait(lock);
                }
            }
            if (m_queue.empty()) return; // Draining, and every effect's I/O has run
            retry_delay = MIN_RETRY_DELAY;
            item = std::move(m_queue.front());
            m_queue.pop_front();
            m_io_running = true;
        }
        bool ok{true};
        if (item.effect) {
            // WaitForIO() is called with cs_main held (Chainstate::SettleWriteOut()).
            WAITED_FOR_UNDER_LOCK("an effect's I/O");
            ok = item.effect();
        }
        {
            LOCK(m_mutex);
            m_done.emplace_back(std::move(item.completion), ok);
            m_backlog.Sub(item.bytes);
            m_io_running = false;
        }
        m_cv_done.notify_all();
        if (m_on_completion) m_on_completion();
    }
}

} // namespace kernel
