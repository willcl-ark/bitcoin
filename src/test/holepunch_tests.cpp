// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <holepunch.h>

#include <hash.h>
#include <netbase.h>
#include <test/data/holepunch.json.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace {

UniValue Vectors()
{
    UniValue vectors;
    BOOST_REQUIRE(vectors.read(json_tests::holepunch));
    return vectors;
}

CKey Secret(std::string_view hex)
{
    const auto bytes = ParseHex(hex);
    CKey key;
    key.Set(bytes.begin(), bytes.end(), true);
    return key;
}

holepunch::Bytes32 Bytes32(std::string_view hex)
{
    const auto bytes = ParseHex(hex);
    BOOST_REQUIRE_EQUAL(bytes.size(), 32);
    holepunch::Bytes32 result;
    std::copy(bytes.begin(), bytes.end(), result.begin());
    return result;
}

holepunch::Descriptor Descriptor(const UniValue& vector)
{
    const auto descriptor = holepunch::Descriptor::Parse(ParseHex(vector["addr_payload"].get_str()), vector["port"].getInt<uint16_t>());
    BOOST_REQUIRE(descriptor);
    return *descriptor;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(holepunch_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(published_vectors)
{
    const auto vectors = Vectors();
    const uint256 chain{ParseHex(vectors["chain"].get_str())};
    BOOST_CHECK(chain == Hash(ParseHex(vectors["genesis_header"].get_str())));
    const uint256 session_id{ParseHex(vectors["session_id"].get_str())};
    BOOST_CHECK_EQUAL(holepunch::FEATURE_ID, vectors["feature_id"].get_str());

    for (const auto& vector : vectors["pairs"].getValues()) {
        const std::array descriptors{Descriptor(vector["descriptors"][0]), Descriptor(vector["descriptors"][1])};
        const std::array keys{Secret(vector["secrets"][0].get_str()), Secret(vector["secrets"][1].get_str())};
        for (size_t side{0}; side < 2; ++side) {
            const auto& expected = vector["descriptors"][side];
            const auto& local = descriptors[side];
            const auto& remote = descriptors[1 - side];
            BOOST_CHECK_EQUAL(local.endpoint.ToStringAddr(), expected["ipv4"].get_str());
            BOOST_CHECK_EQUAL(local.epoch, vectors["epoch"].getInt<uint32_t>());
            BOOST_CHECK_EQUAL(HexStr(local.Payload()), expected["addr_payload"].get_str());
            BOOST_CHECK_EQUAL(HexStr(local.pubkey), expected["public_key"].get_str());
            BOOST_CHECK_EQUAL(HexStr(local.Bytes(chain)), expected["D"].get_str());
            BOOST_CHECK_EQUAL(HexStr(local.Id(chain)), expected["id"].get_str());
            ECDHSecret shared;
            BOOST_REQUIRE(keys[side].ComputeECDHX(remote.pubkey, shared));
            BOOST_CHECK_EQUAL(HexStr(shared), vector["keys"]["z"].get_str());

            const auto pair = holepunch::DerivePair(keys[side], local, remote, chain);
            BOOST_REQUIRE(pair);
            BOOST_CHECK_EQUAL(pair->initiating, vector["roles"][side].getInt<int>() == 0);
            BOOST_CHECK_EQUAL(HexStr(pair->schedule_key), vector["keys"]["schedule"].get_str());
            BOOST_CHECK_EQUAL(HexStr(pair->auth_key), vector["keys"]["auth"].get_str());
            for (const auto& round_vector : vector["rounds"].getValues()) {
                const auto round = round_vector["round"].getInt<uint64_t>();
                const auto ranking = holepunch::RankPair(local, remote, chain, round);
                BOOST_REQUIRE(ranking);
                BOOST_CHECK_EQUAL(HexStr(ranking->draw), round_vector["draw"].get_str());
                BOOST_CHECK_EQUAL(ranking->eligible, round_vector["eligible"].get_bool());
                BOOST_CHECK_EQUAL(HexStr(ranking->priority), round_vector["priority"].get_str());
                BOOST_CHECK_EQUAL(HexStr(ranking->endpoint_priority), round_vector["endpoint_priority"].get_str());
                BOOST_CHECK_EQUAL(HexStr(ranking->tie_break), "0101010108080808");
                BOOST_CHECK_EQUAL(HexStr(ranking->endpoint_tie_break), "01010101208d08080808480c");
                const auto schedule = holepunch::Schedule(*pair, round);
                BOOST_REQUIRE(schedule);
                BOOST_CHECK_EQUAL(schedule->eligible, round_vector["eligible"].get_bool());
                BOOST_CHECK_EQUAL(schedule->start_ms, round_vector["start_ms"].getInt<uint64_t>());
                BOOST_CHECK_EQUAL(schedule->start_ms % 60000, round_vector["offset_ms"].getInt<uint64_t>());
                BOOST_CHECK_EQUAL(HexStr(schedule->priority), round_vector["priority"].get_str());
                BOOST_CHECK_EQUAL(HexStr(schedule->endpoint_priority), round_vector["endpoint_priority"].get_str());
            }
            BOOST_CHECK(!holepunch::Schedule(*pair, local.epoch * 60ULL - 1));
            BOOST_CHECK(!holepunch::Schedule(*pair, (local.epoch + 1) * 60ULL));

            const auto round = vector["proof_round"].getInt<uint64_t>();
            const auto proof = holepunch::MakeProof(*pair, round, session_id);
            BOOST_CHECK_EQUAL(HexStr(proof), vector["featuredata"][side].get_str());
            const auto peer_proof = ParseHex(vector["featuredata"][1 - side].get_str());
            BOOST_CHECK(holepunch::CheckProof(peer_proof, *pair, round, session_id));
            BOOST_CHECK(!holepunch::CheckProof(proof, *pair, round, session_id));
            BOOST_CHECK(!holepunch::CheckProof(peer_proof, *pair, round + 1, session_id));
            BOOST_CHECK(!holepunch::CheckProof(peer_proof, *pair, round, uint256{}));
            for (size_t i{0}; i < peer_proof.size(); ++i) {
                auto corrupt = peer_proof;
                corrupt[i] ^= 1;
                BOOST_CHECK(!holepunch::CheckProof(corrupt, *pair, round, session_id));
            }
            auto wrong_size = peer_proof;
            wrong_size.push_back(0);
            BOOST_CHECK(!holepunch::CheckProof(wrong_size, *pair, round, session_id));
            wrong_size.resize(holepunch::PROOF_SIZE - 1);
            BOOST_CHECK(!holepunch::CheckProof(wrong_size, *pair, round, session_id));
        }
    }
}

BOOST_AUTO_TEST_CASE(public_ranking_invariants)
{
    const auto vectors = Vectors();
    const auto local = Descriptor(vectors["pairs"][0]["descriptors"][0]);
    const auto remote = Descriptor(vectors["pairs"][0]["descriptors"][1]);
    const auto key = Secret(vectors["pairs"][0]["secrets"][0].get_str());
    const uint256 chain{ParseHex(vectors["chain"].get_str())};
    const uint256 session_id{ParseHex(vectors["session_id"].get_str())};
    const auto pair = holepunch::DerivePair(key, local, remote, chain);
    BOOST_REQUIRE(pair);

    auto changed_key = remote;
    changed_key.pubkey = Descriptor(vectors["pairs"][1]["descriptors"][1]).pubkey;
    auto changed_port = remote;
    changed_port.endpoint = CService{remote.endpoint, 8333};
    auto changed_both = changed_key;
    changed_both.endpoint = changed_port.endpoint;
    for (const auto& changed : {changed_key, changed_port, changed_both}) {
        const auto changed_pair = holepunch::DerivePair(key, local, changed, chain);
        BOOST_REQUIRE(changed_pair);
        BOOST_CHECK(changed_pair->schedule_key != pair->schedule_key);
        BOOST_CHECK(changed_pair->auth_key != pair->auth_key);
        for (uint64_t round{local.epoch * 60ULL}; round < (local.epoch + 1) * 60ULL; ++round) {
            const auto original = holepunch::RankPair(local, remote, chain, round);
            const auto ranking = holepunch::RankPair(local, changed, chain, round);
            const auto reverse = holepunch::RankPair(changed, local, chain, round);
            BOOST_REQUIRE(original);
            BOOST_REQUIRE(ranking);
            BOOST_REQUIRE(reverse);
            BOOST_CHECK_EQUAL(ranking->eligible, original->eligible);
            BOOST_CHECK(ranking->draw == original->draw);
            BOOST_CHECK(ranking->priority == original->priority);
            BOOST_CHECK(ranking->tie_break == original->tie_break);
            BOOST_CHECK_EQUAL(ranking->endpoint_priority == original->endpoint_priority, changed.endpoint == remote.endpoint);
            BOOST_CHECK(ranking->draw == reverse->draw);
            BOOST_CHECK(ranking->priority == reverse->priority);
            BOOST_CHECK(ranking->endpoint_priority == reverse->endpoint_priority);
            BOOST_CHECK(ranking->endpoint_tie_break == reverse->endpoint_tie_break);
        }
        const auto round = vectors["pairs"][0]["proof_round"].getInt<uint64_t>();
        BOOST_CHECK(holepunch::MakeProof(*changed_pair, round, session_id) != holepunch::MakeProof(*pair, round, session_id));
    }

    const uint64_t round{local.epoch * 60ULL};
    const auto original = holepunch::RankPair(local, remote, chain, round);
    auto changed_ip = remote;
    changed_ip.endpoint = LookupNumeric("9.9.9.9", remote.endpoint.GetPort());
    for (const auto& ranking : {holepunch::RankPair(local, changed_ip, chain, round),
                                holepunch::RankPair(local, remote, uint256{}, round),
                                holepunch::RankPair(local, remote, chain, round + 1)}) {
        BOOST_REQUIRE(ranking);
        BOOST_CHECK(ranking->draw != original->draw);
        BOOST_CHECK(ranking->priority != original->priority);
        BOOST_CHECK(ranking->endpoint_priority != original->endpoint_priority);
    }
    BOOST_CHECK(!holepunch::RankPair(local, remote, chain, round - 1));
    BOOST_CHECK(!holepunch::RankPair(local, remote, chain, round + 60));
}

BOOST_AUTO_TEST_CASE(exact_schedule_boundaries)
{
    const auto vectors = Vectors();
    for (const auto& vector : vectors["eligibility_boundaries"].getValues()) {
        BOOST_CHECK_EQUAL(holepunch::IsEligible(Bytes32(vector["draw"].get_str())), vector["eligible"].get_bool());
    }
    for (const auto& vector : vectors["offset_boundaries"].getValues()) {
        BOOST_CHECK_EQUAL(holepunch::OffsetMillis(Bytes32(vector["time_draw"].get_str())), vector["offset_ms"].getInt<uint32_t>());
    }
}

BOOST_AUTO_TEST_CASE(fast_profile)
{
    const auto vectors = Vectors();
    const auto& profile = holepunch::GetProfile(holepunch::FAST_FORMAT);
    BOOST_CHECK_EQUAL(profile.round_seconds, 30);
    BOOST_CHECK_EQUAL(profile.epoch_seconds, 60);
    BOOST_CHECK(profile.always_eligible);
    const std::string feature_domain{std::string{profile.domain} + "/feature"};
    holepunch::Bytes32 feature_hash;
    CSHA256{}.Write(reinterpret_cast<const unsigned char*>(feature_domain.data()), feature_domain.size()).Finalize(feature_hash.data());
    BOOST_CHECK_EQUAL(HexStr(feature_hash), profile.feature_id);
    BOOST_CHECK(profile.feature_id != holepunch::FEATURE_ID);

    holepunch::Bytes32 draw{};
    BOOST_CHECK_EQUAL(holepunch::OffsetMillis(draw, profile.format), 5000);
    draw[0] = 0x80;
    BOOST_CHECK_EQUAL(holepunch::OffsetMillis(draw, profile.format), 7500);
    draw.fill(0xff);
    BOOST_CHECK_EQUAL(holepunch::OffsetMillis(draw, profile.format), 9999);
    BOOST_CHECK(holepunch::IsEligible(draw, profile.format));

    const std::array keys{Secret(vectors["pairs"][0]["secrets"][0].get_str()), Secret(vectors["pairs"][0]["secrets"][1].get_str())};
    const uint256 chain{ParseHex(vectors["chain"].get_str())};
    std::array descriptors{Descriptor(vectors["pairs"][0]["descriptors"][0]), Descriptor(vectors["pairs"][0]["descriptors"][1])};
    const auto draft_pair = holepunch::DerivePair(keys[0], descriptors[0], descriptors[1], chain);
    BOOST_REQUIRE(draft_pair);
    descriptors[0].format = profile.format;
    BOOST_CHECK(!holepunch::DerivePair(keys[0], descriptors[0], descriptors[1], chain));
    descriptors[1].format = profile.format;
    const auto fast_pair = holepunch::DerivePair(keys[0], descriptors[0], descriptors[1], chain);
    BOOST_REQUIRE(fast_pair);
    BOOST_CHECK(fast_pair->auth_key != draft_pair->auth_key);
    BOOST_CHECK(fast_pair->schedule_key != draft_pair->schedule_key);
    BOOST_CHECK(fast_pair->local_id != draft_pair->local_id);

    // Local testing permits different ports on the same loopback address.
    descriptors[0].endpoint = LookupNumeric("127.0.0.1", 8333);
    descriptors[1].endpoint = LookupNumeric("127.0.0.1", 18444);
    const std::array pairs{holepunch::DerivePair(keys[0], descriptors[0], descriptors[1], chain),
                           holepunch::DerivePair(keys[1], descriptors[1], descriptors[0], chain)};
    BOOST_REQUIRE(pairs[0]);
    BOOST_REQUIRE(pairs[1]);
    BOOST_CHECK(pairs[0]->initiating != pairs[1]->initiating);
    BOOST_CHECK(pairs[0]->schedule_key == pairs[1]->schedule_key);
    BOOST_CHECK(pairs[0]->auth_key == pairs[1]->auth_key);
    const uint64_t first_round{descriptors[0].epoch * 2ULL};
    const uint256 session_id{ParseHex(vectors["session_id"].get_str())};
    for (size_t side{0}; side < 2; ++side) {
        const auto& local = descriptors[side];
        BOOST_CHECK(holepunch::Descriptor::Parse(local.Payload(), local.endpoint.GetPort()) == local);
        BOOST_CHECK_EQUAL(local.Payload()[0], holepunch::FAST_FORMAT);
        for (uint64_t round{first_round}; round < first_round + 2; ++round) {
            const auto schedule = holepunch::Schedule(*pairs[side], round);
            const auto peer_schedule = holepunch::Schedule(*pairs[1 - side], round);
            BOOST_REQUIRE(schedule);
            BOOST_REQUIRE(peer_schedule);
            BOOST_CHECK(schedule->eligible);
            BOOST_CHECK_EQUAL(schedule->start_ms, peer_schedule->start_ms);
            BOOST_CHECK(schedule->priority == peer_schedule->priority);
            BOOST_CHECK(schedule->tie_break == peer_schedule->tie_break);
            BOOST_CHECK_GE(schedule->start_ms % 30000, 5000);
            BOOST_CHECK_LT(schedule->start_ms % 30000, 10000);
            auto proof = holepunch::MakeProof(*pairs[side], round, session_id);
            BOOST_CHECK_EQUAL(proof[0], holepunch::FAST_FORMAT);
            BOOST_CHECK(holepunch::CheckProof(proof, *pairs[1 - side], round, session_id));
            BOOST_CHECK(!holepunch::CheckProof(proof, *draft_pair, round, session_id));
            proof[0] = 0;
            BOOST_CHECK(!holepunch::CheckProof(proof, *pairs[1 - side], round, session_id));
            proof[0] = 0xfe;
            BOOST_CHECK(!holepunch::CheckProof(proof, *pairs[1 - side], round, session_id));
        }
        BOOST_CHECK(!holepunch::Schedule(*pairs[side], first_round - 1));
        BOOST_CHECK(!holepunch::Schedule(*pairs[side], first_round + 2));
    }
}

BOOST_AUTO_TEST_CASE(fast_profile_endpoints)
{
    for (const auto address : {"127.0.0.1", "10.1.1.1", "192.168.1.1", "169.254.1.1", "192.0.2.1"}) {
        const auto endpoint = LookupNumeric(address, 8333);
        BOOST_CHECK(holepunch::IsValidEndpoint(endpoint, holepunch::FAST_FORMAT));
        BOOST_CHECK(!holepunch::IsValidEndpoint(endpoint, holepunch::FORMAT));
        const std::array observations{holepunch::Observation{endpoint, {1}}, holepunch::Observation{endpoint, {2}}};
        BOOST_CHECK(holepunch::FindObservedEndpoint(observations, holepunch::FAST_FORMAT) == endpoint);
        BOOST_CHECK(!holepunch::FindObservedEndpoint(observations));
    }
    for (const auto address : {"0.0.0.0", "0.1.1.1", "224.0.0.1", "240.0.0.1", "255.255.255.255", "::1"}) {
        BOOST_CHECK(!holepunch::IsValidEndpoint(LookupNumeric(address, 8333), holepunch::FAST_FORMAT));
    }
    BOOST_CHECK(!holepunch::IsValidEndpoint(LookupNumeric("127.0.0.1", 0), holepunch::FAST_FORMAT));
    BOOST_CHECK(!holepunch::IsValidEndpoint(LookupNumeric("1.1.1.1", 8333), 0));
    BOOST_CHECK(!holepunch::IsValidEndpoint(LookupNumeric("1.1.1.1", 8333), 0xfe));
}

BOOST_AUTO_TEST_CASE(invalid_descriptors_and_pairs)
{
    const auto vectors = Vectors();
    auto local = Descriptor(vectors["pairs"][0]["descriptors"][0]);
    auto remote = Descriptor(vectors["pairs"][0]["descriptors"][1]);
    const auto key = Secret(vectors["pairs"][0]["secrets"][0].get_str());
    const uint256 chain{ParseHex(vectors["chain"].get_str())};
    ECDHSecret shared;
    for (const auto& invalid_key : vectors["invalid_keys"].getValues()) {
        auto invalid = ParseHex(vectors["pairs"][0]["descriptors"][0]["addr_payload"].get_str());
        invalid.resize(9);
        const auto bytes = ParseHex(invalid_key.get_str());
        invalid.insert(invalid.end(), bytes.begin(), bytes.end());
        BOOST_CHECK(!holepunch::Descriptor::Parse(invalid, 8333));
        BOOST_CHECK(!key.ComputeECDHX(CPubKey{bytes}, shared));
        auto invalid_remote = remote;
        invalid_remote.pubkey = CPubKey{bytes};
        BOOST_CHECK(!holepunch::DerivePair(key, local, invalid_remote, chain));
    }
    BOOST_CHECK(!CKey{}.ComputeECDHX(remote.pubkey, shared));
    auto payload = local.Payload();
    BOOST_CHECK(!holepunch::Descriptor::Parse(payload, 0));
    for (const uint8_t format : {0, 2, 0xfe}) {
        payload[0] = format;
        BOOST_CHECK(!holepunch::Descriptor::Parse(payload, 8333));
    }
    for (const auto address : {"0.0.0.0", "127.0.0.1", "10.1.1.1", "100.64.0.1", "169.254.1.1", "192.168.1.1", "192.0.2.1", "198.18.0.1", "224.0.0.1", "240.0.0.1", "255.255.255.255"}) {
        auto invalid = local;
        invalid.endpoint = LookupNumeric(address, 8333);
        BOOST_CHECK(!holepunch::Descriptor::Parse(invalid.Payload(), 8333));
    }
    BOOST_CHECK(!holepunch::DerivePair(CKey{}, local, remote, chain));
    BOOST_CHECK(!holepunch::DerivePair(Secret(vectors["pairs"][0]["secrets"][1].get_str()), local, remote, chain));
    remote.epoch++;
    BOOST_CHECK(!holepunch::DerivePair(key, local, remote, chain));
    remote.epoch--;
    remote.pubkey = local.pubkey;
    BOOST_CHECK(!holepunch::DerivePair(key, local, remote, chain));
    remote = Descriptor(vectors["pairs"][0]["descriptors"][1]);
    remote.endpoint = local.endpoint;
    BOOST_CHECK(!holepunch::DerivePair(key, local, remote, chain));
    remote.endpoint = CService{local.endpoint, static_cast<uint16_t>(local.endpoint.GetPort() + 1)};
    BOOST_CHECK(!holepunch::DerivePair(key, local, remote, chain));
    BOOST_CHECK(!holepunch::RankPair(local, remote, chain, local.epoch * 60ULL));
}

BOOST_AUTO_TEST_CASE(retention_conflicts_and_expiry)
{
    const auto vectors = Vectors();
    auto first = Descriptor(vectors["pairs"][0]["descriptors"][0]);
    auto conflict = Descriptor(vectors["pairs"][1]["descriptors"][0]);
    auto second = Descriptor(vectors["pairs"][0]["descriptors"][1]);
    holepunch::DescriptorCache cache{2};
    BOOST_CHECK(cache.Insert(first, first.epoch));
    BOOST_CHECK(cache.Insert(first, first.epoch));
    BOOST_CHECK(!cache.Insert(conflict, first.epoch));
    BOOST_CHECK(cache.Contains(first));
    BOOST_CHECK(!cache.Contains(conflict));
    BOOST_CHECK(cache.Insert(second, first.epoch));
    BOOST_CHECK_EQUAL(cache.Size(), 2);
    auto third = first;
    third.endpoint = CService{third.endpoint, 18444};
    BOOST_CHECK(!cache.Insert(third, first.epoch));
    BOOST_CHECK(!cache.Contains(third));
    BOOST_CHECK(cache.Contains(first));
    BOOST_CHECK(cache.Contains(second));
    BOOST_CHECK_EQUAL(cache.Size(), 2);
    cache.Expire(first.epoch + 1);
    BOOST_CHECK_EQUAL(cache.Size(), 0);
    BOOST_CHECK(!cache.Insert(first, first.epoch + 1));
    conflict.epoch++;
    BOOST_CHECK(cache.Insert(conflict, conflict.epoch));
    first.epoch++;
    BOOST_CHECK(first.Id(uint256{}) != conflict.Id(uint256{}));
    cache.SetLocal(first);
    BOOST_CHECK(cache.Contains(first));
    BOOST_CHECK(!cache.Insert(conflict, first.epoch));
    BOOST_CHECK_EQUAL(cache.Size(), 0);
    BOOST_CHECK(cache.GetAll(first.epoch).empty());
    cache.Expire(first.epoch + 1);
    BOOST_CHECK(!cache.Contains(first));
    holepunch::DescriptorCache disabled{0};
    BOOST_CHECK(!disabled.Insert(first, first.epoch));
}

BOOST_AUTO_TEST_CASE(observer_agreement)
{
    const auto vectors = Vectors();
    const auto first = Descriptor(vectors["pairs"][0]["descriptors"][0]).endpoint;
    const auto second = Descriptor(vectors["pairs"][0]["descriptors"][1]).endpoint;
    std::vector<holepunch::Observation> observations{{first, {1}}, {first, {1}}};
    BOOST_CHECK(!holepunch::FindObservedEndpoint(observations));
    BOOST_CHECK(holepunch::FindObservedEndpoint(std::span{observations}.first(1), holepunch::FAST_FORMAT) == first);
    observations.push_back({first, {2}});
    BOOST_CHECK(holepunch::FindObservedEndpoint(observations) == first);
    observations.push_back({second, {3}});
    BOOST_CHECK(holepunch::FindObservedEndpoint(observations) == first);
    observations.push_back({second, {4}});
    BOOST_CHECK(!holepunch::FindObservedEndpoint(observations));
    observations.erase(observations.begin(), observations.begin() + 3);
    BOOST_CHECK(holepunch::FindObservedEndpoint(observations) == second);
    observations[1].endpoint = CService{observations[1].endpoint, 0};
    BOOST_CHECK(!holepunch::FindObservedEndpoint(observations));
    for (const auto address : {"127.0.0.1", "224.0.0.1", "240.0.0.1"}) {
        const auto invalid = LookupNumeric(address, 8333);
        observations = {{invalid, {1}}, {invalid, {2}}};
        BOOST_CHECK(!holepunch::FindObservedEndpoint(observations));
    }
}

BOOST_AUTO_TEST_SUITE_END()
