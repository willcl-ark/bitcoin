// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/handoff_machine.h>
#include <test/util/machine.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <utility>
#include <vector>

using node::ConnectorHandOffs;
using node::HAND_OFF_TABLE;
using node::HandOffTicket;
using S = node::HandOffState;
using E = node::HandOffEvent;

BOOST_FIXTURE_TEST_SUITE(handoff_machine_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(hand_off_table)
{
    CheckEveryPair(HAND_OFF_TABLE,
                   std::array{S::WithReceiveTask, S::Refused, S::Dropped, S::Queued, S::Taken, S::DoneActive, S::Done, S::Stopped},
                   std::array{E::Store, E::Refuse, E::Drop, E::Take, E::ActiveAttempted, E::AllAttempted, E::Stop},
                   {
                       {S::WithReceiveTask, E::Store, S::Queued},
                       {S::WithReceiveTask, E::Refuse, S::Refused},
                       {S::WithReceiveTask, E::Drop, S::Dropped},
                       {S::Queued, E::Take, S::Taken},
                       {S::Taken, E::ActiveAttempted, S::DoneActive},
                       {S::DoneActive, E::AllAttempted, S::Done},
                       {S::Queued, E::Stop, S::Stopped},
                       {S::Taken, E::Stop, S::Stopped},
                       {S::DoneActive, E::Stop, S::Stopped},
                   });
}

BOOST_AUTO_TEST_CASE(one_pass)
{
    ConnectorHandOffs handoffs;
    BOOST_CHECK(!handoffs.AnyQueued() && !handoffs.AnyUnattempted());
    const HandOffTicket first{handoffs.Hand(/*for_active=*/true)};
    BOOST_CHECK(handoffs.StateOf(first) == S::Queued);
    BOOST_CHECK(handoffs.AnyQueued());
    handoffs.Take();
    BOOST_CHECK(handoffs.StateOf(first) == S::Taken);
    BOOST_CHECK(!handoffs.AnyQueued() && handoffs.AnyUnattempted());
    // A block handed over during the pass waits for the next one.
    const HandOffTicket second{handoffs.Hand(/*for_active=*/true)};
    handoffs.ActiveAttempted();
    BOOST_CHECK(handoffs.StateOf(first) == S::DoneActive);
    BOOST_CHECK(handoffs.ActiveChainAttempted(first) && !handoffs.Attempted(first));
    BOOST_CHECK(handoffs.StateOf(second) == S::Queued);
    BOOST_CHECK(!handoffs.ActiveChainAttempted(second));
    handoffs.AllAttempted();
    BOOST_CHECK(handoffs.StateOf(first) == S::Done);
    BOOST_CHECK(handoffs.Attempted(first) && !handoffs.Attempted(second));
    BOOST_CHECK(handoffs.AnyQueued());
}

BOOST_AUTO_TEST_CASE(a_pass_that_gave_way_is_done_with_the_next)
{
    // The historical chainstate's activation gave way to a block handed over during the pass: the connector
    // takes again without AllAttempted.
    ConnectorHandOffs handoffs;
    const HandOffTicket first{handoffs.Hand(/*for_active=*/true)};
    BOOST_CHECK(handoffs.AnyActiveQueued());
    handoffs.Take();
    BOOST_CHECK(!handoffs.AnyActiveQueued());
    handoffs.ActiveAttempted();
    // A block for the historical chainstate is nothing to give way to.
    const HandOffTicket historical{handoffs.Hand(/*for_active=*/false)};
    BOOST_CHECK(handoffs.AnyQueued() && !handoffs.AnyActiveQueued());
    const HandOffTicket second{handoffs.Hand(/*for_active=*/true)};
    BOOST_CHECK(handoffs.AnyActiveQueued());
    handoffs.Take();
    BOOST_CHECK(handoffs.StateOf(first) == S::DoneActive);
    BOOST_CHECK(handoffs.ActiveChainAttempted(first) && !handoffs.Attempted(first));
    BOOST_CHECK(handoffs.StateOf(historical) == S::Taken);
    BOOST_CHECK(handoffs.StateOf(second) == S::Taken);
    BOOST_CHECK(!handoffs.ActiveChainAttempted(second));
    handoffs.ActiveAttempted();
    BOOST_CHECK(handoffs.StateOf(first) == S::DoneActive);
    BOOST_CHECK(handoffs.StateOf(second) == S::DoneActive);
    BOOST_CHECK(handoffs.ActiveChainAttempted(second) && !handoffs.Attempted(second));
    handoffs.AllAttempted();
    BOOST_CHECK(handoffs.StateOf(first) == S::Done);
    BOOST_CHECK(handoffs.StateOf(second) == S::Done);
    BOOST_CHECK(handoffs.Attempted(historical));
}

BOOST_AUTO_TEST_CASE(a_block_of_the_historical_chain_is_not_waited_for_by_the_active_one)
{
    ConnectorHandOffs handoffs;
    const HandOffTicket active{handoffs.Hand(/*for_active=*/true)};
    handoffs.Take();
    handoffs.ActiveAttempted();
    handoffs.AllAttempted();
    const HandOffTicket historical{handoffs.Hand(/*for_active=*/false)};
    BOOST_CHECK(handoffs.StateOf(historical) == S::Queued);
    // A transaction that follows it waits for nothing; a ping does.
    BOOST_CHECK(handoffs.ActiveChainAttempted(handoffs.Latest()));
    BOOST_CHECK(!handoffs.Attempted(handoffs.Latest()));
    BOOST_CHECK(handoffs.Attempted(active));
}

BOOST_AUTO_TEST_CASE(stopped_stays_stopped)
{
    ConnectorHandOffs handoffs;
    const HandOffTicket done{handoffs.Hand(/*for_active=*/true)};
    handoffs.Take();
    handoffs.ActiveAttempted();
    handoffs.AllAttempted();
    const HandOffTicket taken{handoffs.Hand(/*for_active=*/true)};
    handoffs.Take();
    const HandOffTicket queued{handoffs.Hand(/*for_active=*/true)};
    handoffs.Stop();
    // The pass that was running when the connector was stopped ends, and changes nothing any more.
    handoffs.ActiveAttempted();
    handoffs.AllAttempted();
    handoffs.Take();
    const HandOffTicket late{handoffs.Hand(/*for_active=*/true)};
    BOOST_CHECK(handoffs.StateOf(done) == S::Done);
    BOOST_CHECK(handoffs.StateOf(taken) == S::Stopped);
    BOOST_CHECK(handoffs.StateOf(queued) == S::Stopped);
    BOOST_CHECK(handoffs.StateOf(late) == S::Stopped);
    BOOST_CHECK(!handoffs.Attempted(taken) && !handoffs.ActiveChainAttempted(queued));
}

BOOST_AUTO_TEST_CASE(every_block_follows_the_table)
{
    // The marks move many blocks at once. Seen from each block, every move is an edge of the table, and
    // what the waiters ask agrees with the state of the blocks they wait for.
    ConnectorHandOffs handoffs;
    std::vector<std::pair<HandOffTicket, bool>> blocks;
    // Where the connector stands in its loop: 0 waiting (or gave way), 1 taken, 2 the active chain attempted.
    int at{0};
    for (int i{0}; i < 2000; ++i) {
        std::vector<S> before;
        for (const auto& [ticket, for_active] : blocks) before.push_back(handoffs.StateOf(ticket));
        E event{};
        bool moved{true};
        switch (m_rng.randrange(4)) {
        case 0: {
            const bool for_active{m_rng.randbool()};
            blocks.emplace_back(handoffs.Hand(for_active), for_active);
            BOOST_CHECK(handoffs.StateOf(blocks.back().first) == (handoffs.Stopped() ? S::Stopped : S::Queued));
            moved = false;
            break;
        }
        case 1:
            if (at == 0 && handoffs.AnyQueued()) {
                handoffs.Take();
                event = E::Take;
                at = 1;
            } else if (at == 1) {
                handoffs.ActiveAttempted();
                event = E::ActiveAttempted;
                at = 2;
            } else if (at == 2) {
                handoffs.AllAttempted();
                event = E::AllAttempted;
                at = 0;
            } else {
                moved = false;
            }
            break;
        case 2:
            if (m_rng.randrange(200) != 0) {
                moved = false;
                break;
            }
            handoffs.Stop();
            event = E::Stop;
            break;
        default:
            // The historical activation gives way to a block queued for the active chainstate: no event, and
            // the connector takes next.
            if (at == 2 && handoffs.AnyActiveQueued()) at = 0;
            moved = false;
        }
        for (size_t b{0}; b < before.size(); ++b) {
            const auto& [ticket, for_active]{blocks[b]};
            const S now{handoffs.StateOf(ticket)};
            if (now != before[b]) {
                BOOST_REQUIRE(moved);
                BOOST_CHECK(HAND_OFF_TABLE.Allows(before[b], event, now));
            }
            BOOST_CHECK_EQUAL(handoffs.Attempted(ticket), now == S::Done);
            if (for_active && !handoffs.Stopped()) {
                BOOST_CHECK_EQUAL(handoffs.ActiveChainAttempted(ticket), now == S::DoneActive || now == S::Done);
            }
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
