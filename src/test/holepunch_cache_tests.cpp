// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <holepunch.h>

#include <netbase.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <vector>

namespace {
holepunch::Descriptor Record(uint16_t port = 8333)
{
    CKey key;
    key.MakeNewKey(true);
    return {LookupNumeric("8.8.8.8", port), 100, key.GetPubKey()};
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(holepunch_cache_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(replacement_flood_preserves_retained_records)
{
    holepunch::DescriptorCache cache{2};
    const auto first{Record()};
    const auto second{Record(8334)};
    BOOST_REQUIRE(cache.Insert(first, first.epoch));
    BOOST_REQUIRE(cache.Insert(second, second.epoch));
    for (uint16_t port{9000}; port < 9100; ++port) {
        auto flood{first};
        flood.endpoint = CService{first.endpoint, port};
        BOOST_CHECK(!cache.Insert(flood, flood.epoch));
    }
    BOOST_CHECK(cache.Contains(first));
    BOOST_CHECK(cache.Contains(second));
    cache.Expire(first.epoch + 1);
    auto next{first};
    ++next.epoch;
    BOOST_CHECK(cache.Insert(next, next.epoch));
}

BOOST_AUTO_TEST_CASE(source_and_destination_group_limits)
{
    const auto record{Record()};
    for (bool common_source : {false, true}) {
        holepunch::DescriptorCache cache;
        size_t accepted{0};
        for (uint16_t port{1}; port <= 100; ++port) {
            auto candidate{record};
            candidate.endpoint = CService{record.endpoint, port};
            const std::vector<unsigned char> group{static_cast<unsigned char>(port)};
            accepted += cache.Insert(candidate, candidate.epoch,
                                     common_source ? std::vector<unsigned char>{0} : group,
                                     common_source ? group : std::vector<unsigned char>{0});
        }
        // Distinct ports behind the same IP remain candidates, subject to both caps.
        BOOST_CHECK_EQUAL(accepted, 32);
        BOOST_CHECK_EQUAL(cache.Size(), accepted);
        auto other{record};
        other.endpoint = CService{record.endpoint, 1000};
        BOOST_CHECK(cache.Insert(other, other.epoch, {101}, {101}));
        BOOST_CHECK(cache.SourceGroup(other.endpoint) == std::vector<unsigned char>{101});
        // Repeating a descriptor from another source cannot migrate its ownership.
        BOOST_CHECK(cache.Insert(other, other.epoch, {102}, {102}));
        BOOST_CHECK(cache.SourceGroup(other.endpoint) == std::vector<unsigned char>{101});
    }
}

BOOST_AUTO_TEST_CASE(placement_ignores_keys_and_epochs)
{
    holepunch::DescriptorCache cache{64};
    auto record{Record()};
    const auto replacement_key{Record().pubkey};
    std::vector<bool> admitted;
    for (int epoch{0}; epoch < 2; ++epoch) {
        for (uint16_t port{1}; port <= 256; ++port) {
            record.endpoint = CService{record.endpoint, port};
            const std::vector<unsigned char> group{static_cast<unsigned char>(port >> 8), static_cast<unsigned char>(port)};
            const bool accepted{cache.Insert(record, record.epoch, group, group)};
            if (epoch == 0) {
                admitted.push_back(accepted);
            } else {
                BOOST_CHECK_EQUAL(accepted, admitted[port - 1]);
            }
        }
        BOOST_CHECK_LE(cache.Size(), 64);
        ++record.epoch;
        record.pubkey = replacement_key;
        cache.Expire(record.epoch);
    }
}

BOOST_AUTO_TEST_CASE(structural_parsing_precedes_curve_validation)
{
    const auto record{Record()};
    auto payload{record.Payload()};
    // A syntactically compressed x coordinate outside the field is not a curve point.
    std::fill(payload.begin() + 10, payload.end(), 0xff);
    const auto unchecked{holepunch::Descriptor::ParseUnchecked(payload, record.endpoint.GetPort())};
    BOOST_REQUIRE(unchecked);
    BOOST_CHECK(unchecked->IsValidStructure());
    BOOST_CHECK(!unchecked->IsValid());
    BOOST_CHECK(!holepunch::Descriptor::Parse(payload, record.endpoint.GetPort()));
    BOOST_CHECK(!holepunch::Descriptor::ParseUnchecked(payload, 0));
    payload[0] = 2;
    BOOST_CHECK(!holepunch::Descriptor::ParseUnchecked(payload, record.endpoint.GetPort()));
}

BOOST_AUTO_TEST_SUITE_END()
