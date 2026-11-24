// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/effects.h>
#include <sync.h>
#include <util/byte_ledger.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using kernel::EffectsExecutor;

BOOST_AUTO_TEST_SUITE(effects_tests)

BOOST_AUTO_TEST_CASE(effects_run_and_complete_in_order)
{
    std::mutex m;
    std::vector<int> ran;
    std::vector<int> completed;
    util::ByteAccount backlog;
    EffectsExecutor exec{backlog};
    exec.Start();
    for (int i = 0; i < 64; ++i) {
        exec.Enqueue(
            [&, i] {
                // Vary the I/O time so a reordering executor would show it.
                if (i % 7 == 0) std::this_thread::sleep_for(std::chrono::milliseconds{2});
                std::lock_guard<std::mutex> l{m};
                ran.push_back(i);
                return i % 5 != 0; // some effects fail
            },
            [&, i](bool ok) {
                BOOST_CHECK_EQUAL(ok, i % 5 != 0);
                completed.push_back(i);
            },
            /*bytes=*/1);
    }
    exec.WaitForIO();
    BOOST_CHECK_EQUAL(exec.PendingBytes(), 0U);
    {
        std::lock_guard<std::mutex> l{m};
        BOOST_REQUIRE_EQUAL(ran.size(), 64U);
        for (int i = 0; i < 64; ++i) BOOST_CHECK_EQUAL(ran[i], i);
    }
    // Completions are applied by the caller, in order, and only once: none has run yet (no hook applies them).
    BOOST_CHECK(exec.HasPendingCompletions());
    BOOST_CHECK(completed.empty());
    exec.ApplyCompletions();
    BOOST_REQUIRE_EQUAL(completed.size(), 64U);
    for (int i = 0; i < 64; ++i) BOOST_CHECK_EQUAL(completed[i], i);
    exec.ApplyCompletions();
    BOOST_CHECK_EQUAL(completed.size(), 64U);
    exec.Stop();
}

BOOST_AUTO_TEST_CASE(effects_backlog_and_stop_drain)
{
    std::atomic<int> ran{0};
    std::atomic<bool> release{false};
    util::ByteAccount backlog;
    EffectsExecutor exec{backlog};
    exec.Start();
    // The first effect blocks until released, holding everything behind it.
    exec.Enqueue([&] { while (!release) std::this_thread::sleep_for(std::chrono::milliseconds{1}); ++ran; return true; }, {}, 10);
    for (int i = 0; i < 9; ++i) exec.Enqueue([&] { ++ran; return true; }, {}, 10);
    BOOST_CHECK_EQUAL(exec.PendingBytes(), 100U);
    BOOST_CHECK_EQUAL(backlog.Bytes(), 100U);

    std::thread waiter{[&] { exec.WaitForBacklogBelow(50); }};
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    BOOST_CHECK_EQUAL(ran.load(), 0);
    release = true;
    waiter.join();
    BOOST_CHECK_LE(exec.PendingBytes(), 50U);

    // Stop runs every remaining effect before returning.
    exec.Stop();
    BOOST_CHECK_EQUAL(ran.load(), 10);
    BOOST_CHECK_EQUAL(exec.PendingBytes(), 0U);
    BOOST_CHECK_EQUAL(backlog.Bytes(), 0U);
    exec.Stop(); // idempotent
}

BOOST_AUTO_TEST_CASE(effects_completion_hook_runs_after_each_effect)
{
    std::atomic<int> hooked{0};
    std::vector<int> completed;
    util::ByteAccount backlog;
    EffectsExecutor exec{backlog, [&] { ++hooked; }};
    exec.Start();
    for (int i = 0; i < 5; ++i) exec.Enqueue([] { return true; }, [&, i](bool) { completed.push_back(i); });
    exec.WaitForIO();
    // The hook ran, but the completions themselves are applied only by ApplyCompletions(). The count
    // says little here: the hook is also called again every few milliseconds while completions wait,
    // which alone reaches it (the case below tells the two apart). The executor wakes the waiter
    // before it calls the hook for the last effect, so that call is waited for.
    // Bounded, so that a hook that never runs fails the case instead of hanging the suite.
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{60}};
    while (hooked.load() < 5 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    BOOST_REQUIRE_MESSAGE(hooked.load() >= 5, "the completion hook did not run five times");
    BOOST_CHECK(completed.empty());
    exec.ApplyCompletions();
    BOOST_CHECK_EQUAL(completed.size(), 5U);
}

