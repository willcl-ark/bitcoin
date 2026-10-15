// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <holepunch.h>

#include <crypto/common.h>
#include <crypto/hmac_sha256.h>
#include <crypto/sha256.h>
#include <crypto/siphash.h>
#include <support/cleanse.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <set>

namespace holepunch {

const Profile& GetProfile(uint8_t format)
{
    static constexpr Profile draft{FORMAT, ROUND_SECONDS, EPOCH_SECONDS, 30000, false, "BitcoinHolePunch/v1", FEATURE_ID};
    static constexpr Profile fast{FAST_FORMAT, 30, 60, 5000, true, "BitcoinHolePunch/test-fast/v1",
                                  "3e4f5bc6bad88ba8ec2be4c54dcdbef6f17b08884ad5ae1884ac2f4eabc547b0"};
    assert(IsSupportedFormat(format));
    return format == FAST_FORMAT ? fast : draft;
}

bool IsGlobalEndpoint(const CService& endpoint)
{
    // IsRoutable alone does not exclude IPv4 multicast and reserved space.
    return endpoint.IsIPv4() && endpoint.IsRoutable() && endpoint.GetPort() != 0 &&
           endpoint.GetLinkedIPv4() < 0xe0000000;
}

bool IsValidEndpoint(const CService& endpoint, uint8_t format)
{
    if (format == FORMAT) return IsGlobalEndpoint(endpoint);
    return format == FAST_FORMAT && endpoint.IsIPv4() && endpoint.GetPort() != 0 &&
           endpoint.GetLinkedIPv4() >= 0x01000000 && endpoint.GetLinkedIPv4() < 0xe0000000;
}

namespace {

bool IsValidPair(const Descriptor& local, const Descriptor& remote)
{
    return local.IsValidStructure() && remote.IsValidStructure() && local.epoch == remote.epoch &&
           local.format == remote.format && local.endpoint != remote.endpoint && local.pubkey != remote.pubkey &&
           (local.format == FAST_FORMAT || local.endpoint.GetLinkedIPv4() != remote.endpoint.GetLinkedIPv4());
}

Bytes32 RoundHash(const Bytes32& key, std::string_view label, uint64_t round)
{
    std::array<unsigned char, 8> encoded_round;
    WriteLE64(encoded_round.data(), round);
    Bytes32 result;
    CHMAC_SHA256{key.data(), key.size()}
        .Write(reinterpret_cast<const unsigned char*>(label.data()), label.size())
        .Write(encoded_round.data(), encoded_round.size())
        .Finalize(result.data());
    return result;
}

std::array<unsigned char, PROOF_SIZE> Proof(const Pair& pair, uint64_t round, const uint256& session_id, bool local)
{
    std::array<unsigned char, PROOF_SIZE> result{};
    result[0] = pair.local.format;
    WriteLE64(result.data() + 1, round);
    const auto& sender = local ? pair.local_id : pair.remote_id;
    const auto& receiver = local ? pair.remote_id : pair.local_id;
    std::copy(sender.begin(), sender.end(), result.begin() + 9);
    std::copy(receiver.begin(), receiver.end(), result.begin() + 41);
    const unsigned char role = (local == pair.initiating) ? 0 : 1;
    constexpr std::string_view label{"connection"};
    CHMAC_SHA256{pair.auth_key.data(), pair.auth_key.size()}
        .Write(reinterpret_cast<const unsigned char*>(label.data()), label.size())
        .Write(session_id.data(), session_id.size())
        .Write(result.data() + 1, 72)
        .Write(&role, 1)
        .Finalize(result.data() + 73);
    return result;
}

} // namespace

bool Descriptor::IsValidStructure() const
{
    return IsValidEndpoint(endpoint, format) && pubkey.IsCompressed();
}

bool Descriptor::IsValid() const
{
    return IsValidStructure() && pubkey.IsFullyValid();
}

std::array<unsigned char, PAYLOAD_SIZE> Descriptor::Payload() const
{
    assert(endpoint.IsIPv4());
    assert(pubkey.IsCompressed());
    std::array<unsigned char, PAYLOAD_SIZE> payload{};
    payload[0] = format;
    in_addr ip;
    const bool ipv4 = endpoint.GetInAddr(&ip);
    assert(ipv4);
    std::memcpy(payload.data() + 1, &ip, 4);
    WriteLE32(payload.data() + 5, epoch);
    std::copy(pubkey.begin(), pubkey.end(), payload.begin() + 9);
    return payload;
}

std::optional<Descriptor> Descriptor::Parse(std::span<const unsigned char> payload, uint16_t port)
{
    auto descriptor{ParseUnchecked(payload, port)};
    if (!descriptor || !descriptor->pubkey.IsFullyValid()) return std::nullopt;
    return descriptor;
}

std::optional<Descriptor> Descriptor::ParseUnchecked(std::span<const unsigned char> payload, uint16_t port)
{
    if (payload.size() != PAYLOAD_SIZE || !IsSupportedFormat(payload[0])) return std::nullopt;
    in_addr ip;
    std::memcpy(&ip, payload.data() + 1, 4);
    Descriptor descriptor{CService{CNetAddr{ip}, port}, ReadLE32(payload.data() + 5), CPubKey{payload.subspan(9)}, payload[0]};
    if (!descriptor.IsValidStructure()) return std::nullopt;
    return descriptor;
}

std::array<unsigned char, 76> Descriptor::Bytes(const uint256& genesis_hash) const
{
    std::array<unsigned char, 76> result;
    std::copy(genesis_hash.begin(), genesis_hash.end(), result.begin());
    const auto payload = Payload();
    std::copy(payload.begin(), payload.end(), result.begin() + 32);
    WriteBE16(result.data() + 74, endpoint.GetPort());
    return result;
}

uint256 Descriptor::Id(const uint256& genesis_hash) const
{
    const auto bytes = Bytes(genesis_hash);
    uint256 id;
    CSHA256{}.Write(bytes.data(), bytes.size()).Finalize(id.data());
    return id;
}

Pair::~Pair()
{
    memory_cleanse(schedule_key.data(), schedule_key.size());
    memory_cleanse(auth_key.data(), auth_key.size());
}

std::optional<Pair> DerivePair(const CKey& key, const Descriptor& local, const Descriptor& remote, const uint256& genesis_hash)
{
    if (!IsValidPair(local, remote) || !key.IsValid() || key.GetPubKey() != local.pubkey) return std::nullopt;
    Pair pair;
    pair.local = local;
    pair.remote = remote;
    pair.local_id = local.Id(genesis_hash);
    pair.remote_id = remote.Id(genesis_hash);
    pair.genesis_hash = genesis_hash;
    pair.initiating = local.pubkey < remote.pubkey;
    const auto lo = (pair.initiating ? local : remote).Bytes(genesis_hash);
    const auto hi = (pair.initiating ? remote : local).Bytes(genesis_hash);
    const auto domain = GetProfile(local.format).domain;
    Bytes32 salt;
    CSHA256{}.Write(reinterpret_cast<const unsigned char*>(domain.data()), domain.size())
        .Write(lo.data(), lo.size()).Write(hi.data(), hi.size()).Finalize(salt.data());
    ECDHSecret secret;
    if (!key.ComputeECDHX(remote.pubkey, secret)) return std::nullopt;
    Bytes32 prk;
    CHMAC_SHA256{salt.data(), salt.size()}.Write(UCharCast(secret.data()), secret.size()).Finalize(prk.data());
    memory_cleanse(secret.data(), secret.size());
    // Each HKDF output is exactly one SHA256 block: HMAC(PRK, info || 0x01).
    const unsigned char counter{1};
    const auto expand = [&](std::string_view info, Bytes32& output) {
        CHMAC_SHA256{prk.data(), prk.size()}.Write(reinterpret_cast<const unsigned char*>(info.data()), info.size())
            .Write(&counter, 1).Finalize(output.data());
    };
    expand("schedule", pair.schedule_key);
    expand("authentication", pair.auth_key);
    memory_cleanse(prk.data(), prk.size());
    return pair;
}

bool IsEligible(const Bytes32& draw, uint8_t format)
{
    return GetProfile(format).always_eligible || draw[0] < 8;
}

uint32_t OffsetMillis(const Bytes32& draw, uint8_t format)
{
    // Base-256 multiplication retains the carry above the 256-bit product,
    // exactly floor(int256(draw) * range / 2^256), without floating point.
    uint32_t carry{0};
    const uint32_t range = GetProfile(format).offset_range;
    for (auto it = draw.rbegin(); it != draw.rend(); ++it) {
        carry = (uint32_t{*it} * range + carry) >> 8;
    }
    return 5000 + carry;
}

std::optional<PublicSchedule> RankPair(const Descriptor& local, const Descriptor& remote, const uint256& genesis_hash, uint64_t round)
{
    if (!IsValidPair(local, remote)) return std::nullopt;
    const auto& profile = GetProfile(local.format);
    if (round / (profile.epoch_seconds / profile.round_seconds) != local.epoch) return std::nullopt;

    std::array<unsigned char, 6> local_endpoint, remote_endpoint;
    WriteBE32(local_endpoint.data(), local.endpoint.GetLinkedIPv4());
    WriteBE16(local_endpoint.data() + 4, local.endpoint.GetPort());
    WriteBE32(remote_endpoint.data(), remote.endpoint.GetLinkedIPv4());
    WriteBE16(remote_endpoint.data() + 4, remote.endpoint.GetPort());
    const auto& lo = std::min(local_endpoint, remote_endpoint);
    const auto& hi = std::max(local_endpoint, remote_endpoint);
    PublicSchedule result;
    std::copy_n(lo.begin(), 4, result.tie_break.begin());
    std::copy_n(hi.begin(), 4, result.tie_break.begin() + 4);
    std::copy(lo.begin(), lo.end(), result.endpoint_tie_break.begin());
    std::copy(hi.begin(), hi.end(), result.endpoint_tie_break.begin() + lo.size());

    std::array<unsigned char, 8> encoded_round;
    WriteLE64(encoded_round.data(), round);
    const auto hash = [&](std::string_view label, std::span<const unsigned char> endpoints, Bytes32& output) {
        CSHA256{}.Write(reinterpret_cast<const unsigned char*>(profile.domain.data()), profile.domain.size())
            .Write(reinterpret_cast<const unsigned char*>(label.data()), label.size())
            .Write(genesis_hash.data(), genesis_hash.size())
            .Write(encoded_round.data(), encoded_round.size())
            .Write(endpoints.data(), endpoints.size()).Finalize(output.data());
    };
    hash("/eligibility", result.tie_break, result.draw);
    result.eligible = IsEligible(result.draw, profile.format);
    hash("/priority", result.tie_break, result.priority);
    hash("/endpoint", result.endpoint_tie_break, result.endpoint_priority);
    return result;
}

std::optional<RoundSchedule> Schedule(const Pair& pair, uint64_t round)
{
    const auto& profile = GetProfile(pair.local.format);
    const auto ranking = RankPair(pair.local, pair.remote, pair.genesis_hash, round);
    if (!ranking) return std::nullopt;
    return RoundSchedule{
        ranking->eligible,
        round,
        round * profile.round_seconds * 1000 + OffsetMillis(RoundHash(pair.schedule_key, "time", round), profile.format),
        ranking->priority,
        ranking->tie_break,
        ranking->endpoint_priority,
        ranking->endpoint_tie_break,
    };
}

std::array<unsigned char, PROOF_SIZE> MakeProof(const Pair& pair, uint64_t round, const uint256& session_id)
{
    return Proof(pair, round, session_id, true);
}

bool CheckProof(std::span<const unsigned char> proof, const Pair& pair, uint64_t round, const uint256& session_id)
{
    if (proof.size() != PROOF_SIZE) return false;
    const auto expected = Proof(pair, round, session_id, false);
    return TimingResistantEqual(proof, std::span<const unsigned char>{expected});
}

bool DescriptorCache::Insert(const Descriptor& descriptor, uint32_t epoch,
                            const std::vector<unsigned char>& source_group,
                            const std::vector<unsigned char>& destination_group)
{
    Expire(epoch);
    if (descriptor.epoch != epoch || !descriptor.IsValidStructure()) return false;
    if (const auto retained = Get(descriptor.endpoint)) return *retained == descriptor;
    if (m_remote.size() >= m_capacity) return false;
    // Neither epoch nor advertised key gives a sender another placement choice.
    const auto bucket{CSipHasher{m_salt0, m_salt1}.Write(source_group.size()).Write(source_group)
                          .Write(descriptor.endpoint.GetKey()).Finalize() % ((m_capacity + BUCKET_SIZE - 1) / BUCKET_SIZE)};
    size_t sources{0}, destinations{0}, occupants{0};
    for (const auto& [endpoint, entry] : m_remote) {
        sources += entry.source_group == source_group;
        destinations += entry.destination_group == destination_group;
        occupants += entry.bucket == bucket;
    }
    if (sources >= GROUP_LIMIT || destinations >= GROUP_LIMIT || occupants >= BUCKET_SIZE) return false;
    m_remote.emplace(descriptor.endpoint, Entry{descriptor, source_group, destination_group, bucket});
    return true;
}

bool DescriptorCache::Contains(const Descriptor& descriptor) const
{
    const auto retained = Get(descriptor.endpoint);
    return retained && *retained == descriptor;
}

std::optional<Descriptor> DescriptorCache::Get(const CService& endpoint) const
{
    if (m_local && m_local->endpoint == endpoint) return m_local;
    const auto it = m_remote.find(endpoint);
    if (it == m_remote.end()) return std::nullopt;
    return it->second.descriptor;
}

std::vector<unsigned char> DescriptorCache::SourceGroup(const CService& endpoint) const
{
    const auto it{m_remote.find(endpoint)};
    return it == m_remote.end() ? std::vector<unsigned char>{} : it->second.source_group;
}

std::vector<Descriptor> DescriptorCache::GetAll(uint32_t epoch) const
{
    std::vector<Descriptor> result;
    result.reserve(m_remote.size());
    for (const auto& [endpoint, entry] : m_remote) {
        if (entry.descriptor.epoch == epoch) result.push_back(entry.descriptor);
    }
    return result;
}

void DescriptorCache::Expire(uint32_t epoch)
{
    std::erase_if(m_remote, [epoch](const auto& entry) { return entry.second.descriptor.epoch != epoch; });
    if (m_local && m_local->epoch != epoch) m_local.reset();
}

void DescriptorCache::SetLocal(std::optional<Descriptor> descriptor)
{
    if (descriptor) {
        assert(descriptor->IsValidStructure());
        m_remote.erase(descriptor->endpoint);
    }
    m_local = std::move(descriptor);
}

std::optional<CService> FindObservedEndpoint(std::span<const Observation> observations, uint8_t format)
{
    std::map<CService, std::set<std::vector<unsigned char>>> groups;
    for (const auto& observation : observations) {
        if (IsValidEndpoint(observation.endpoint, format)) {
            groups[observation.endpoint].insert(observation.network_group);
        }
    }
    std::optional<CService> endpoint;
    for (const auto& [reported, sources] : groups) {
        if (sources.size() < 2) continue;
        if (endpoint) return std::nullopt;
        endpoint = reported;
    }
    return endpoint;
}

} // namespace holepunch
