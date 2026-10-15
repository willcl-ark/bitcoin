// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_HOLEPUNCH_H
#define BITCOIN_HOLEPUNCH_H

#include <key.h>
#include <netaddress.h>
#include <pubkey.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/** Experimental coordinatorless IPv4 TCP hole punching, version 0. */
namespace holepunch {

inline constexpr uint64_t ROUND_SECONDS{60};
inline constexpr uint64_t EPOCH_SECONDS{3600};
inline constexpr uint64_t ROUNDS_PER_EPOCH{60};
inline constexpr size_t PAYLOAD_SIZE{42};
inline constexpr size_t PROOF_SIZE{105};
inline constexpr std::string_view FEATURE_ID{"277db3e91161123f45dbd6d5300cd8642c855d6f36660390c2e713a314217fdb"};
using Bytes32 = std::array<unsigned char, 32>;

/** Globally routable IPv4 address with a nonzero TCP port. */
bool IsGlobalEndpoint(const CService& endpoint);

struct Descriptor {
    CService endpoint;
    uint32_t epoch{0};
    CPubKey pubkey;

    bool IsValid() const;
    std::array<unsigned char, PAYLOAD_SIZE> Payload() const;
    static std::optional<Descriptor> Parse(std::span<const unsigned char> payload, uint16_t port);
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
    bool initiating{false};
    Bytes32 schedule_key{};
    Bytes32 auth_key{};
    std::array<unsigned char, 152> tie_break{};
    ~Pair();
};

std::optional<Pair> DerivePair(const CKey& key, const Descriptor& local, const Descriptor& remote, const uint256& genesis_hash);

struct RoundSchedule {
    bool eligible;
    uint64_t round;
    uint64_t start_ms;
    Bytes32 priority;
    std::array<unsigned char, 152> tie_break;
};

bool IsEligible(const Bytes32& draw);
uint32_t OffsetMillis(const Bytes32& draw);
/** Returns nullopt if the round is outside the pair's epoch. */
std::optional<RoundSchedule> Schedule(const Pair& pair, uint64_t round);
std::array<unsigned char, PROOF_SIZE> MakeProof(const Pair& pair, uint64_t round, const uint256& session_id);
/** Validate the remote peer's proof against the pinned pair and BIP324 session. */
bool CheckProof(std::span<const unsigned char> proof, const Pair& pair, uint64_t round, const uint256& session_id);

/** Nonpersistent, first-admitted-per-endpoint cache. Callers supply synchronization. */
class DescriptorCache {
    const size_t m_capacity;
    std::map<CService, Descriptor> m_remote;
    std::optional<Descriptor> m_local;
    FastRandomContext m_rng;

public:
    explicit DescriptorCache(size_t capacity = 1024) : m_capacity{capacity} {}
    /** Accept a new or identical descriptor; reject conflicts, invalid and out-of-epoch records. */
    bool Insert(const Descriptor& descriptor, uint32_t epoch);
    bool Contains(const Descriptor& descriptor) const;
    std::optional<Descriptor> Get(const CService& endpoint) const;
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
std::optional<CService> FindObservedEndpoint(std::span<const Observation> observations);

} // namespace holepunch

#endif // BITCOIN_HOLEPUNCH_H
