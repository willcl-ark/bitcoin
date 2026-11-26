// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/block_connector.h>

#include <util/thread.h>

#include <algorithm>
#include <utility>

namespace node {

BlockConnector::BlockConnector(ActivateActiveFn activate_active,
                               ActivateHistoricalFn activate_historical,
                               IsInterruptedFn is_interrupted)
    : m_activate_active{std::move(activate_active)},
      m_activate_historical{std::move(activate_historical)},
      m_is_interrupted{std::move(is_interrupted)}
{
}

BlockConnector::~BlockConnector()
{
    Stop();
}

void BlockConnector::Signal(std::shared_ptr<const CBlock> block, bool for_active)
{
    {
        WAIT_LOCK(m_mutex, lock);
        m_cv.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
            return m_state != State::DrainingForIdle;
        });
        if (m_state == State::Shutdown || m_state == State::DrainingForShutdown) return;
        if (m_state == State::Idle) {
            m_state = State::Running;
            try {
                m_thread = std::thread(&util::TraceThread, "blockconn", std::function<void()>{[this] { Loop(); }});
                m_thread_id = m_thread.get_id();
            } catch (...) {
                m_state = State::Idle;
                m_thread_id = {};
                UpdatePendingLocked();
                throw;
            }
        }
        ++m_any_handed;
        if (for_active) ++m_active_handed;
        if (block) m_latest_block = std::move(block);
        UpdatePendingLocked();
    }
    m_cv.notify_all();
}

HandOffTicket BlockConnector::Latest() const
{
    LOCK(m_mutex);
    return {m_active_handed};
}

bool BlockConnector::Done(const HandOffTicket& ticket) const
{
    LOCK(m_mutex);
    return m_state == State::Shutdown || m_active_attempted >= ticket.value;
}

bool BlockConnector::IsConnectorThread() const
{
    LOCK(m_mutex);
    return m_thread_id != std::thread::id{} && m_thread_id == std::this_thread::get_id();
}

void BlockConnector::WaitForPending()
{
    AssertMayWait("the connector");
    WAIT_LOCK(m_mutex, lock);
    const uint64_t target{m_active_handed};
    m_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
        return m_state == State::Shutdown || m_active_attempted >= target;
    });
}

void BlockConnector::SetOnActiveDone(std::function<void()> cb)
{
    LOCK(m_mutex);
    m_on_active_done = std::move(cb);
}

void BlockConnector::StopForIdle()
{
    std::thread to_join;
    {
        WAIT_LOCK(m_mutex, lock);
        if (m_state == State::Idle || m_state == State::Shutdown || m_state == State::DrainingForShutdown) return;
        if (m_state == State::DrainingForIdle) {
            AssertMayWait("the connector");
            m_cv.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
                return m_state != State::DrainingForIdle;
            });
            return;
        }
        m_state = State::DrainingForIdle;
        to_join = std::move(m_thread);
        UpdatePendingLocked();
    }
    m_cv.notify_all();
    if (to_join.joinable()) {
        AssertMayWait("the connector");
        to_join.join();
    }
    {
        LOCK(m_mutex);
        m_thread_id = {};
        m_latest_block.reset();
        m_active_attempted = std::max(m_active_attempted, m_active_handed);
        if (m_state == State::DrainingForIdle) {
            m_state = State::Idle;
        }
        UpdatePendingLocked();
    }
    m_cv.notify_all();
}

void BlockConnector::Stop()
{
    std::thread to_join;
    {
        WAIT_LOCK(m_mutex, lock);
        m_cv.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
            return m_state != State::DrainingForIdle;
        });
        if (m_state == State::Shutdown) return;
        if (m_state == State::DrainingForShutdown) {
            AssertMayWait("the connector");
            m_cv.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
                return m_state == State::Shutdown;
            });
            return;
        }
        m_state = State::DrainingForShutdown;
        to_join = std::move(m_thread);
        UpdatePendingLocked();
    }
    m_cv.notify_all();
    if (to_join.joinable()) {
        AssertMayWait("the connector");
        to_join.join();
    }
    {
        LOCK(m_mutex);
        m_thread_id = {};
        m_latest_block.reset();
        m_state = State::Shutdown;
        UpdatePendingLocked();
    }
    m_cv.notify_all();
}

void BlockConnector::Loop()
{
    while (true) {
        std::shared_ptr<const CBlock> block;
        uint64_t taken_active{0};
        {
            WAIT_LOCK(m_mutex, lock);
            m_cv.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
                return m_state != State::Running || m_any_taken < m_any_handed;
            });
            if (m_state != State::Running) return;
            m_any_taken = m_any_handed;
            taken_active = m_active_handed;
            block = std::exchange(m_latest_block, nullptr);
        }
        if (m_is_interrupted()) {
            {
                LOCK(m_mutex);
                m_active_attempted = std::max(m_active_attempted, taken_active);
                UpdatePendingLocked();
            }
            m_cv.notify_all();
            continue;
        }
        m_activate_active(std::move(block));
        std::function<void()> on_done;
        {
            LOCK(m_mutex);
            m_active_attempted = std::max(m_active_attempted, taken_active);
            on_done = m_on_active_done;
            UpdatePendingLocked();
        }
        m_cv.notify_all();
        if (on_done) on_done();
        m_activate_historical();
    }
}

} // namespace node
