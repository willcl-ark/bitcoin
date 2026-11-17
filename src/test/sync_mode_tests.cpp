// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/sync_mode.h>

#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <set>

using node::ModeChoice;
using node::SyncMode;

BOOST_AUTO_TEST_SUITE(sync_mode_tests)

BOOST_AUTO_TEST_CASE(every_choice_has_one_row)
{
    std::set<ModeChoice> seen;
    for (const node::ModeChoiceRow& row : node::MODE_CHOICES) {
        BOOST_CHECK(seen.insert(row.choice).second);
        // A row says where its copy is read and what each mode does.
        BOOST_CHECK(!row.read.empty() && !row.download.empty() && !row.tip.empty() && !row.departs.empty());
    }
    BOOST_CHECK_EQUAL(seen.size(), static_cast<size_t>(ModeChoice::HistoricalYield) + 1);
}

BOOST_AUTO_TEST_CASE(mode_of_the_latch)
{
    BOOST_CHECK(node::SyncModeOf(true) == SyncMode::Download);
    BOOST_CHECK(node::SyncModeOf(false) == SyncMode::Tip);
}

//! The waits and the yield go by the copy they are given, not by the latch: an operation that started in
//! the download finishes as one of the download after the latch fell.
BOOST_FIXTURE_TEST_CASE(a_copy_outlives_the_latch, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    BOOST_REQUIRE(!chainman.IsInitialBlockDownload());
    BOOST_CHECK(chainman.SyncModeNow() == SyncMode::Tip);
    // Nothing is queued: neither mode gives way, and neither wait has anything to wait for.
    BOOST_CHECK(!chainman.YieldHistoricalActivation(SyncMode::Download));
    BOOST_CHECK(!chainman.YieldHistoricalActivation(SyncMode::Tip));
    chainman.WaitForPendingBlocks(SyncMode::Download);
    chainman.WaitForPendingBlocks(SyncMode::Tip);
}

BOOST_AUTO_TEST_SUITE_END()
