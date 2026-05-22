// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_IPC_CAPNP_EVENT_LOOP_H
#define BITCOIN_IPC_CAPNP_EVENT_LOOP_H

#include <kj/async.h>
#include <kj/common.h>
#include <kj/function.h>

#include <memory>
#include <thread>

namespace ipc {
namespace capnp {

//! Dispatcher for Cap'n Proto I/O bound to one KJ event-loop thread.
//!
//! Bitcoin code can run on many threads, but Cap'n Proto clients and promises
//! must be used on the event-loop thread that owns them. This class provides a
//! narrow marshalling point for short I/O callbacks. Blocking Bitcoin work
//! should use WorkerQueue instead.
class EventLoopDispatcher
{
public:
    //! Create a dispatcher for the current thread's KJ event loop.
    static std::shared_ptr<EventLoopDispatcher> CurrentThread();

    EventLoopDispatcher(kj::Own<const kj::Executor> executor, std::thread::id thread_id);

    EventLoopDispatcher(const EventLoopDispatcher&) = delete;
    EventLoopDispatcher& operator=(const EventLoopDispatcher&) = delete;

    //! Execute fn on the event-loop thread. If called from another thread, this
    //! blocks only until fn has run on the event-loop thread.
    void execute(kj::Function<void()> fn) const;

private:
    kj::Own<const kj::Executor> m_executor;
    const std::thread::id m_thread_id;
};

} // namespace capnp
} // namespace ipc

#endif // BITCOIN_IPC_CAPNP_EVENT_LOOP_H
