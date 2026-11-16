// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_WORKER_TASK_H
#define BITCOIN_UTIL_WORKER_TASK_H

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace util {
/**
 * What a std::runtime_error means when it leaves a task that runs on a
 * worker thread, where there is no caller on the stack to decide. This is
 * for the tasks run through RunWorkerTask(): a task that expects an error
 * of its own handles that one itself, and lets no other leave it.
 */
enum class WorkerError {
    //! The block the task works on fails with an error state; the node goes on.
    FAILS_BLOCK,
    //! The node cannot go on (a failed read or write of its own data); the
    //! owner of the task raises a fatal error.
    STOPS_NODE,
};

/** A kind of worker task: its name for the log, and what its failure means. */
struct WorkerTaskType {
    std::string_view name;
    WorkerError on_runtime_error;
};

/** A worker task's failure as a value, for the thread that owns the task to act on. */
struct WorkerTaskFailure {
    WorkerError meaning;
    std::string what;
};

/**
 * Run a task of the given type on the calling (worker) thread.
 *
 * A std::runtime_error, or a class derived from it such as a failed read
 * of a file, is returned as a value together with what the type says it
 * means. Nothing wider is caught: a logic error is a bug and a failed
 * allocation has no handler anywhere in the node, and both terminate.
 */
template <typename Task>
[[nodiscard]] std::optional<WorkerTaskFailure> RunWorkerTask(const WorkerTaskType& type, Task&& task)
{
    try {
        std::forward<Task>(task)();
    } catch (const std::runtime_error& e) {
        return WorkerTaskFailure{type.on_runtime_error, std::string{type.name} + ": " + e.what()};
    }
    return std::nullopt;
}
} // namespace util

#endif // BITCOIN_UTIL_WORKER_TASK_H
