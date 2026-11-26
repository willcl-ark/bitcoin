// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/effects.h>

#include <sync.h>
#include <util/check.h>
#include <util/threadnames.h>

#include <chrono>
#include <exception>

namespace kernel {

EffectsExecutor::EffectsExecutor(util::ByteAccount& backlog, std::function<void()> on_completion, bool threaded)
    : m_threaded{threaded}, m_backlog{backlog}, m_on_completion{std::move(on_completion)}
{
}

EffectsExecutor::~EffectsExecutor()
{
    Stop();
}

void EffectsExecutor::Start()
{
    if (!m_threaded) return;
    LOCK(m_mutex);
    Assert(m_state == State::Stopped);
    Assert(!m_thread.joinable());
    m_state = State::Running;
    try {
        m_thread = std::thread([this] { Loop(); });
    } catch (...) {
        m_state = State::Stopped;
        throw;
    }
}

void EffectsExecutor::Stop()
{
    if (!m_threaded) return;
    std::thread to_join;
    {
        WAIT_LOCK(m_mutex, lock);
        if (m_state == State::Stopped) return;
        if (m_state == State::Draining) {
            m_cv_done.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_state == State::Stopped; });
            return;
        }
        m_state = State::Draining;
        to_join = std::move(m_thread);
    }
    m_cv_work.notify_all();
    if (to_join.joinable()) to_join.join();
    {
        LOCK(m_mutex);
        m_state = State::Stopped;
    }
    m_cv_done.notify_all();
}

void EffectsExecutor::Enqueue(Effect effect, Completion completion, size_t bytes)
{
    if (!m_threaded) {
        bool ok{true};
        if (effect) ok = effect();
        {
            LOCK(m_mutex);
            m_done.emplace_back(std::move(completion), ok);
        }
        if (m_on_completion) m_on_completion();
        m_cv_done.notify_all();
        return;
    }
    {
        LOCK(m_mutex);
        Assert(m_state == State::Running);
        m_queue.push_back(Item{std::move(effect), std::move(completion), bytes});
        m_backlog.Add(bytes);
    }
    m_cv_work.notify_one();
}

void EffectsExecutor::WaitForIO()
{
    if (!m_threaded) return;
    WAIT_LOCK(m_mutex, lock);
    m_cv_done.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
        return m_queue.empty() && !m_io_running;
    });
}

void EffectsExecutor::WaitForBacklogBelow(size_t limit_bytes)
{
    if (!m_threaded) return;
    WAIT_LOCK(m_mutex, lock);
    m_cv_done.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
        return m_backlog.Bytes() <= limit_bytes;
    });
}

void EffectsExecutor::WaitForCompletionOr(const std::function<bool()>& enough)
{
    WAIT_LOCK(m_mutex, lock);
    m_cv_done.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
        return !m_done.empty() || (m_queue.empty() && !m_io_running) || enough();
    });
}

void EffectsExecutor::ApplyCompletions()
{
    bool applied{false};
    while (true) {
        std::pair<Completion, bool> done;
        {
            LOCK(m_mutex);
            if (m_done.empty()) break;
            done = std::move(m_done.front());
            m_done.pop_front();
        }
        if (done.first) done.first(done.second);
        applied = true;
    }
    if (applied) {
        WITH_LOCK(m_mutex, m_cv_done.notify_all());
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
    while (true) {
        Item item;
        {
            WAIT_LOCK(m_mutex, lock);
            while (m_state == State::Running && m_queue.empty()) {
                if (!m_done.empty() && m_on_completion) {
                    m_cv_work.wait_for(lock, std::chrono::milliseconds{10});
                    if (m_state == State::Running && m_queue.empty() && !m_done.empty()) {
                        REVERSE_LOCK(lock, m_mutex);
                        m_on_completion();
                    }
                } else {
                    m_cv_work.wait(lock);
                }
            }
            if (m_queue.empty()) return;
            item = std::move(m_queue.front());
            m_queue.pop_front();
            m_io_running = true;
        }
        bool ok{true};
        if (item.effect) {
            WAITED_FOR_UNDER_LOCK("an effect's I/O");
            try {
                ok = item.effect();
            } catch (const std::exception&) {
                ok = false;
            }
        }
        {
            LOCK(m_mutex);
            m_done.emplace_back(std::move(item.completion), ok);
            m_backlog.Sub(item.bytes);
            m_io_running = false;
        }
        if (m_on_completion) m_on_completion();
        m_cv_done.notify_all();
    }
}

} // namespace kernel