//! A hook that applies the completions, as the block manager's does when the lock is free, leaves none
//! waiting, so that nothing calls it a second time for an effect: once for each, no more and no fewer.
BOOST_AUTO_TEST_CASE(effects_completion_hook_runs_once_per_effect)
{
    std::atomic<int> hooked{0};
    std::vector<int> completed;
    EffectsExecutor* self{nullptr};
    util::ByteAccount backlog;
    EffectsExecutor exec{backlog, [&] {
        ++hooked;
        self->ApplyCompletions();
    }};
    self = &exec;
    exec.Start();
    for (int i = 0; i < 5; ++i) exec.Enqueue([] { return true; }, [&, i](bool) { completed.push_back(i); });
    exec.WaitForIO();
    // Joins the executor's thread: the call for the last effect has returned.
    exec.Stop();
    BOOST_CHECK_EQUAL(hooked.load(), 5);
    BOOST_CHECK_EQUAL(completed.size(), 5U);
}

BOOST_AUTO_TEST_CASE(effects_inline_mode_runs_on_caller_thread)
{
    const auto caller_id{std::this_thread::get_id()};
    std::vector<int> events;
    util::ByteAccount backlog;
    EffectsExecutor exec{backlog, {}, /*inline_mode=*/true};
    exec.Start();
    for (int i = 0; i < 4; ++i) {
        exec.Enqueue(
            [&, i] {
                BOOST_CHECK(std::this_thread::get_id() == caller_id);
                events.push_back(i * 2);
                return true;
            },
            [&, i](bool ok) {
                BOOST_CHECK(ok);
                BOOST_CHECK(std::this_thread::get_id() == caller_id);
                events.push_back(i * 2 + 1);
            },
            /*bytes=*/10);
    }
    BOOST_CHECK_EQUAL(exec.PendingBytes(), 40U);
    exec.WaitForBacklogBelow(20);
    BOOST_CHECK_EQUAL(exec.PendingBytes(), 20U);
    exec.WaitForIO();
    BOOST_CHECK_EQUAL(exec.PendingBytes(), 0U);
    exec.ApplyCompletions();
    BOOST_CHECK((events == std::vector<int>{0, 2, 4, 6, 1, 3, 5, 7}));
    exec.Stop();
}

BOOST_AUTO_TEST_CASE(effects_reentrant_apply_preserves_order)
{
    std::vector<int> order;
    util::ByteAccount backlog;
    EffectsExecutor exec{backlog, {}, /*inline_mode=*/true};
    exec.Start();
    exec.Enqueue([] { return true; }, [&](bool) {
        order.push_back(1);
        // Re-entrant ApplyCompletions() must not pull item 2 ahead of item 1's remainder.
        exec.ApplyCompletions();
        order.push_back(2);
    });
    exec.Enqueue([] { return true; }, [&](bool) {
        order.push_back(3);
    });
    exec.WaitForIO();
    exec.ApplyCompletions();
    BOOST_CHECK((order == std::vector<int>{1, 2, 3}));
    exec.Stop();
}

BOOST_AUTO_TEST_CASE(effects_concurrent_stop_waits_for_worker_exit)
{
    std::atomic<bool> release{false};
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    util::ByteAccount backlog;
    EffectsExecutor exec{backlog};
    exec.Start();
    exec.Enqueue([&] {
        started.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds{1});
        finished.store(true, std::memory_order_release);
        return true;
    }, {});
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();

    std::thread stopper1{[&] { exec.Stop(); }};
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    std::atomic<bool> stopper2_saw_finished{false};
    std::thread stopper2{[&] {
        exec.Stop();
        stopper2_saw_finished.store(finished.load(std::memory_order_acquire), std::memory_order_release);
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    release.store(true, std::memory_order_release);
    stopper1.join();
    stopper2.join();
    BOOST_CHECK(stopper2_saw_finished.load(std::memory_order_acquire));
}

BOOST_AUTO_TEST_SUITE_END()
