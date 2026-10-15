// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <holepunch.h>

#include <crypto/common.h>
#include <crypto/hmac_sha256.h>
#include <crypto/sha256.h>
#include <support/cleanse.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iterator>
#include <set>

namespace holepunch {

bool IsGlobalEndpoint(const CService& endpoint)
{
    // IsRoutable alone does not exclude IPv4 multicast and reserved space.
    return endpoint.IsIPv4() && endpoint.IsRoutable() && endpoint.GetPort() != 0 &&
           endpoint.GetLinkedIPv4() < 0xe0000000;
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

bool Descriptor::IsValid() const
{
    return IsGlobalEndpoint(endpoint) && pubkey.IsCompressed() && pubkey.IsFullyValid();
}

std::array<unsigned char, PAYLOAD_SIZE> Descriptor::Payload() const
{
    assert(endpoint.IsIPv4());
    assert(pubkey.IsCompressed());
    std::array<unsigned char, PAYLOAD_SIZE> payload{};
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
    if (payload.size() != PAYLOAD_SIZE || payload[0] != 0) return std::nullopt;
    in_addr ip;
    std::memcpy(&ip, payload.data() + 1, 4);
    Descriptor descriptor{CService{CNetAddr{ip}, port}, ReadLE32(payload.data() + 5), CPubKey{payload.subspan(9)}};
    if (!descriptor.IsValid()) return std::nullopt;
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
    if (!local.IsValid() || !remote.IsValid() || local.epoch != remote.epoch ||
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

    constexpr std::string_view domain{"BitcoinHolePunch/v0"};
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

bool IsEligible(const Bytes32& draw)
{
    return draw[0] < 8;
}

uint32_t OffsetMillis(const Bytes32& draw)
{
    // Base-256 multiplication retains the carry above the 256-bit product,
    // exactly floor(int256(draw) * 30000 / 2^256), without floating point.
    uint32_t carry{0};
    for (auto it = draw.rbegin(); it != draw.rend(); ++it) {
        carry = (uint32_t{*it} * 30000 + carry) >> 8;
    }
    return 5000 + carry;
}

std::optional<RoundSchedule> Schedule(const Pair& pair, uint64_t round)
{
    if (round / ROUNDS_PER_EPOCH != pair.local.epoch) return std::nullopt;
    return RoundSchedule{
        IsEligible(RoundHash(pair.schedule_key, "eligibility", round)),
        round,
        round * ROUND_SECONDS * 1000 + OffsetMillis(RoundHash(pair.schedule_key, "time", round)),
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

bool DescriptorCache::Insert(const Descriptor& descriptor, uint32_t epoch)
{
    Expire(epoch);
    if (descriptor.epoch != epoch || !descriptor.IsValid()) return false;
    if (const auto retained = Get(descriptor.endpoint)) return *retained == descriptor;
    if (m_capacity == 0) return false;
    if (m_remote.size() == m_capacity) {
        m_remote.erase(std::next(m_remote.begin(), m_rng.randrange(m_remote.size())));
    }
    m_remote.emplace(descriptor.endpoint, descriptor);
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
    return it->second;
}

std::vector<Descriptor> DescriptorCache::GetAll(uint32_t epoch) const
{
    std::vector<Descriptor> result;
    result.reserve(m_remote.size());
    for (const auto& [endpoint, descriptor] : m_remote) {
        if (descriptor.epoch == epoch) result.push_back(descriptor);
    }
    return result;
}

void DescriptorCache::Expire(uint32_t epoch)
{
    std::erase_if(m_remote, [epoch](const auto& entry) { return entry.second.epoch != epoch; });
    if (m_local && m_local->epoch != epoch) m_local.reset();
}

void DescriptorCache::SetLocal(std::optional<Descriptor> descriptor)
{
    if (descriptor) {
        assert(descriptor->IsValid());
        m_remote.erase(descriptor->endpoint);
    }
    m_local = std::move(descriptor);
}

std::optional<CService> FindObservedEndpoint(std::span<const Observation> observations)
{
    std::map<CService, std::set<std::vector<unsigned char>>> groups;
    for (const auto& observation : observations) {
        if (IsGlobalEndpoint(observation.endpoint)) {
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
