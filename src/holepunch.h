// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_HOLEPUNCH_H
#define BITCOIN_HOLEPUNCH_H

#include <key.h>
#include <netaddress.h>
#include <protocol.h>
#include <pubkey.h>
#include <random.h>
#include <serialize.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <ios>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace holepunch {
inline constexpr char FEATURE_ID[]{"https://delvingbitcoin.org/t/2497/22"};
inline constexpr uint32_t BOARD{1}, VISITOR{2};
inline constexpr uint8_t PUNCHKEY{9};
inline constexpr size_t PUNCHKEY_PER_HOUR{240}, SOURCE_CAP{64}, TABLE_MAX{1024};
inline constexpr size_t BOARD_VISITORS_MAX{32}, BOARD_PER_GROUP{4};
inline constexpr int64_t ENTRY_TTL{3600}, BOARD_GRACE{600}, BOARD_REFRESH{1800};
inline constexpr int64_t KEY_ROTATE_MIN{18 * 3600}, KEY_ROTATE_MAX{24 * 3600};
using Group = std::vector<unsigned char>;
using Pair = std::array<unsigned char, 32>;
using Peer = int64_t;

struct Feature {
    uint8_t version{0};
    uint32_t flags{0};
    SERIALIZE_METHODS(Feature, obj) { READWRITE(obj.version, obj.flags); }
};
std::optional<Feature> ParseFeature(std::span<const unsigned char> bytes);

struct Announcement {
    uint32_t time{0};
    ServiceFlags services{NODE_NONE};
    CService endpoint;
    CPubKey pubkey;
};
/** Raw ADDRv2 element. Validate/count PUNCHKEY before any curve operation. */
struct WireAddress {
    uint32_t time{0};
    uint64_t services{0};
    uint8_t network{0};
    std::vector<unsigned char> address;
    uint16_t port{0};
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << time << COMPACTSIZE(services) << network << address;
        s << Using<BigEndianFormatter<2>>(port);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> time >> COMPACTSIZE(services) >> network;
        const auto size = ReadCompactSize(s);
        if (size > 512) throw std::ios_base::failure("Address too long");
        address.resize(size);
        s >> std::span{address};
        s >> Using<BigEndianFormatter<2>>(port);
    }
};
WireAddress Encode(const Announcement& announcement);
std::optional<Announcement> Decode(const WireAddress& wire, bool allow_unroutable = false);
std::optional<CAddress> DecodeAddress(const WireAddress& wire);
/** Unlike GetNetwork(), preserves the IP family for regtest/local addresses. */
Network Family(const CNetAddr& address);
bool ValidEndpoint(const CService& endpoint, bool allow_unroutable = false);
std::vector<unsigned char> EncodeEndpoint(const CService& endpoint);
Pair PairValue(const Pair& secret, const uint256& block, const CService& own, const CService& remote);
bool IsInitiator(const Pair& pair, const CPubKey& own, const CPubKey& remote);

/** Rolling one-hour element budget. Call before decoding/validating an element. */
class RateLimit
{
    std::deque<int64_t> m_times;

public:
    bool Take(int64_t now, size_t limit = PUNCHKEY_PER_HOUR, size_t count = 1);
};
class Reports
{
    struct Report {
        Group group;
        CService endpoint;
    };
    std::map<Peer, Report> m_reports;

public:
    void Set(Peer peer, Group group, const CService& endpoint, bool allow_unroutable = false);
    void Remove(Peer peer);
    std::optional<CService> External(Network family) const;
};
struct Entry {
    Peer board;
    Group source;
    Announcement announcement;
    int64_t received_at, last_seen;
    Pair secret;
};
struct Candidate {
    Entry entry;
    Pair pair;
    bool initiator;
};
/** Caller serializes access and applies socket, capacity and network-group policy. */
class State
{
    struct FamilyState {
        CKey key;
        std::optional<CService> external;
        std::optional<CService> last_external;
        int64_t rotate_at{0};
    };
    std::map<Network, FamilyState> m_families;
    std::vector<Entry> m_entries;
    FastRandomContext m_rng;
    std::map<uint256, int64_t> m_tips;
    std::set<uint256> m_rounds;
    int m_height{-1};
    void Rotate(Network family, int64_t now);

public:
    State();
    bool SetExternal(Network family, std::optional<CService> endpoint, int64_t now);
    bool MaybeRotate(Network family, int64_t now);
    CPubKey PubKey(Network family) const;
    std::optional<CService> External(Network family) const;
    bool Add(Peer board, const Group& source, const Announcement& announcement, int64_t now);
    void Expire(int64_t now);
    void Remove(const Entry& entry);
    const std::vector<Entry>& Entries() const { return m_entries; }
    std::vector<Candidate> Ranked(const uint256& block, int64_t parent_time, int64_t now) const;
    /** Records every tip, including IBD. Returns parent arrival only for a new round. */
    std::optional<int64_t> Tip(const uint256& block, const uint256& parent, int height, int64_t now, bool ibd);
};

class Board
{
    struct Visitor {
        CService endpoint;
        Group group;
        std::optional<Announcement> announcement;
        int64_t opened_at, refreshed_at;
        bool initial{true}, ack{false};
        std::deque<Peer> pending{};
        RateLimit received{}, refresh_budget{}, ack_budget{}, push_budget{};
    };
    struct Grace {
        Announcement announcement;
        int64_t until;
    };
    std::map<Peer, Visitor> m_visitors;
    std::vector<Grace> m_grace;
    bool m_allow_unroutable;
    std::vector<Announcement> Live() const;

public:
    explicit Board(bool allow_unroutable = false) : m_allow_unroutable{allow_unroutable} {}
    bool Admit(Peer peer, const CService& endpoint, const Group& group, int64_t now);
    void Remove(Peer peer, int64_t now);
    void Expire(int64_t now);
    /** Includes invalid elements in the receive budget, before curve validation. */
    bool Announce(Peer peer, const WireAddress& wire, int64_t now);
    std::vector<Announcement> Drain(Peer peer, int64_t now);
    size_t Size() const { return m_visitors.size(); }
};
} // namespace holepunch
#endif // BITCOIN_HOLEPUNCH_H
