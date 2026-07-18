#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lanspeak::core {

inline constexpr std::uint16_t kDiscoveryPort = 49741;
inline constexpr std::uint32_t kUdpDiscoveryMagic = 0x3144534c; // "LSD1" in little-endian.
inline constexpr std::uint16_t kUdpDiscoveryVersion = 1;
inline constexpr std::size_t kMaximumDiscoveryNameBytes = 256;

enum class UdpDiscoveryType : std::uint16_t {
    query = 1,
    response = 2
};

#pragma pack(push, 1)
struct UdpDiscoveryHeader {
    std::uint32_t magic = kUdpDiscoveryMagic;
    std::uint16_t version = kUdpDiscoveryVersion;
    UdpDiscoveryType type = UdpDiscoveryType::query;
    std::uint16_t header_size = sizeof(UdpDiscoveryHeader);
    std::uint16_t name_bytes = 0;
    std::uint32_t packet_size = sizeof(UdpDiscoveryHeader);
    std::uint64_t request_id = 0;
    std::uint64_t session_id = 0;
    std::uint16_t voice_port = 0;
    std::uint16_t flags = 0;
    std::uint32_t reserved = 0;
};
#pragma pack(pop)

static_assert(sizeof(UdpDiscoveryHeader) == 40);

struct UdpDiscoveryPacket {
    UdpDiscoveryHeader header;
    std::string computer_name_utf8;
};

std::vector<std::byte> serialize_udp_discovery_packet(const UdpDiscoveryPacket& packet);
bool validate_udp_discovery_packet(
    std::span<const std::byte> datagram,
    UdpDiscoveryPacket& packet);

} // namespace lanspeak::core
