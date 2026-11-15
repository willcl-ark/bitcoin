// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_MACHINE_H
#define BITCOIN_TEST_UTIL_MACHINE_H

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <tuple>
#include <vector>

//! Check a table pair by pair against the edges written out again here: every listed pair leads
//! where the list says, and every pair not listed is refused.
template <typename Table, typename State, typename Event, size_t NS, size_t NE>
void CheckEveryPair(const Table& table, const std::array<State, NS>& states, const std::array<Event, NE>& events,
                    const std::vector<std::tuple<State, Event, State>>& expected)
{
    BOOST_CHECK_EQUAL(table.Edges().size(), expected.size());
    for (const State from : states) {
        for (const Event event : events) {
            std::optional<State> want;
            for (const auto& [e_from, e_event, e_to] : expected) {
                if (e_from == from && e_event == event) want = e_to;
            }
            BOOST_CHECK_MESSAGE(table.Next(from, event) == want,
                                "state " << static_cast<int>(from) << " event " << static_cast<int>(event));
        }
    }
}

#endif // BITCOIN_TEST_UTIL_MACHINE_H
