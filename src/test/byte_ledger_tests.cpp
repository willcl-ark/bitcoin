// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/byte_ledger.h>

#include <boost/test/unit_test.hpp>

#include <limits>

BOOST_AUTO_TEST_SUITE(byte_ledger_tests)

BOOST_AUTO_TEST_CASE(account_limits_and_overflow_safety)
{
    util::ByteLedger ledger;
    util::ByteAccount& acc{ledger[util::ByteAccountId::Window]};

    BOOST_CHECK_EQUAL(acc.Bytes(), 0U);
    BOOST_CHECK_EQUAL(acc.Limit(), 0U);
    BOOST_CHECK(acc.Admits(std::numeric_limits<size_t>::max()));

    acc.SetLimit(1000);
    BOOST_CHECK(acc.Admits(1000));
    BOOST_CHECK(!acc.Admits(1001));

    acc.Add(600);
    BOOST_CHECK_EQUAL(acc.Bytes(), 600U);
    BOOST_CHECK(acc.Admits(400));
    BOOST_CHECK(!acc.Admits(401));
    // Must not wrap around and erroneously admit when more is near SIZE_MAX.
    BOOST_CHECK(!acc.Admits(std::numeric_limits<size_t>::max() - 10));

    acc.Sub(250);
    BOOST_CHECK_EQUAL(acc.Bytes(), 350U);
    acc.Set(0);
    BOOST_CHECK_EQUAL(acc.Bytes(), 0U);
}

BOOST_AUTO_TEST_SUITE_END()
