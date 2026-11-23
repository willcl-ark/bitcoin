// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/connect_machine.h>

#include <boost/test/unit_test.hpp>

using node::WINDOW_BLOCK_TABLE;
using S = node::WindowBlockState;
using E = node::WindowBlockEvent;

BOOST_AUTO_TEST_SUITE(connect_machine_tests)

BOOST_AUTO_TEST_CASE(window_block_readings)
{
    // A block is published from a terminal state (Done or Failed) so its outcome cannot change under
    // the publisher. Only IndexAndValidate or StartValidate enters Validating.
    for (const S state : {S::Reading, S::Indexed, S::Validating, S::Done, S::Failed}) {
        bool leaves{false};
        for (const E event : {E::Index, E::IndexAndValidate, E::FailRead, E::StartValidate, E::Finish}) {
            const auto next{WINDOW_BLOCK_TABLE.Next(state, event)};
            if (!next) continue;
            leaves = true;
            BOOST_CHECK_EQUAL(node::IsValidating(*next), event == E::IndexAndValidate || event == E::StartValidate);
            if (event != E::FailRead) BOOST_CHECK(node::IsIndexed(*next));
        }
        BOOST_CHECK_EQUAL(node::IsComplete(state), !leaves);
    }
    BOOST_CHECK(!node::IsIndexed(S::Reading));
    BOOST_CHECK(!node::IsIndexed(S::Failed));
    BOOST_CHECK(node::IsIndexed(S::Indexed) && !node::IsComplete(S::Indexed));
    BOOST_CHECK(node::IsIndexed(S::Validating) && node::IsValidating(S::Validating) && !node::IsComplete(S::Validating));
    BOOST_CHECK(node::IsIndexed(S::Done) && node::IsComplete(S::Done));
    BOOST_CHECK(node::IsComplete(S::Failed));
}

BOOST_AUTO_TEST_SUITE_END()
