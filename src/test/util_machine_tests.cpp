// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/machine.h>

#include <boost/test/unit_test.hpp>

#include <optional>

namespace {
enum class Door { Open, Closed, Locked };
enum class Act { Push, Pull, Turn };

constexpr auto DOOR_TABLE{util::MakeTransitionTable<Door, Act>({
    {Door::Open, Act::Push, Door::Closed},
    {Door::Closed, Act::Pull, Door::Open},
    {Door::Closed, Act::Turn, Door::Locked},
    {Door::Locked, Act::Turn, Door::Closed},
    {Door::Locked, Act::Push, Door::Locked},
})};

// The table is usable in constant expressions.
static_assert(DOOR_TABLE.Next(Door::Open, Act::Push) == Door::Closed);
static_assert(!DOOR_TABLE.Next(Door::Open, Act::Pull));
static_assert(DOOR_TABLE.Allows(Door::Closed, Act::Turn, Door::Locked));
static_assert(!DOOR_TABLE.Allows(Door::Closed, Act::Turn, Door::Open));
} // namespace

BOOST_AUTO_TEST_SUITE(util_machine_tests)

BOOST_AUTO_TEST_CASE(next_follows_the_edge)
{
    BOOST_CHECK(DOOR_TABLE.Next(Door::Open, Act::Push) == Door::Closed);
    BOOST_CHECK(DOOR_TABLE.Next(Door::Closed, Act::Pull) == Door::Open);
    BOOST_CHECK(DOOR_TABLE.Next(Door::Closed, Act::Turn) == Door::Locked);
    BOOST_CHECK(DOOR_TABLE.Next(Door::Locked, Act::Turn) == Door::Closed);
    // An edge may lead back to its own state.
    BOOST_CHECK(DOOR_TABLE.Next(Door::Locked, Act::Push) == Door::Locked);
}

BOOST_AUTO_TEST_CASE(a_pair_without_an_edge_is_refused)
{
    BOOST_CHECK(!DOOR_TABLE.Next(Door::Open, Act::Pull));
    BOOST_CHECK(!DOOR_TABLE.Next(Door::Open, Act::Turn));
    BOOST_CHECK(!DOOR_TABLE.Next(Door::Closed, Act::Push));
    BOOST_CHECK(!DOOR_TABLE.Next(Door::Locked, Act::Pull));
}

BOOST_AUTO_TEST_CASE(allows_names_the_one_outcome)
{
    BOOST_CHECK(DOOR_TABLE.Allows(Door::Open, Act::Push, Door::Closed));
    BOOST_CHECK(!DOOR_TABLE.Allows(Door::Open, Act::Push, Door::Open));
    BOOST_CHECK(!DOOR_TABLE.Allows(Door::Open, Act::Push, Door::Locked));
    // A refused pair allows no outcome at all.
    BOOST_CHECK(!DOOR_TABLE.Allows(Door::Open, Act::Pull, Door::Open));
    BOOST_CHECK(!DOOR_TABLE.Allows(Door::Open, Act::Pull, Door::Closed));
}

BOOST_AUTO_TEST_CASE(edges_are_kept_as_given)
{
    BOOST_CHECK_EQUAL(DOOR_TABLE.Edges().size(), 5U);
    BOOST_CHECK(DOOR_TABLE.Edges().front().from == Door::Open);
    BOOST_CHECK(DOOR_TABLE.Edges().back().to == Door::Locked);
}

BOOST_AUTO_TEST_SUITE_END()
