// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <holepunch.h>

#include <hash.h>
#include <streams.h>

#include <algorithm>

namespace holepunch {
std::optional<Feature> ParseFeature(std::span<const unsigned char> bytes)
{
    if (bytes.size() != 5) return std::nullopt;
    DataStream stream{bytes};
    Feature feature;
    stream >> feature;
    return feature;
}

Network Family(const CNetAddr& address)
{
    return address.IsIPv4() ? NET_IPV4 : address.IsIPv6() ? NET_IPV6 :
                                                            NET_UNROUTABLE;
}

bool ValidEndpoint(const CService& endpoint, bool allow_unroutable)
{
    const bool valid = endpoint.IsValid() || (allow_unroutable && (endpoint.IsRFC3849() || endpoint.IsRFC9637()));
    return valid && (endpoint.IsIPv4() || endpoint.IsIPv6()) && endpoint.GetPort() != 0 &&
           (allow_unroutable || endpoint.IsRoutable());
}

WireAddress Encode(const Announcement& a)
{
    WireAddress wire{a.time, uint64_t(a.services), PUNCHKEY, {}, a.endpoint.GetPort()};
    DataStream stream;
    stream << CNetAddr::V2(static_cast<const CNetAddr&>(a.endpoint));
    uint8_t family;
    stream >> family >> wire.address;
    wire.address.insert(wire.address.begin(), family);
    wire.address.insert(wire.address.end(), a.pubkey.begin(), a.pubkey.end());
    return wire;
}

std::optional<Announcement> Decode(const WireAddress& wire, bool allow_unroutable)
{
    if (wire.network != PUNCHKEY || !(wire.services & NODE_P2P_V2) || wire.port == 0) return std::nullopt;
    if (wire.address.size() != 38 && wire.address.size() != 50) return std::nullopt;
    const auto net = wire.address[0];
    const size_t size = net == 1 ? 4 : net == 2 ? 16 :
                                                  0;
    if (size == 0 || wire.address.size() != size + 34) return std::nullopt;
    // Decode using ADDRv2 so mapped IPv6 is rejected before it can become IPv4.
    DataStream stream;
    stream << net << std::vector<unsigned char>(wire.address.begin() + 1, wire.address.begin() + 1 + size);
    CNetAddr address;
    try {
        stream >> CNetAddr::V2(address);
    } catch (const std::ios_base::failure&) {
        return std::nullopt;
    }
    if ((net == 1 && !address.IsIPv4()) || (net == 2 && !address.IsIPv6())) return std::nullopt;
    CService endpoint{address, wire.port};
    CPubKey pubkey{wire.address.begin() + 1 + size, wire.address.end()};
    if (!ValidEndpoint(endpoint, allow_unroutable) || !pubkey.IsCompressed() || !pubkey.IsFullyValid()) return std::nullopt;
    return Announcement{wire.time, ServiceFlags(wire.services), endpoint, pubkey};
}

std::optional<CAddress> DecodeAddress(const WireAddress& wire)
{
    if (wire.network == PUNCHKEY) return std::nullopt;
    DataStream stream;
    stream << wire;
    CAddress address;
    stream >> CAddress::V2_NETWORK(address);
    if (!address.IsValid()) return std::nullopt;
    return address;
}

std::vector<unsigned char> EncodeEndpoint(const CService& endpoint)
{
    DataStream stream;
    stream << CNetAddr::V2(static_cast<const CNetAddr&>(endpoint));
    const auto port = endpoint.GetPort();
    stream << Using<BigEndianFormatter<2>>(port);
    return {UCharCast(stream.data()), UCharCast(stream.data()) + stream.size()};
}

Pair PairValue(const Pair& secret, const uint256& block, const CService& own, const CService& remote)
{
    auto a = EncodeEndpoint(own), b = EncodeEndpoint(remote);
    if (b < a) std::swap(a, b);
    auto hash = TaggedHash("tcp-holepunch/pair");
    hash.write(std::as_bytes(std::span{secret}));
    hash.write(std::as_bytes(std::span{block}));
    hash.write(std::as_bytes(std::span{a}));
    hash.write(std::as_bytes(std::span{b}));
    const auto result = hash.GetSHA256();
    Pair pair;
    std::copy(result.begin(), result.end(), pair.begin());
    return pair;
}

bool IsInitiator(const Pair& pair, const CPubKey& own, const CPubKey& remote)
{
    return (own < remote) == ((pair.back() & 1) == 0);
}

bool RateLimit::Take(int64_t now, size_t limit, size_t count)
{
    while (!m_times.empty() && m_times.front() <= now - ENTRY_TTL)
        m_times.pop_front();
    if (count > limit || m_times.size() > limit - count) return false;
    for (size_t i{0}; i < count; ++i)
        m_times.push_back(now);
    return true;
}

void Reports::Set(Peer peer, Group group, const CService& endpoint, bool allow_unroutable)
{
    Remove(peer);
    if (ValidEndpoint(endpoint, allow_unroutable)) m_reports.emplace(peer, Report{std::move(group), endpoint});
}
void Reports::Remove(Peer peer) { m_reports.erase(peer); }
std::optional<CService> Reports::External(Network family) const
{
    std::map<CService, std::set<Group>> groups;
    for (const auto& [peer, report] : m_reports) {
        if (Family(report.endpoint) == family) groups[report.endpoint].insert(report.group);
    }
    std::optional<CService> result;
    for (const auto& [endpoint, sources] : groups) {
        if (sources.size() < 2) continue;
        if (result) return std::nullopt;
        result = endpoint;
    }
    return result;
}

State::State()
{
    for (auto family : {NET_IPV4, NET_IPV6})
        m_families[family].key.MakeNewKey(true);
}
void State::Rotate(Network family, int64_t now)
{
    auto& f = m_families.at(family);
    f.key.MakeNewKey(true);
    f.rotate_at = now + KEY_ROTATE_MIN + m_rng.randrange(KEY_ROTATE_MAX - KEY_ROTATE_MIN + 1);
    // Retained entries must use the current family secret, never the old key.
    for (auto& entry : m_entries) {
        if (Family(entry.announcement.endpoint) == family) {
            entry.secret = *f.key.ComputeECDHSecret(entry.announcement.pubkey);
        }
    }
}
bool State::SetExternal(Network family, std::optional<CService> endpoint, int64_t now)
{
    auto& f = m_families.at(family);
    if (endpoint == f.external) return false;
    if (endpoint && f.last_external && endpoint != f.last_external) Rotate(family, now);
    f.external = endpoint;
    if (endpoint) f.last_external = endpoint;
    if (!f.rotate_at) f.rotate_at = now + KEY_ROTATE_MIN + m_rng.randrange(KEY_ROTATE_MAX - KEY_ROTATE_MIN + 1);
    return true;
}
bool State::MaybeRotate(Network family, int64_t now)
{
    auto& f = m_families.at(family);
    if (!f.rotate_at) f.rotate_at = now + KEY_ROTATE_MIN + m_rng.randrange(KEY_ROTATE_MAX - KEY_ROTATE_MIN + 1);
    if (now < f.rotate_at) return false;
    Rotate(family, now);
    return true;
}
CPubKey State::PubKey(Network family) const { return m_families.at(family).key.GetPubKey(); }
std::optional<CService> State::External(Network family) const { return m_families.at(family).external; }

bool State::Add(Peer board, const Group& source, const Announcement& a, int64_t now)
{
    Expire(now);
    const auto family = Family(a.endpoint);
    if (!m_families.contains(family)) return false;
    for (const auto& [net, local] : m_families) {
        if (a.pubkey == local.key.GetPubKey() || (local.external && static_cast<const CNetAddr&>(a.endpoint) == static_cast<const CNetAddr&>(*local.external))) return false;
    }
    auto same = std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& e) {
        return e.board == board && e.announcement.endpoint == a.endpoint && e.announcement.pubkey == a.pubkey;
    });
    if (same != m_entries.end()) {
        same->last_seen = now;
        same->announcement.time = a.time;
        return true;
    }
    const auto secret = m_families.at(family).key.ComputeECDHSecret(a.pubkey);
    if (!secret) return false;
    std::erase_if(m_entries, [&](const auto& e) {
        return e.board == board && static_cast<const CNetAddr&>(e.announcement.endpoint) == static_cast<const CNetAddr&>(a.endpoint) && e.announcement.pubkey != a.pubkey;
    });
    std::vector<size_t> group_entries;
    for (size_t i{0}; i < m_entries.size(); ++i)
        if (m_entries[i].source == source) group_entries.push_back(i);
    if (group_entries.size() >= SOURCE_CAP) m_entries.erase(m_entries.begin() + group_entries[m_rng.randrange(group_entries.size())]);
    if (m_entries.size() >= TABLE_MAX) {
        std::map<Group, std::vector<size_t>> groups;
        for (size_t i{0}; i < m_entries.size(); ++i)
            groups[m_entries[i].source].push_back(i);
        const auto& largest = std::max_element(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.second.size() < b.second.size(); })->second;
        m_entries.erase(m_entries.begin() + largest[m_rng.randrange(largest.size())]);
    }
    m_entries.push_back({board, source, a, now, now, *secret});
    return true;
}
void State::Expire(int64_t now)
{
    std::erase_if(m_entries, [&](const auto& e) { return e.last_seen <= now - ENTRY_TTL; });
}
void State::Remove(const Entry& entry)
{
    // Picking consumes every board's copy of this announcement. Otherwise a
    // stale duplicate could start another session after the first one fails.
    std::erase_if(m_entries, [&](const auto& e) { return e.announcement.endpoint == entry.announcement.endpoint && e.announcement.pubkey == entry.announcement.pubkey; });
}
std::vector<Candidate> State::Ranked(const uint256& block, int64_t parent_time, int64_t now) const
{
    std::vector<Candidate> candidates;
    for (const auto& entry : m_entries) {
        if (entry.received_at >= parent_time || entry.last_seen <= now - ENTRY_TTL) continue;
        const auto& f = m_families.at(Family(entry.announcement.endpoint));
        if (!f.external) continue;
        const auto pair = PairValue(entry.secret, block, *f.external, entry.announcement.endpoint);
        candidates.push_back({entry, pair, IsInitiator(pair, f.key.GetPubKey(), entry.announcement.pubkey)});
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.pair < b.pair; });
    return candidates;
}
std::optional<int64_t> State::Tip(const uint256& block, const uint256& parent, int height, int64_t now, bool ibd)
{
    const auto previous = m_tips.find(parent);
    const auto parent_time = previous == m_tips.end() ? std::nullopt : std::optional{previous->second};
    m_tips.try_emplace(block, now);
    const bool round = height > m_height && !ibd && m_rounds.insert(block).second;
    m_height = height;
    // Recent tips suffice for parent maturity, without retaining the chain.
    while (m_tips.size() > 288) {
        const auto oldest = std::min_element(m_tips.begin(), m_tips.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        m_tips.erase(oldest);
    }
    return round ? parent_time : std::nullopt;
}

std::vector<Announcement> Board::Live() const
{
    std::vector<Announcement> result;
    for (const auto& [peer, visitor] : m_visitors)
        if (visitor.announcement) result.push_back(*visitor.announcement);
    return result;
}
void Board::Expire(int64_t now)
{
    std::erase_if(m_grace, [&](const auto& grace) { return grace.until <= now; });
}
bool Board::Admit(Peer peer, const CService& endpoint, const Group& group, int64_t now)
{
    Expire(now);
    if (!ValidEndpoint(endpoint, m_allow_unroutable) || m_visitors.size() >= BOARD_VISITORS_MAX || m_visitors.contains(peer)) return false;
    size_t in_group{0};
    for (const auto& [id, visitor] : m_visitors) {
        if (static_cast<const CNetAddr&>(visitor.endpoint) == static_cast<const CNetAddr&>(endpoint)) return false;
        in_group += visitor.group == group;
    }
    if (in_group >= BOARD_PER_GROUP) return false;
    m_visitors.emplace(peer, Visitor{endpoint, group, std::nullopt, now, now});
    return true;
}
void Board::Remove(Peer peer, int64_t now)
{
    auto it = m_visitors.find(peer);
    if (it == m_visitors.end()) return;
    if (it->second.announcement) m_grace.push_back({*it->second.announcement, now + BOARD_GRACE});
    m_visitors.erase(it);
    for (auto& [id, visitor] : m_visitors)
        std::erase(visitor.pending, peer);
}
bool Board::Announce(Peer peer, const WireAddress& wire, int64_t now)
{
    auto it = m_visitors.find(peer);
    if (it == m_visitors.end() || !it->second.received.Take(now)) return false;
    auto announcement = Decode(wire, m_allow_unroutable);
    if (!announcement || announcement->endpoint != it->second.endpoint) return false;
    announcement->time = now;
    it->second.announcement = *announcement;
    it->second.ack = true;
    std::erase_if(m_grace, [&](const auto& grace) { return static_cast<const CNetAddr&>(grace.announcement.endpoint) == static_cast<const CNetAddr&>(announcement->endpoint); });
    for (auto& [other, visitor] : m_visitors) {
        if (other != peer && std::find(visitor.pending.begin(), visitor.pending.end(), peer) == visitor.pending.end()) visitor.pending.push_back(peer);
    }
    return true;
}
std::vector<Announcement> Board::Drain(Peer peer, int64_t now)
{
    auto it = m_visitors.find(peer);
    if (it == m_visitors.end()) return {};
    auto& visitor = it->second;
    if (!visitor.announcement) return {};
    auto live = Live();
    if ((visitor.initial && visitor.ack) || (!visitor.initial && now - visitor.refreshed_at >= BOARD_REFRESH)) {
        if (visitor.initial || visitor.refresh_budget.Take(now, 64, live.size())) {
            visitor.initial = false;
            visitor.ack = false;
            visitor.refreshed_at = now;
            visitor.pending.clear();
            return live;
        }
    }
    std::vector<Announcement> result;
    if (visitor.ack && visitor.announcement && visitor.ack_budget.Take(now, 16)) {
        result.push_back(*visitor.announcement);
        visitor.ack = false;
    }
    while (!visitor.pending.empty() && visitor.push_budget.Take(now, 128)) {
        const auto source = visitor.pending.front();
        visitor.pending.pop_front();
        const auto from = m_visitors.find(source);
        if (from != m_visitors.end() && from->second.announcement) result.push_back(*from->second.announcement);
    }
    return result;
}
} // namespace holepunch
