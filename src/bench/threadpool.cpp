// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <util/threadpool.h>

#include <chrono>
#include <functional>
#include <future>
#include <thread>
#include <vector>

// The pool's hand-off costs. Wall time per operation is what nanobench prints; the CPU these operations burn,
// which is where waking idle workers shows, is the user and system time of the bench process, so run these
// under `time` as well. Workers that idle longer than a few hundred microseconds have gone to sleep on the
// condition variable, as they do between two blocks, which is what the pause before each submission models.
// The pause is inside the timed region: ThreadPoolPauseOnly times it alone, and what waking the pool costs is
// an idle bench's time less that.

namespace {

constexpr int WORKERS{8};
constexpr auto PAUSE{std::chrono::microseconds{300}};

//! Sleeping rather than spinning, so that the process's CPU time is the pool's and not the pause's.
void Pause()
{
    std::this_thread::sleep_for(PAUSE);
}

//! The pause alone, with no pool: the baseline of the two benches after it.
void ThreadPoolPauseOnly(benchmark::Bench& bench)
{
    bench.unit("pause").run([&] { Pause(); });
}

//! A couple of tasks on a pool with many workers, after the workers went idle: a small block's prevout
//! fetching. The pool wakes every worker; this measures what that costs the tasks' latency.
void ThreadPoolFewTasksAfterIdle(benchmark::Bench& bench)
{
    ThreadPool pool{"bench"};
    pool.Start(WORKERS);
    bench.unit("submit").run([&] {
        Pause();
        std::vector<std::function<void()>> tasks(2, [] {});
        auto futures{pool.Submit(std::move(tasks))};
        for (auto& f : *futures) f.wait();
    });
}

//! One task per worker, after the workers went idle: a large block's prevout fetching.
void ThreadPoolAllWorkersAfterIdle(benchmark::Bench& bench)
{
    ThreadPool pool{"bench"};
    pool.Start(WORKERS);
    bench.unit("submit").run([&] {
        Pause();
        std::vector<std::function<void()>> tasks(WORKERS, [] {});
        auto futures{pool.Submit(std::move(tasks))};
        for (auto& f : *futures) f.wait();
    });
}

//! Single tasks back to back, the workers never idle: a window's blocks handed to the pool as they are staged.
void ThreadPoolSingleTasksHot(benchmark::Bench& bench)
{
    ThreadPool pool{"bench"};
    pool.Start(WORKERS);
    bench.unit("submit").run([&] {
        auto future{pool.Submit([] {})};
        future->wait();
    });
}

} // namespace

BENCHMARK(ThreadPoolPauseOnly);
BENCHMARK(ThreadPoolFewTasksAfterIdle);
BENCHMARK(ThreadPoolAllWorkersAfterIdle);
BENCHMARK(ThreadPoolSingleTasksHot);
