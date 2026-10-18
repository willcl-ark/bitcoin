// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <holepunch.h>
#include <netbase.h>
#include <streams.h>
#include <test/data/holepunch.json.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>

using namespace holepunch;

BOOST_FIXTURE_TEST_SUITE(holepunch_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(wire_and_pair_vectors)
{
    UniValue vectors;
    BOOST_REQUIRE(vectors.read(json_tests::holepunch));
    BOOST_REQUIRE(vectors.isObject());
    for (const auto& vector : vectors["feature_valid"].getValues()) {
        const auto bytes = ParseHex(vector["hex"].get_str());
        const auto feature = ParseFeature(bytes);
        BOOST_REQUIRE(feature);
        BOOST_CHECK_EQUAL(feature->version, vector["decoded"]["version"].getInt<int>());
        BOOST_CHECK_EQUAL(feature->flags, vector["decoded"]["flags"].getInt<uint32_t>());
        DataStream stream;
        stream << *feature;
        BOOST_CHECK_EQUAL(HexStr(stream), HexStr(bytes));
    }
    for (const auto& vector : vectors["feature_invalid"].getValues())
        BOOST_CHECK(!ParseFeature(ParseHex(vector["hex"].get_str())));
    for (const auto& vector : vectors["punchkey_elements_valid"].getValues()) {
        DataStream stream{ParseHex(vector["element_hex"].get_str())};
        WireAddress wire;
        stream >> wire;
        BOOST_CHECK(stream.empty());
        const auto announcement = Decode(wire, true);
        BOOST_REQUIRE(announcement);
        DataStream encoded;
        encoded << Encode(*announcement);
        BOOST_CHECK_EQUAL(HexStr(encoded), vector["element_hex"].get_str());
        BOOST_CHECK(!Decode(wire)); // Published vectors use documentation addresses.
        if (wire.address[0] == 2) {
            std::fill(wire.address.begin() + 1, wire.address.begin() + 11, 0);
            wire.address[11] = wire.address[12] = 0xff;
            BOOST_CHECK(!Decode(wire, true));
        }
    }
    for (const auto& vector : vectors["punchkey_elements_invalid"].getValues()) {
        DataStream stream{ParseHex(vector["element_hex"].get_str())};
        WireAddress wire;
        stream >> wire;
        BOOST_CHECK(!Decode(wire, true));
    }
    for (const auto& vector : vectors["pairing"].getValues()) {
        const auto parties = vector["parties"].getValues();
        const auto a = parties[0].get_str(), b = parties[1].get_str();
        CKey key_a, key_b;
        const auto secret_a = ParseHex(vector["secret_" + a].get_str());
        const auto secret_b = ParseHex(vector["secret_" + b].get_str());
        key_a.Set(secret_a.begin(), secret_a.end(), true);
        key_b.Set(secret_b.begin(), secret_b.end(), true);
        BOOST_CHECK_EQUAL(HexStr(key_a.GetPubKey()), vector["pubkey_" + a].get_str());
        const auto shared = key_a.ComputeECDHSecret(key_b.GetPubKey());
        BOOST_REQUIRE(shared);
        BOOST_CHECK(*shared == *key_b.ComputeECDHSecret(key_a.GetPubKey()));
        BOOST_CHECK_EQUAL(HexStr(*shared), vector["shared_x"].get_str());
        const auto endpoint = [&](const std::string& party) {
            const auto& item = vector["endpoint_" + party];
            DataStream stream;
            stream << uint8_t(item["net"].getInt<int>()) << ParseHex(item["addr"].get_str());
            CNetAddr address;
            stream >> CNetAddr::V2(address);
            return CService{address, uint16_t(item["port"].getInt<int>())};
        };
        const auto pair = PairValue(*shared, uint256::FromHex(vector["block_hash_display"].get_str()).value(), endpoint(a), endpoint(b));
        BOOST_CHECK_EQUAL(HexStr(pair), vector["pair"].get_str());
        BOOST_CHECK_EQUAL(IsInitiator(pair, key_a.GetPubKey(), key_b.GetPubKey()), vector["initiator"].get_str() == a);
        BOOST_CHECK(pair == PairValue(*shared, uint256::FromHex(vector["block_hash_display"].get_str()).value(), endpoint(b), endpoint(a)));
    }
}

BOOST_AUTO_TEST_CASE(reports_and_table)
{
    const auto own = LookupNumeric("8.8.8.8:50000");
    const auto other = LookupNumeric("1.1.1.1:50001");
    Reports reports;
    reports.Set(1, {1}, own);
    reports.Set(2, {1}, own);
    BOOST_CHECK(!reports.External(NET_IPV4));
    reports.Set(2, {2}, own);
    reports.Set(3, {3}, other);
    BOOST_CHECK(reports.External(NET_IPV4) == own);
    reports.Set(4, {4}, other);
    BOOST_CHECK(!reports.External(NET_IPV4));
    reports.Remove(4);
    BOOST_CHECK(reports.External(NET_IPV4) == own);

    State state;
    state.SetExternal(NET_IPV4, own, 100);
    const auto ipv4_key = state.PubKey(NET_IPV4), ipv6_key = state.PubKey(NET_IPV6);
    CKey key;
    key.MakeNewKey(true);
    Announcement a{0, NODE_P2P_V2, other, key.GetPubKey()};
    BOOST_CHECK(state.Add(1, {1}, a, 100));
    BOOST_CHECK(state.Add(1, {1}, a, 101));
    BOOST_CHECK_EQUAL(state.Entries()[0].received_at, 100);
    BOOST_CHECK(state.Ranked(uint256{}, 100, 101).empty());
    BOOST_CHECK_EQUAL(state.Ranked(uint256{}, 101, 101).size(), 1U);
    BOOST_CHECK(state.Add(2, {2}, a, 102));
    key.MakeNewKey(true);
    a.pubkey = key.GetPubKey();
    BOOST_CHECK(state.Add(1, {1}, a, 103));
    BOOST_CHECK_EQUAL(state.Entries().size(), 2U);
    BOOST_CHECK_EQUAL(state.Entries()[0].board, 2);
    BOOST_CHECK_EQUAL(state.Entries()[0].received_at, 102);
    BOOST_CHECK(state.SetExternal(NET_IPV4, LookupNumeric("9.9.9.9:50000"), 104));
    BOOST_CHECK(state.PubKey(NET_IPV4) != ipv4_key);
    BOOST_CHECK(state.PubKey(NET_IPV6) == ipv6_key);
    BOOST_CHECK(state.Entries()[1].secret == *key.ComputeECDHSecret(state.PubKey(NET_IPV4)));
    BOOST_CHECK(state.Entries()[0].secret != state.Entries()[1].secret);
    state.Expire(3703);
    BOOST_CHECK(state.Entries().empty());
    for (size_t i{1}; i <= SOURCE_CAP + 1; ++i) {
        a.endpoint = LookupNumeric("1.1.2." + std::to_string(i) + ":50000");
        BOOST_CHECK(state.Add(1, {1}, a, 4000));
    }
    BOOST_CHECK_EQUAL(state.Entries().size(), SOURCE_CAP);
    const auto block = uint256::ONE;
    BOOST_CHECK(!state.Tip(uint256{}, uint256{}, 0, 4001, false));
    BOOST_CHECK(state.Tip(block, uint256{}, 1, 4002, false) == 4001);
    BOOST_CHECK(!state.Tip(block, uint256{}, 1, 4003, false));
    BOOST_REQUIRE(state.Add(1, {1}, a, 4000 + ENTRY_TTL));
    BOOST_CHECK_EQUAL(state.Entries().front().received_at, 4000 + ENTRY_TTL);
}

BOOST_AUTO_TEST_CASE(board_limits_and_coalescing)
{
    Board board;
    CKey key;
    key.MakeNewKey(true);
    const auto a = LookupNumeric("8.8.8.8:50000"), b = LookupNumeric("1.1.1.1:50001");
    BOOST_REQUIRE(board.Admit(1, a, {1}, 100));
    BOOST_REQUIRE(board.Admit(2, b, {2}, 100));
    BOOST_CHECK(!board.Admit(3, CService{a, 50002}, {3}, 100));
    Announcement announcement{0, NODE_P2P_V2, a, key.GetPubKey()};
    BOOST_CHECK(!board.Announce(2, Encode(announcement), 100));
    BOOST_CHECK(board.Drain(2, 100).empty());
    BOOST_REQUIRE(board.Announce(1, Encode(announcement), 100));
    BOOST_CHECK_EQUAL(board.Drain(1, 100).size(), 1U);
    auto second = announcement;
    second.endpoint = b;
    BOOST_REQUIRE(board.Announce(2, Encode(second), 100));
    board.Drain(2, 100);
    board.Drain(1, 100);
    for (int i{0}; i < 20; ++i) {
        key.MakeNewKey(true);
        announcement.pubkey = key.GetPubKey();
        BOOST_REQUIRE(board.Announce(1, Encode(announcement), 101));
    }
    const auto updates = board.Drain(2, 101);
    BOOST_REQUIRE_EQUAL(updates.size(), 1U);
    BOOST_CHECK(updates[0].pubkey == announcement.pubkey);
    size_t pushed = updates.size();
    for (int i{0}; i < 128; ++i) {
        key.MakeNewKey(true);
        announcement.pubkey = key.GetPubKey();
        BOOST_REQUIRE(board.Announce(1, Encode(announcement), 102));
        pushed += board.Drain(2, 102).size();
    }
    BOOST_CHECK_EQUAL(pushed, 128U);
    BOOST_CHECK(board.Drain(2, 103).empty());
    key.MakeNewKey(true);
    second.pubkey = key.GetPubKey();
    BOOST_REQUIRE(board.Announce(2, Encode(second), 103));
    const auto ack = board.Drain(2, 103);
    BOOST_REQUIRE_EQUAL(ack.size(), 1U);
    BOOST_CHECK(ack[0].endpoint == b && ack[0].pubkey == second.pubkey);
    const auto roster = board.Drain(2, 1901);
    BOOST_REQUIRE_EQUAL(roster.size(), 2U);
    BOOST_CHECK(std::any_of(roster.begin(), roster.end(), [&](const auto& item) {
        return item.endpoint == b && item.pubkey == second.pubkey;
    }));
    RateLimit limit;
    for (int i{0}; i < 240; ++i)
        BOOST_CHECK(limit.Take(100));
    BOOST_CHECK(!limit.Take(3699));
    BOOST_CHECK(limit.Take(3700));
    board.Remove(1, 1902);
    const auto refresh = board.Drain(2, 3701);
    BOOST_REQUIRE_EQUAL(refresh.size(), 1U);
    BOOST_CHECK(refresh[0].endpoint == b);
}
BOOST_AUTO_TEST_SUITE_END()
