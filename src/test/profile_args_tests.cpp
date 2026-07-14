// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit.

#include <common/args.h>
#include <node/profile_args.h>
#include <test/util/setup_common.h>
#include <util/byte_units.h>

#include <boost/test/unit_test.hpp>

#include <optional>

BOOST_FIXTURE_TEST_SUITE(profile_args_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(profile_names)
{
    BOOST_CHECK_EQUAL(node::ListProfiles(), "pruned-tiny, pruned-low, performance, server");
    BOOST_CHECK(node::GetProfileOptions("pruned-tiny", std::nullopt, 1024).has_value());
    BOOST_CHECK(!node::GetProfileOptions("unknown", std::nullopt, 1024).has_value());
}

BOOST_AUTO_TEST_CASE(profile_option_table)
{
    const auto tiny{node::GetProfileOptions("pruned-tiny", std::nullopt, 1024)};
    BOOST_REQUIRE(tiny);
    BOOST_CHECK_EQUAL(*tiny->dbcache_mib, 100);
    BOOST_CHECK_EQUAL(*tiny->par, 1);
    BOOST_CHECK_EQUAL(*tiny->maxsigcachesize_mib, 8);
    BOOST_CHECK_EQUAL(*tiny->prune_mib, 2048);
    BOOST_CHECK_EQUAL(*tiny->maxconnections, 40);
    BOOST_CHECK_EQUAL(*tiny->maxreceivebuffer, 2000);
    BOOST_CHECK_EQUAL(*tiny->maxsendbuffer, 500);

    const auto low{node::GetProfileOptions("pruned-low", std::nullopt, 1024)};
    BOOST_REQUIRE(low);
    BOOST_CHECK_EQUAL(*low->dbcache_mib, 300);
    BOOST_CHECK_EQUAL(*low->par, 2);
    BOOST_CHECK_EQUAL(*low->maxsigcachesize_mib, 16);
    BOOST_CHECK_EQUAL(*low->prune_mib, 10240);
    BOOST_CHECK_EQUAL(*low->maxconnections, 60);
    BOOST_CHECK(!low->maxreceivebuffer);
    BOOST_CHECK(!low->maxsendbuffer);
}

BOOST_AUTO_TEST_CASE(high_resource_formulas)
{
    BOOST_CHECK_EQUAL(node::CalculateHighResourceDbCacheMiB(8_GiB, 1024), 4096);
    BOOST_CHECK_EQUAL(node::CalculateHighResourceDbCacheMiB(100_GiB, 1024), 4096);
    BOOST_CHECK_EQUAL(node::CalculateHighResourceDbCacheMiB(std::nullopt, 1536), 1536);
}

BOOST_AUTO_TEST_CASE(scaled_profiles)
{
    // performance scales the cache to RAM but leaves network limits alone.
    const auto performance{node::GetProfileOptions("performance", 100_GiB, 1024)};
    BOOST_REQUIRE(performance);
    BOOST_CHECK_EQUAL(*performance->dbcache_mib, 4096);
    BOOST_CHECK(!performance->maxsigcachesize_mib);
    BOOST_CHECK(!performance->maxconnections);
    BOOST_CHECK(!performance->prune_mib);

    // server scales the same cache but additionally widens serving limits.
    const auto server{node::GetProfileOptions("server", 8_GiB, 1024)};
    BOOST_REQUIRE(server);
    BOOST_CHECK_EQUAL(*server->dbcache_mib, 4096);
    BOOST_CHECK(!server->maxsigcachesize_mib);
    BOOST_CHECK_EQUAL(*server->maxconnections, 250);
    BOOST_CHECK_EQUAL(*server->maxreceivebuffer, 10000);
    BOOST_CHECK_EQUAL(*server->maxsendbuffer, 2000);
    BOOST_CHECK(!server->prune_mib);
}

BOOST_AUTO_TEST_CASE(profile_validation)
{
    ArgsManager args;
    BOOST_CHECK(!node::GetProfileError(args));

    args.ForceSetArg("-profile", "pruned-tiny");
    BOOST_CHECK(!node::GetProfileError(args));

    args.ForceSetArg("-profile", "invalid");
    const auto error{node::GetProfileError(args)};
    BOOST_REQUIRE(error);
    BOOST_CHECK_EQUAL(*error, "Unknown -profile value 'invalid'. Valid values are: pruned-tiny, pruned-low, performance, server.");
}

BOOST_AUTO_TEST_CASE(profile_soft_set)
{
    ArgsManager args;
    args.ForceSetArg("-profile", "pruned-tiny");
    node::ApplyProfileArgs(args);

    BOOST_CHECK_EQUAL(args.GetIntArg("-dbcache", 0), 100);
    BOOST_CHECK_EQUAL(args.GetIntArg("-par", 0), 1);
    BOOST_CHECK_EQUAL(args.GetIntArg("-maxsigcachesize", 0), 8);
    BOOST_CHECK_EQUAL(args.GetIntArg("-prune", 0), 2048);
    BOOST_CHECK_EQUAL(args.GetIntArg("-maxconnections", 0), 40);
    BOOST_CHECK_EQUAL(args.GetIntArg("-maxreceivebuffer", 0), 2000);
    BOOST_CHECK_EQUAL(args.GetIntArg("-maxsendbuffer", 0), 500);
    BOOST_CHECK(!args.GetArg("-maxmempool"));
    BOOST_CHECK(!args.GetArg("-blockreconstructionextratxn"));
}

BOOST_AUTO_TEST_CASE(profile_user_values_win)
{
    ArgsManager args;
    args.ForceSetArg("-profile", "pruned-tiny");
    args.ForceSetArg("-dbcache", "999");
    args.ForceSetArg("-par", "7");
    args.ForceSetArg("-maxsigcachesize", "64");

    node::ApplyProfileArgs(args);

    BOOST_CHECK_EQUAL(args.GetIntArg("-dbcache", 0), 999);
    BOOST_CHECK_EQUAL(args.GetIntArg("-par", 0), 7);
    BOOST_CHECK_EQUAL(args.GetIntArg("-maxsigcachesize", 0), 64);
    BOOST_CHECK_EQUAL(args.GetIntArg("-prune", 0), 2048);
}

BOOST_AUTO_TEST_SUITE_END()
