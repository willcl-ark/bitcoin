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
    static constexpr Profile draft{0, ROUND_SECONDS, EPOCH_SECONDS, 30000, false, "BitcoinHolePunch/v0", FEATURE_ID};
    static constexpr Profile fast{FAST_FORMAT, 30, 60, 5000, true, "BitcoinHolePunch/test-fast/v0",
                                  "5282aeae4c7dbd76b35765093623f374efd0318378da760927c4d47cb0c019c5"};
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
    if (format == 0) return IsGlobalEndpoint(endpoint);
    return format == FAST_FORMAT && endpoint.IsIPv4() && endpoint.GetPort() != 0 &&
           endpoint.GetLinkedIPv4() >= 0x01000000 && endpoint.GetLinkedIPv4() < 0xe0000000;
}

namespace {

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
    if (!local.IsValid() || !remote.IsValid() || local.epoch != remote.epoch || local.format != remote.format ||
        local.endpoint == remote.endpoint || local.pubkey == remote.pubkey ||
        !key.IsValid() || key.GetPubKey() != local.pubkey) return std::nullopt;
    Pair pair;
    pair.local = local;
    pair.remote = remote;
    pair.local_id = local.Id(genesis_hash);
    pair.remote_id = remote.Id(genesis_hash);
    pair.initiating = local.pubkey < remote.pubkey;
    const auto lo = (pair.initiating ? local : remote).Bytes(genesis_hash);
    const auto hi = (pair.initiating ? remote : local).Bytes(genesis_hash);
    std::copy(lo.begin(), lo.end(), pair.tie_break.begin());
    std::copy(hi.begin(), hi.end(), pair.tie_break.begin() + lo.size());

    const auto domain = GetProfile(local.format).domain;
    Bytes32 salt;
    CSHA256{}.Write(reinterpret_cast<const unsigned char*>(domain.data()), domain.size())
        .Write(pair.tie_break.data(), pair.tie_break.size()).Finalize(salt.data());
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

std::optional<RoundSchedule> Schedule(const Pair& pair, uint64_t round)
{
    const auto& profile = GetProfile(pair.local.format);
    if (round / (profile.epoch_seconds / profile.round_seconds) != pair.local.epoch) return std::nullopt;
    return RoundSchedule{
        IsEligible(RoundHash(pair.schedule_key, "eligibility", round), profile.format),
        round,
        round * profile.round_seconds * 1000 + OffsetMillis(RoundHash(pair.schedule_key, "time", round), profile.format),
        RoundHash(pair.schedule_key, "priority", round),
        pair.tie_break,
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
