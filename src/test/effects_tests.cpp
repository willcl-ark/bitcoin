// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/effects.h>
#include <util/byte_ledger.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

BOOST_AUTO_TEST_SUITE(effects_tests)

BOOST_AUTO_TEST_CASE(fifo_order_and_concurrent_stop)
{
    util::ByteLedger ledger;
    std::vector<int> io_order;
    std::vector<int> done_order;
    kernel::EffectsExecutor exec(ledger[util::ByteAccountId::UndoBacklog], {}, /*threaded=*/true);
    exec.Start();

    for (int i{0}; i < 20; ++i) {
        exec.Enqueue(
            [&io_order, i]() {
                io_order.push_back(i);
                return (i % 2) == 0;
            },
            [&done_order, i](bool ok) {
                BOOST_CHECK_EQUAL(ok, (i % 2) == 0);
                done_order.push_back(i);
            },
            /*bytes=*/100);
    }

    exec.WaitForIO();
    BOOST_CHECK_EQUAL(exec.PendingBytes(), 0U);
    BOOST_CHECK(exec.HasPendingCompletions());
    exec.ApplyCompletions();
    BOOST_CHECK(exec.Idle());

    BOOST_REQUIRE_EQUAL(io_order.size(), 20U);
    BOOST_REQUIRE_EQUAL(done_order.size(), 20U);
    for (int i{0}; i < 20; ++i) {
        BOOST_CHECK_EQUAL(io_order[i], i);
        BOOST_CHECK_EQUAL(done_order[i], i);
    }

    // Concurrent Stop() calls must both wait cleanly until the worker has exited.
    std::thread t1([&] { exec.Stop(); });
    std::thread t2([&] { exec.Stop(); });
    t1.join();
    t2.join();

    // Restarting after Stop() works cleanly.
    exec.Start();
    exec.Stop();
}

BOOST_AUTO_TEST_CASE(inline_mode_spawns_no_thread_and_preserves_order)
{
    util::ByteLedger ledger;
    std::vector<int> order;
    kernel::EffectsExecutor exec(ledger[util::ByteAccountId::UndoBacklog], {}, /*threaded=*/false);
    exec.Start();

    exec.Enqueue([&] { order.push_back(1); return true; }, [&](bool ok) { BOOST_CHECK(ok); order.push_back(2); }, 50);
    exec.Enqueue([&] { order.push_back(3); return false; }, [&](bool ok) { BOOST_CHECK(!ok); order.push_back(4); }, 50);

    BOOST_CHECK_EQUAL(exec.PendingBytes(), 0U);
    exec.WaitForIO();
    exec.ApplyCompletions();
    BOOST_CHECK(exec.Idle());
    exec.Stop();

    const std::vector<int> expected{1, 3, 2, 4};
    BOOST_CHECK(order == expected);
}

BOOST_AUTO_TEST_SUITE_END()
