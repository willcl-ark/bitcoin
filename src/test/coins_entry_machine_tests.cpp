// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins_entry_machine.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstddef>
#include <set>
#include <vector>

using namespace coins_entry;

namespace {
constexpr State Go(State from, Event event) { return Next(from, event); }

std::vector<Event> AllEvents()
{
    std::vector<Event> events;
    for (size_t e{0}; e < EVENT_COUNT; ++e) events.push_back(static_cast<Event>(e));
    return events;
}

struct Filed {
    State state;
    int height;
};

Filed Step(Filed entry, Event event, int height)
{
    const State to{Next(entry.state, event)};
    BOOST_REQUIRE(to != REFUSED);
    const auto filed{NextHeight(HeightRule(entry.state, event), entry.state, entry.height, height)};
    BOOST_REQUIRE(filed.has_value());
    return {to, *filed};
}
} // namespace

BOOST_AUTO_TEST_SUITE(coins_entry_machine_tests)

BOOST_AUTO_TEST_CASE(known_states)
{
    // The 6 confirmed states an entry may rest in.
    const std::set<State> stored{
        BASE_COPY,
        DIRTY,
        DIRTY | FRESH,
        DIRTY | COLLECTED,
        DIRTY | SPENT,
        DIRTY | SPENT | COLLECTED,
    };
    for (size_t s{0}; s < 256; ++s) {
        const State state{static_cast<State>(s)};
        BOOST_CHECK_EQUAL(IsStored(state), stored.contains(state));
        BOOST_CHECK_EQUAL(IsKnown(state), stored.contains(state) || state == ABSENT);
    }
    BOOST_CHECK(!IsStored(ABSENT));
    BOOST_CHECK(!IsKnown(REFUSED));
}

BOOST_AUTO_TEST_CASE(table_is_closed_and_refuses_the_unknown)
{
    for (const Event event : AllEvents()) {
        for (size_t s{0}; s < 256; ++s) {
            const State from{static_cast<State>(s)};
            const State to{Go(from, event)};
            if (!IsKnown(from)) {
                BOOST_CHECK_EQUAL(to, REFUSED);
            } else {
                BOOST_CHECK(to == REFUSED || IsKnown(to));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(every_known_state_is_reached)
{
    std::array<bool, 256> seen{};
    std::vector<State> todo{ABSENT};
    seen[ABSENT] = true;
    while (!todo.empty()) {
        const State from{todo.back()};
        todo.pop_back();
        for (const Event event : AllEvents()) {
            const State to{Go(from, event)};
            if (to == REFUSED || seen[to]) continue;
            seen[to] = true;
            todo.push_back(to);
        }
    }
    for (size_t s{0}; s < 256; ++s) BOOST_CHECK_EQUAL(seen[s], IsKnown(static_cast<State>(s)));
}

BOOST_AUTO_TEST_CASE(insertions)
{
    const std::set<Event> inserting{Event::BaseCopy, Event::ConfirmedNew, Event::ConfirmedWrite, Event::ConfirmedSpend};
    for (const Event event : AllEvents()) {
        const State to{Go(ABSENT, event)};
        if (to == ABSENT) continue;
        BOOST_CHECK_EQUAL(to != REFUSED, inserting.contains(event));
    }
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::BaseCopy), BASE_COPY);
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::ConfirmedNew), DIRTY | FRESH);
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::ConfirmedWrite), DIRTY);
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::ConfirmedSpend), DIRTY | SPENT);
    for (size_t s{0}; s < 256; ++s) {
        const State from{static_cast<State>(s)};
        if (from == ABSENT) continue;
        for (const Event event : {Event::BaseCopy, Event::ConfirmedNew}) {
            BOOST_CHECK_EQUAL(Go(from, event), REFUSED);
        }
    }
}

BOOST_AUTO_TEST_CASE(fresh_means_nothing_to_erase)
{
    // A confirmed spend of a fresh entry is erased directly, not transitioned.
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH, Event::ConfirmedSpend), REFUSED);
    // Collecting clears FRESH so a subsequent spend writes an erase.
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH, Event::RunCollects), DIRTY | COLLECTED);
    BOOST_CHECK_EQUAL(Go(DIRTY | COLLECTED, Event::ConfirmedSpend), DIRTY | SPENT);
    // BatchOverwrite preserves FRESH; ConfirmedWrite clears it.
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH, Event::BatchOverwrite), DIRTY | FRESH);
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH, Event::ConfirmedWrite), DIRTY);
    BOOST_CHECK_EQUAL(Go(DIRTY | SPENT, Event::ConfirmedNew), REFUSED);
    BOOST_CHECK_EQUAL(Go(DIRTY | SPENT, Event::ConfirmedWrite), DIRTY);
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH, Event::KeptWritten), DIRTY);
}

BOOST_AUTO_TEST_CASE(a_change_withdraws_the_entry_from_its_run)
{
    for (const Event event : AllEvents()) {
        if (event == Event::RunCollects) continue;
        for (size_t s{0}; s < 256; ++s) {
            const State from{static_cast<State>(s)};
            if (!IsKnown(from) || !(from & COLLECTED)) continue;
            const State to{Go(from, event)};
            if (to == REFUSED) continue;
            BOOST_CHECK_EQUAL(static_cast<bool>(to & COLLECTED), event == Event::KeptWritten);
        }
    }
    for (size_t s{0}; s < 256; ++s) {
        const State from{static_cast<State>(s)};
        if (!IsKnown(from)) continue;
        BOOST_CHECK_EQUAL(Go(from, Event::RunLands) != REFUSED, static_cast<bool>(from & COLLECTED));
        if (Go(from, Event::RunLands) != REFUSED) BOOST_CHECK(!(Go(from, Event::RunLands) & DIRTY));
    }
}

BOOST_AUTO_TEST_CASE(height_rules_preserve_owed_dirty_height)
{
    BOOST_CHECK(NextHeight(Height::Keep, DIRTY, 5, 9) == 5);
    BOOST_CHECK(NextHeight(Height::Take, DIRTY, 5, 9) == 9);
    BOOST_CHECK(NextHeight(Height::Take, DIRTY, 9, 5) == 5);
    // Lower takes the minimum height when an earlier dirty change is already owed (CS-5).
    BOOST_CHECK(NextHeight(Height::Lower, DIRTY | SPENT, 5, 9) == 5);
    BOOST_CHECK(NextHeight(Height::Lower, DIRTY | SPENT, 9, 5) == 5);
    BOOST_CHECK(NextHeight(Height::Lower, BASE_COPY, 5, 9) == 9);

    // A confirmed spend at height 2 followed by a BIP30 re-create at height 6 keeps height 2 so a
    // lagged write-out at bound 4 still writes the owed change.
    const Filed spent{Step({BASE_COPY, 1}, Event::ConfirmedSpend, 2)};
    BOOST_CHECK_EQUAL(spent.state, DIRTY | SPENT);
    BOOST_CHECK_EQUAL(spent.height, 2);
    const Filed recreated{Step(spent, Event::ConfirmedWrite, 6)};
    BOOST_CHECK_EQUAL(recreated.state, DIRTY);
    BOOST_CHECK_EQUAL(recreated.height, 2);
}

BOOST_AUTO_TEST_SUITE_END()
