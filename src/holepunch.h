// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_HOLEPUNCH_H
#define BITCOIN_HOLEPUNCH_H

#include <netaddress.h>
#include <protocol.h>
#include <serialize.h>
#include <streams.h>
#include <util/time.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <span>
#include <variant>
#include <vector>

// TCP hole punching draft, pinned for this implementation:
// https://github.com/willcl-ark/bips/blob/0314af539fee2af61da5ccade300cc890aa47d47/bip-tcp-holepunch.md
namespace holepunch {
enum class Role : uint8_t { Responder = 0,
                            Initiator = 1 };
inline constexpr uint32_t COORDINATOR{1}, ADVERTISED{2}, POOL_OK{4}, HANDOFF_OK{8}, PUNCHPORT{16};
inline constexpr auto WINDOW{std::chrono::seconds{15}};
inline constexpr auto RETRY{std::chrono::seconds{1}};
inline constexpr auto GRACE{std::chrono::seconds{2}};
inline constexpr auto COORD_TIMEOUT{std::chrono::seconds{10}};
inline constexpr auto PER_IP_INTERVAL{std::chrono::minutes{10}};
inline constexpr auto ATTEMPT_INTERVAL{std::chrono::minutes{5}};
inline constexpr auto CACHE_TTL{std::chrono::minutes{10}};
inline constexpr auto ANNOUNCE_INTERVAL{std::chrono::minutes{10}};
inline constexpr int MAX_ACTIVE_SESSIONS{2}, SESSIONS_PER_MIN{4}, CACHE_MAX{32}, CACHE_PER_PEER{4}, COORD_INBOUND_MAX{8};

bool IsEndpoint(const CService& endpoint);

namespace detail {
size_t AddressSize(uint8_t network, bool coordinator);
uint8_t NetworkId(const CService& endpoint, bool coordinator);
CService DecodeAddress(uint8_t network, std::span<const uint8_t> bytes, uint16_t port);
std::vector<uint8_t> AnnouncementBytes(const CService& coordinator, const CService& target);
void DecodeAnnouncement(std::span<const uint8_t> bytes, uint16_t port, CService& coordinator, CService& target);

template <typename Stream>
void WriteEndpoint(Stream& s, const CService& endpoint)
{
    NetworkId(endpoint, false);
    s << CNetAddr::V2(endpoint);
}

template <typename Stream>
void ReadEndpoint(Stream& s, CService& endpoint)
{
    uint8_t network;
    s >> network;
    const auto expected{AddressSize(network, false)};
    if (ReadCompactSize(s) != expected) throw std::ios_base::failure("Invalid holepunch endpoint length");
    std::vector<uint8_t> bytes(expected);
    s >> std::span{bytes};
    uint16_t port;
    s >> Using<BigEndianFormatter<2>>(port);
    endpoint = DecodeAddress(network, bytes, port);
}
} // namespace detail

struct Rendezvous {
    CService target;
    CService requester;
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        if (target.IsIPv4() != requester.IsIPv4()) throw std::ios_base::failure("Holepunch endpoint family mismatch");
        detail::WriteEndpoint(s, target);
        detail::WriteEndpoint(s, requester);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        detail::ReadEndpoint(s, target);
        detail::ReadEndpoint(s, requester);
        if (target.IsIPv4() != requester.IsIPv4()) throw std::ios_base::failure("Holepunch endpoint family mismatch");
    }
};

struct Punch {
    Role role{Role::Responder};
    CService peer;
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        if (role != Role::Responder && role != Role::Initiator) throw std::ios_base::failure("Invalid holepunch role");
        s << static_cast<uint8_t>(role);
        detail::WriteEndpoint(s, peer);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t raw_role;
        s >> raw_role;
        if (raw_role > 1) throw std::ios_base::failure("Invalid holepunch role");
        role = static_cast<Role>(raw_role);
        detail::ReadEndpoint(s, peer);
    }
};

struct Announcement {
    NodeSeconds time{};
    ServiceFlags services{NODE_NONE};
    CService coordinator;
    CService target;
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << Using<LossyChronoFormatter<uint32_t>>(time);
        WriteCompactSize(s, static_cast<uint64_t>(services));
        s << uint8_t{8};
        const auto bytes{detail::AnnouncementBytes(coordinator, target)};
        WriteCompactSize(s, bytes.size());
        s << std::span{bytes};
        s << Using<BigEndianFormatter<2>>(target.GetPort());
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> Using<LossyChronoFormatter<uint32_t>>(time);
        services = static_cast<ServiceFlags>(ReadCompactSize(s, false));
        uint8_t network;
        s >> network;
        if (network != 8) throw std::ios_base::failure("Invalid holepunch announcement network");
        const auto size{ReadCompactSize(s)};
        if (size > 52) throw std::ios_base::failure("Invalid holepunch announcement length");
        std::vector<uint8_t> bytes(size);
        s >> std::span{bytes};
        uint16_t port;
        s >> Using<BigEndianFormatter<2>>(port);
        detail::DecodeAnnouncement(bytes, port, coordinator, target);
    }
};

/** An addrv2 element, keeping holepunch announcements out of CAddress. */
struct AddrV2 {
    std::variant<CAddress, Announcement> value;
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        if (const auto* address = std::get_if<CAddress>(&value)) {
            s << CAddress::V2_NETWORK(*address);
        } else {
            s << std::get<Announcement>(value);
        }
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint32_t time;
        s >> time;
        const auto services{ReadCompactSize(s, false)};
        uint8_t network;
        s >> network;
        const auto size{ReadCompactSize(s)};
        if (size > 512) throw std::ios_base::failure("Address too long");
        std::vector<uint8_t> bytes(size);
        s >> std::span{bytes};
        uint16_t port;
        s >> Using<BigEndianFormatter<2>>(port);
        if (network == 8) {
            Announcement announcement{NodeSeconds{std::chrono::seconds{time}}, static_cast<ServiceFlags>(services), {}, {}};
            detail::DecodeAnnouncement(bytes, port, announcement.coordinator, announcement.target);
            value = announcement;
        } else {
            // Reuse the ordinary parser, including its handling of unknown networks.
            DataStream encoded;
            encoded << time;
            WriteCompactSize(encoded, services);
            encoded << network;
            WriteCompactSize(encoded, size);
            encoded << std::span{bytes} << Using<BigEndianFormatter<2>>(port);
            CAddress address;
            encoded >> CAddress::V2_NETWORK(address);
            value = address;
        }
    }
};
} // namespace holepunch

#endif // BITCOIN_HOLEPUNCH_H
