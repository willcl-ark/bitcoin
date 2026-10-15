// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_HOLEPUNCH_H
#define BITCOIN_HOLEPUNCH_H

#include <key.h>
#include <netaddress.h>
#include <pubkey.h>
#include <random.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/** Experimental coordinatorless IPv4 TCP hole punching, version 1. */
namespace holepunch {

inline constexpr uint8_t FORMAT{1};
inline constexpr uint64_t ROUND_SECONDS{60};
inline constexpr uint64_t EPOCH_SECONDS{3600};
inline constexpr size_t PAYLOAD_SIZE{42};
inline constexpr size_t PROOF_SIZE{105};
inline constexpr std::string_view FEATURE_ID{"a3f13a7f6c39ee3c031aa28cffd55bc3cdbf7be05e5c0a0264c6b277af0346bc"};
inline constexpr uint8_t FAST_FORMAT{0xfd};
using Bytes32 = std::array<unsigned char, 32>;

/** Two fixed, incompatible profiles: the draft and an accelerated local test. */
struct Profile {
    uint8_t format;
    uint64_t round_seconds;
    uint64_t epoch_seconds;
    uint32_t offset_range; //!< Number of millisecond offsets, starting at 5,000 ms.
    bool always_eligible;
    std::string_view domain;
    std::string_view feature_id;
};

constexpr bool IsSupportedFormat(uint8_t format) { return format == FORMAT || format == FAST_FORMAT; }
/** The caller must check IsSupportedFormat before looking up untrusted input. */
const Profile& GetProfile(uint8_t format);

/** Globally routable IPv4 address with a nonzero TCP port. */
bool IsGlobalEndpoint(const CService& endpoint);
/** The fast test profile additionally permits private and loopback unicast IPv4. */
bool IsValidEndpoint(const CService& endpoint, uint8_t format);

struct Descriptor {
    CService endpoint;
    uint32_t epoch{0};
    CPubKey pubkey;
    uint8_t format{FORMAT};

    /** Cheap format, destination, and compressed-key encoding checks only. */
    bool IsValidStructure() const;
    bool IsValid() const;
    std::array<unsigned char, PAYLOAD_SIZE> Payload() const;
    static std::optional<Descriptor> Parse(std::span<const unsigned char> payload, uint16_t port);
    /** The caller must budget and validate the curve point before admitting this record. */
    static std::optional<Descriptor> ParseUnchecked(std::span<const unsigned char> payload, uint16_t port);
    /** genesis_hash is the chain's ordinary uint256 block hash, whose bytes are the raw digest. */
    std::array<unsigned char, 76> Bytes(const uint256& genesis_hash) const;
    uint256 Id(const uint256& genesis_hash) const;
    bool operator==(const Descriptor&) const = default;
};

/** Inputs and secrets pinned for an attempt; never change these after scheduling. */
struct Pair {
    Descriptor local;
    Descriptor remote;
    uint256 local_id;
    uint256 remote_id;
    uint256 genesis_hash;
    bool initiating{false};
    Bytes32 schedule_key{};
    Bytes32 auth_key{};
    ~Pair();
};

std::optional<Pair> DerivePair(const CKey& key, const Descriptor& local, const Descriptor& remote, const uint256& genesis_hash);

/** Public IPv4-group and endpoint rankings, independent of descriptor keys. */
struct PublicSchedule {
    bool eligible;
    Bytes32 draw;
    Bytes32 priority;
    std::array<unsigned char, 8> tie_break;
    Bytes32 endpoint_priority;
    std::array<unsigned char, 12> endpoint_tie_break;
};

/** Cheap ranking before ECDH. Callers must validate descriptor keys separately. */
std::optional<PublicSchedule> RankPair(const Descriptor& local, const Descriptor& remote, const uint256& genesis_hash, uint64_t round);

struct RoundSchedule {
    bool eligible;
    uint64_t round;
    uint64_t start_ms;
    Bytes32 priority;
    std::array<unsigned char, 8> tie_break;
    Bytes32 endpoint_priority;
    std::array<unsigned char, 12> endpoint_tie_break;
};

bool IsEligible(const Bytes32& draw, uint8_t format = FORMAT);
uint32_t OffsetMillis(const Bytes32& draw, uint8_t format = FORMAT);
/** Returns nullopt if the round is outside the pair's epoch. */
std::optional<RoundSchedule> Schedule(const Pair& pair, uint64_t round);
std::array<unsigned char, PROOF_SIZE> MakeProof(const Pair& pair, uint64_t round, const uint256& session_id);
/** Validate the remote peer's proof against the pinned pair and BIP324 session. */
bool CheckProof(std::span<const unsigned char> proof, const Pair& pair, uint64_t round, const uint256& session_id);

/** Nonpersistent, first-admitted-per-endpoint cache. Callers supply synchronization. */
class DescriptorCache {
    struct Entry {
        Descriptor descriptor;
        std::vector<unsigned char> source_group;
        std::vector<unsigned char> destination_group;
        size_t bucket;
    };
    static constexpr size_t BUCKET_SIZE{16};
    static constexpr size_t GROUP_LIMIT{32};
    const size_t m_capacity;
    const uint64_t m_salt0{FastRandomContext{}.rand64()};
    const uint64_t m_salt1{FastRandomContext{}.rand64()};
    std::map<CService, Entry> m_remote;
    std::optional<Descriptor> m_local;

public:
    explicit DescriptorCache(size_t capacity = 1024) : m_capacity{capacity} {}
    /** Admit a previously curve-validated descriptor, subject to private placement and group limits.
     * Occupied buckets never replace retained entries; expiry permits fresh admission. */
    bool Insert(const Descriptor& descriptor, uint32_t epoch,
                const std::vector<unsigned char>& source_group = {},
                const std::vector<unsigned char>& destination_group = {});
    bool Contains(const Descriptor& descriptor) const;
    std::optional<Descriptor> Get(const CService& endpoint) const;
    std::vector<unsigned char> SourceGroup(const CService& endpoint) const;
    /** Only remote records are candidates for attempts. */
    std::vector<Descriptor> GetAll(uint32_t epoch) const;
    void Expire(uint32_t epoch);
    void SetLocal(std::optional<Descriptor> descriptor);
    size_t Size() const { return m_remote.size(); }
};

struct Observation {
    CService endpoint;
    std::vector<unsigned char> network_group;
};

/** Reports must already be restricted to connected, fresh observers on the punch binding. */
std::optional<CService> FindObservedEndpoint(std::span<const Observation> observations, uint8_t format = FORMAT);

} // namespace holepunch

#endif // BITCOIN_HOLEPUNCH_H
