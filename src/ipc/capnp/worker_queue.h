// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_IPC_CAPNP_WORKER_QUEUE_H
#define BITCOIN_IPC_CAPNP_WORKER_QUEUE_H

#include <kj/async.h>
#include <kj/common.h>
#include <kj/function.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace ipc {
namespace capnp {

//! Serialized worker queue for blocking IPC server calls.
//!
//! The queue runs posted work on one worker thread and returns KJ promises that
//! resolve on the Cap'n Proto event-loop thread that called post().
class WorkerQueue
{
public:
    explicit WorkerQueue(std::string thread_name);
    ~WorkerQueue();

    WorkerQueue(const WorkerQueue&) = delete;
    WorkerQueue& operator=(const WorkerQueue&) = delete;

    template <typename Fn>
    auto post(Fn&& fn)
    {
        using Callable = std::decay_t<Fn>;
        using Result = std::invoke_result_t<Callable&>;

        auto pair{kj::newPromiseAndCrossThreadFulfiller<Result>()};
        enqueue([callable = Callable{std::forward<Fn>(fn)}, fulfiller = kj::mv(pair.fulfiller)]() mutable {
            if (!fulfiller->isWaiting()) return;

            if constexpr (std::is_void_v<Result>) {
                kj::Maybe<kj::Exception> exception{kj::runCatchingExceptions([&] { callable(); })};
                KJ_IF_MAYBE(e, exception) {
                    fulfiller->reject(kj::mv(*e));
                } else {
                    fulfiller->fulfill();
                }
            } else {
                std::optional<Result> result;
                kj::Maybe<kj::Exception> exception{kj::runCatchingExceptions([&] { result.emplace(callable()); })};
                KJ_IF_MAYBE(e, exception) {
                    fulfiller->reject(kj::mv(*e));
                } else {
                    fulfiller->fulfill(std::move(*result));
                }
            }
        });
        return kj::mv(pair.promise);
    }

private:
    void enqueue(kj::Function<void()> fn);
    void run();

    const std::string m_thread_name;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<kj::Function<void()>> m_queue;
    bool m_stop{false};
    std::thread m_thread;
};

} // namespace capnp
} // namespace ipc

#endif // BITCOIN_IPC_CAPNP_WORKER_QUEUE_H
