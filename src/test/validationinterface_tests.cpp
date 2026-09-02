// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>
#include <chain.h>
#include <consensus/validation.h>
#include <kernel/types.h>
#include <primitives/block.h>
#include <scheduler.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <validationinterface.h>

#include <atomic>
#include <future>
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

struct BlockSubscriber final : public CValidationInterface {
    std::shared_ptr<const CBlock> m_connected_block;
    std::shared_ptr<const CBlock> m_disconnected_block;

    void BlockConnected(const kernel::ChainstateRole&, const std::shared_ptr<const CBlock>& block, const CBlockIndex*) override
    {
        m_connected_block = block;
    }

    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex*) override
    {
        m_disconnected_block = block;
    }
};

BOOST_AUTO_TEST_CASE(deferred_block_read)
{
    std::promise<void> queue_blocked;
    std::promise<void> release_queue;
    auto release_future{release_queue.get_future().share()};
    m_node.validation_signals->CallFunctionInValidationInterfaceQueue([&] {
        queue_blocked.set_value();
        release_future.wait();
    });
    queue_blocked.get_future().wait();

    const uint256 block_hash{1};
    CBlockIndex index;
    index.phashBlock = &block_hash;
    index.nHeight = 1;
    auto expected_block{std::make_shared<CBlock>()};
    std::atomic<bool> connected_reader_called{false};
    std::atomic<bool> disconnected_reader_called{false};
    auto subscriber{std::make_shared<BlockSubscriber>()};
    m_node.validation_signals->RegisterSharedValidationInterface(subscriber);
    m_node.validation_signals->BlockConnected(kernel::ChainstateRole{}, [&] {
        connected_reader_called = true;
        return expected_block;
    }, &index);
    m_node.validation_signals->BlockDisconnected([&] {
        disconnected_reader_called = true;
        return expected_block;
    }, &index);

    BOOST_CHECK(!connected_reader_called);
    BOOST_CHECK(!disconnected_reader_called);
    BOOST_CHECK(!subscriber->m_connected_block);
    BOOST_CHECK(!subscriber->m_disconnected_block);
    release_queue.set_value();
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK(connected_reader_called);
    BOOST_CHECK(disconnected_reader_called);
    BOOST_CHECK(subscriber->m_connected_block == expected_block);
    BOOST_CHECK(subscriber->m_disconnected_block == expected_block);
    m_node.validation_signals->UnregisterSharedValidationInterface(subscriber);
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

BOOST_AUTO_TEST_SUITE_END()
