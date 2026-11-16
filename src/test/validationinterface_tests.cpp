// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>
#include <consensus/validation.h>
#include <primitives/block.h>
#include <scheduler.h>
#include <sync.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <util/check.h>
#include <validationinterface.h>

#include <atomic>
#include <memory>

BOOST_FIXTURE_TEST_SUITE(validationinterface_tests, ChainTestingSetup)

struct TestSubscriberNoop final : public CValidationInterface {
    void BlockChecked(const std::shared_ptr<const CBlock>&, const BlockValidationState&) override {}
};

BOOST_AUTO_TEST_CASE(unregister_validation_interface_race)
{
    std::atomic<bool> generate{true};

    // Start thread to generate notifications
    std::thread gen{[&] {
        BlockValidationState state_dummy;
        while (generate) {
            m_node.validation_signals->BlockChecked(std::make_shared<const CBlock>(), state_dummy);
        }
    }};

    // Start thread to consume notifications
    std::thread sub{[&] {
        // keep going for about 1 sec, which is 250k iterations
        for (int i = 0; i < 250000; i++) {
            auto sub = std::make_shared<TestSubscriberNoop>();
            m_node.validation_signals->RegisterSharedValidationInterface(sub);
            m_node.validation_signals->UnregisterSharedValidationInterface(sub);
        }
        // tell the other thread we are done
        generate = false;
    }};

    gen.join();
    sub.join();
    BOOST_CHECK(!generate);
}

class TestInterface : public CValidationInterface
{
public:
    TestInterface(ValidationSignals& signals, std::function<void()> on_call = nullptr, std::function<void()> on_destroy = nullptr)
        : m_on_call(std::move(on_call)), m_on_destroy(std::move(on_destroy)), m_signals{signals}
    {
    }
    virtual ~TestInterface()
    {
        if (m_on_destroy) m_on_destroy();
    }
    void BlockChecked(const std::shared_ptr<const CBlock>& block, const BlockValidationState& state) override
    {
        if (m_on_call) m_on_call();
    }
    void Call()
    {
        BlockValidationState state;
        m_signals.BlockChecked(std::make_shared<const CBlock>(), state);
    }
    std::function<void()> m_on_call;
    std::function<void()> m_on_destroy;
    ValidationSignals& m_signals;
};

// Regression test to ensure UnregisterAllValidationInterfaces calls don't
// destroy a validation interface while it is being called. Bug:
// https://github.com/bitcoin/bitcoin/pull/18551
BOOST_AUTO_TEST_CASE(unregister_all_during_call)
{
    bool destroyed = false;
    auto shared{std::make_shared<TestInterface>(
        *m_node.validation_signals,
        [&] {
            // First call should decrements reference count 2 -> 1
            m_node.validation_signals->UnregisterAllValidationInterfaces();
            BOOST_CHECK(!destroyed);
            // Second call should not decrement reference count 1 -> 0
            m_node.validation_signals->UnregisterAllValidationInterfaces();
            BOOST_CHECK(!destroyed);
        },
        [&] { destroyed = true; })};
    m_node.validation_signals->RegisterSharedValidationInterface(shared);
    BOOST_CHECK(shared.use_count() == 2);
    shared->Call();
    BOOST_CHECK(shared.use_count() == 1);
    BOOST_CHECK(!destroyed);
    shared.reset();
    BOOST_CHECK(destroyed);
}

#ifdef DEBUG_LOCKORDER
// The wait rule is applied where the node waits for the queue: under the mempool's lock,
// which the queued callbacks take, the wait is refused before anything is queued.
BOOST_AUTO_TEST_CASE(wait_for_the_queue_refuses_the_mempool_lock)
{
    const bool prev{g_debug_lockorder_abort};
    g_debug_lockorder_abort = false;

    ValidationSignals& signals{*Assert(m_node.validation_signals)};
    BOOST_CHECK_NO_THROW(signals.SyncWithValidationInterfaceQueue());
    {
        LOCK(Assert(m_node.mempool)->cs);
        BOOST_CHECK_EXCEPTION(signals.SyncWithValidationInterfaceQueue(), std::logic_error,
                              HasReason{"waiting for the validation signal queue while holding cs"});
    }
    BOOST_CHECK_NO_THROW(signals.SyncWithValidationInterfaceQueue());

    g_debug_lockorder_abort = prev;
}
#endif // DEBUG_LOCKORDER

BOOST_AUTO_TEST_SUITE_END()
