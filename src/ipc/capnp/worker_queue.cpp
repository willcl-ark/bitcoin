// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <ipc/capnp/worker_queue.h>

#include <util/threadnames.h>

namespace ipc {
namespace capnp {

WorkerQueue::WorkerQueue(std::string thread_name)
    : m_thread_name{std::move(thread_name)}, m_thread{&WorkerQueue::run, this}
{
}

WorkerQueue::~WorkerQueue()
{
    {
        std::lock_guard lock{m_mutex};
        m_stop = true;
        m_queue.clear();
    }
    m_cv.notify_one();
    if (m_thread.joinable()) m_thread.join();
}

void WorkerQueue::enqueue(kj::Function<void()> fn)
{
    {
        std::lock_guard lock{m_mutex};
        if (m_stop) return;
        m_queue.push_back(kj::mv(fn));
    }
    m_cv.notify_one();
}

void WorkerQueue::run()
{
    util::ThreadRename(m_thread_name);
    while (true) {
        kj::Function<void()> fn;
        {
            std::unique_lock lock{m_mutex};
            m_cv.wait(lock, [&] { return m_stop || !m_queue.empty(); });
            if (m_queue.empty()) return;
            fn = kj::mv(m_queue.front());
            m_queue.pop_front();
        }
        fn();
    }
}

} // namespace capnp
} // namespace ipc
