// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <ipc/capnp/event_loop.h>

#include <logging.h>

namespace ipc {
namespace capnp {

std::shared_ptr<EventLoopDispatcher> EventLoopDispatcher::CurrentThread()
{
    return std::make_shared<EventLoopDispatcher>(kj::getCurrentThreadExecutor().addRef(), std::this_thread::get_id());
}

EventLoopDispatcher::EventLoopDispatcher(kj::Own<const kj::Executor> executor, std::thread::id thread_id)
    : m_executor{kj::mv(executor)}, m_thread_id{thread_id}
{
}

void EventLoopDispatcher::execute(kj::Function<void()> fn) const
{
    if (std::this_thread::get_id() == m_thread_id) {
        fn();
        return;
    }

    try {
        m_executor->executeSync([&fn] { fn(); });
    } catch (const kj::Exception& e) {
        LogDebug(BCLog::IPC, "IPC event-loop dispatch failed: %s", e.getDescription().cStr());
    }
}

} // namespace capnp
} // namespace ipc
