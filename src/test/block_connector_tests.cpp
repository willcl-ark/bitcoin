// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/block_connector.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <future>
#include <memory>
#include <thread>

BOOST_AUTO_TEST_SUITE(block_connector_tests)

BOOST_AUTO_TEST_CASE(lazy_start_and_empty_stop)
{
    std::atomic<int> active_calls{0};
    std::atomic<int> historical_calls{0};
    node::BlockConnector connector{
        [&](std::shared_ptr<const CBlock>) { ++active_calls; },
        [&]() { ++historical_calls; },
        []() { return false; },
    };

    const node::HandOffTicket t0{connector.Latest()};
    BOOST_CHECK(connector.Done(t0));
    BOOST_CHECK(!connector.AnyActivePending());
    BOOST_CHECK(!connector.IsConnectorThread());
    connector.WaitForPending();
    connector.StopForIdle();
    connector.Stop();
    connector.Stop(); // idempotent
    BOOST_CHECK_EQUAL(active_calls.load(), 0);
    BOOST_CHECK_EQUAL(historical_calls.load(), 0);
}

BOOST_AUTO_TEST_CASE(active_and_historical_tickets)
{
    std::promise<void> in_active;
    std::promise<void> release_active;
    std::shared_future<void> release_active_f{release_active.get_future()};
    std::promise<void> in_historical;
    std::promise<void> release_historical;
    std::shared_future<void> release_historical_f{release_historical.get_future()};

    std::atomic<int> active_calls{0};
    std::atomic<int> historical_calls{0};
    std::atomic<bool> saw_connector_thread{false};

    node::BlockConnector* connector_ptr{nullptr};
    node::BlockConnector connector{
        [&](std::shared_ptr<const CBlock>) {
            if (connector_ptr->IsConnectorThread()) saw_connector_thread.store(true);
            if (++active_calls == 1) {
                in_active.set_value();
                release_active_f.wait();
            }
        },
        [&]() {
            if (++historical_calls == 1) {
                in_historical.set_value();
                release_historical_f.wait();
            }
        },
        []() { return false; },
    };
    connector_ptr = &connector;

    connector.Signal(std::make_shared<CBlock>(), /*for_active=*/true);
    const node::HandOffTicket t1{connector.Latest()};
    in_active.get_future().wait();

    // Active pass is running: ticket is not yet done.
    BOOST_CHECK(!connector.Done(t1));
    BOOST_CHECK(connector.AnyActivePending());
    BOOST_CHECK(saw_connector_thread.load());
    BOOST_CHECK(!connector.IsConnectorThread());

    // Signal a historical-only block while active pass is running: ticket does not advance.
    connector.Signal(std::make_shared<CBlock>(), /*for_active=*/false);
    BOOST_CHECK_EQUAL(connector.Latest().value, t1.value);

    // Release active pass: ticket completes BEFORE historical pass finishes.
    release_active.set_value();
    in_historical.get_future().wait();
    BOOST_CHECK(connector.Done(t1));
    BOOST_CHECK(!connector.AnyActivePending());

    release_historical.set_value();
    connector.Stop();
    BOOST_CHECK_GE(active_calls.load(), 1);
    BOOST_CHECK_GE(historical_calls.load(), 1);
}

BOOST_AUTO_TEST_CASE(stop_for_idle_and_restart)
{
    std::atomic<int> active_calls{0};
    node::BlockConnector connector{
        [&](std::shared_ptr<const CBlock>) { ++active_calls; },
        []() {},
        []() { return false; },
    };

    connector.Signal(std::make_shared<CBlock>(), /*for_active=*/true);
    connector.WaitForPending();
    BOOST_CHECK_GE(active_calls.load(), 1);

    // Join thread for idle state, then restart cleanly on a subsequent Signal().
    connector.StopForIdle();
    connector.StopForIdle(); // idempotent
    BOOST_CHECK(!connector.AnyActivePending());

    const int before_restart{active_calls.load()};
    connector.Signal(std::make_shared<CBlock>(), /*for_active=*/true);
    connector.WaitForPending();
    BOOST_CHECK_GT(active_calls.load(), before_restart);

    connector.Stop();
}

BOOST_AUTO_TEST_CASE(stop_resolves_pending_tickets_and_concurrent_stops)
{
    std::promise<void> entered;
    std::promise<void> allow_exit;
    std::shared_future<void> allow_exit_f{allow_exit.get_future()};

    std::atomic<int> active_calls{0};
    node::BlockConnector connector{
        [&](std::shared_ptr<const CBlock>) {
            if (++active_calls == 1) {
                entered.set_value();
                allow_exit_f.wait();
            }
        },
        []() {},
        []() { return false; },
    };

    connector.Signal(std::make_shared<CBlock>(), /*for_active=*/true);
    entered.get_future().wait();

    // Queue a second active hand-off that will not be attempted once Stop() drains.
    connector.Signal(std::make_shared<CBlock>(), /*for_active=*/true);
    const node::HandOffTicket t2{connector.Latest()};
    BOOST_CHECK(!connector.Done(t2));

    std::thread stopper1{[&] { connector.Stop(); }};
    std::thread stopper2{[&] { connector.Stop(); }};
    allow_exit.set_value();
    stopper1.join();
    stopper2.join();

    // Once stopped, Done() must report true and AnyActivePending() must report false.
    BOOST_CHECK(connector.Done(t2));
    BOOST_CHECK(!connector.AnyActivePending());
    connector.WaitForPending();

    // Signaling after Stop() is a safe no-op.
    connector.Signal(std::make_shared<CBlock>(), /*for_active=*/true);
    BOOST_CHECK(connector.Done(connector.Latest()));
}

BOOST_AUTO_TEST_SUITE_END()
