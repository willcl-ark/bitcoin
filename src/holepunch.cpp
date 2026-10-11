// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <holepunch.h>

#include <algorithm>
#include <array>

namespace holepunch {
bool IsEndpoint(const CService& endpoint)
{
    return endpoint.IsIPv4() || endpoint.IsIPv6();
}

namespace detail {
size_t AddressSize(uint8_t network, bool coordinator)
{
    switch (network) {
    case 1: return 4;
    case 2: return 16;
    case 4:
    case 5:
        if (coordinator) return 32;
    }
    throw std::ios_base::failure("Invalid holepunch address network");
}

uint8_t NetworkId(const CService& endpoint, bool coordinator)
{
    if (endpoint.IsIPv4()) return 1;
    if (endpoint.IsIPv6()) return 2;
    if (coordinator && endpoint.IsTor()) return 4;
    if (coordinator && endpoint.IsI2P()) return 5;
    throw std::ios_base::failure("Invalid holepunch address network");
}

CService DecodeAddress(uint8_t network, std::span<const uint8_t> bytes, uint16_t port)
{
    if (bytes.size() != AddressSize(network, true)) throw std::ios_base::failure("Invalid holepunch address length");
    static constexpr std::array<uint8_t, 12> IPV4_MAPPED{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (network == 2 && std::ranges::equal(bytes.first(12), IPV4_MAPPED)) {
        throw std::ios_base::failure("IPv4-mapped holepunch address");
    }
    // The ordinary addrv2 parser normalizes legacy embedded address prefixes.
    // Here the address remains an IPv6 endpoint until policy checks reject it.
    struct RawAddress : CNetAddr {
        RawAddress(uint8_t network, std::span<const uint8_t> bytes)
        {
            switch (network) {
            case 1: m_net = NET_IPV4; break;
            case 2: m_net = NET_IPV6; break;
            case 4: m_net = NET_ONION; break;
            case 5: m_net = NET_I2P; break;
            default: throw std::ios_base::failure("Invalid holepunch address network");
            }
            m_addr.assign(bytes.begin(), bytes.end());
        }
    };
    return CService{RawAddress{network, bytes}, port};
}

std::vector<uint8_t> AnnouncementBytes(const CService& coordinator, const CService& target)
{
    const uint8_t coord_net{NetworkId(coordinator, true)};
    const uint8_t target_net{NetworkId(target, false)};
    if (coord_net == 5 && coordinator.GetPort() != 0) throw std::ios_base::failure("Nonzero I2P coordinator port");
    DataStream encoded;
    const auto address_bytes = [](const CService& endpoint) {
        DataStream address;
        address << CNetAddr::V2(static_cast<const CNetAddr&>(endpoint));
        uint8_t network;
        address >> network;
        std::vector<uint8_t> bytes(ReadCompactSize(address));
        address >> std::span{bytes};
        return bytes;
    };
    const auto coord_bytes{address_bytes(coordinator)};
    const auto target_bytes{address_bytes(target)};
    encoded << coord_net << std::span{coord_bytes} << Using<BigEndianFormatter<2>>(coordinator.GetPort());
    encoded << target_net << std::span{target_bytes};
    const auto* begin{reinterpret_cast<const uint8_t*>(encoded.data())};
    return {begin, begin + encoded.size()};
}

void DecodeAnnouncement(std::span<const uint8_t> bytes, uint16_t port, CService& coordinator, CService& target)
{
    DataStream encoded{bytes};
    uint8_t coord_net;
    encoded >> coord_net;
    std::vector<uint8_t> coord_bytes(AddressSize(coord_net, true));
    encoded >> std::span{coord_bytes};
    uint16_t coord_port;
    encoded >> Using<BigEndianFormatter<2>>(coord_port);
    if (coord_net == 5 && coord_port != 0) throw std::ios_base::failure("Nonzero I2P coordinator port");
    uint8_t target_net;
    encoded >> target_net;
    std::vector<uint8_t> target_bytes(AddressSize(target_net, false));
    encoded >> std::span{target_bytes};
    if (!encoded.empty()) throw std::ios_base::failure("Invalid holepunch announcement length");
    coordinator = DecodeAddress(coord_net, coord_bytes, coord_port);
    target = DecodeAddress(target_net, target_bytes, port);
}
} // namespace detail
} // namespace holepunch
