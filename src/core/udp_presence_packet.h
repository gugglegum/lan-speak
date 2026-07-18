#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lanspeak::core {

inline constexpr std::uint32_t kUdpPresenceMagic = 0x3150534c; // "LSP1" in little-endian.
inline constexpr std::uint16_t kUdpPresenceVersion = 1;

enum class UdpPresenceType : std::uint16_t {
    ping = 1,
    pong = 2,
    goodbye = 3
};

#pragma pack(push, 1)
struct UdpPresencePacket {
    std::uint32_t magic = kUdpPresenceMagic;
    std::uint16_t version = kUdpPresenceVersion;
    std::uint16_t header_size = sizeof(UdpPresencePacket);
    UdpPresenceType type = UdpPresenceType::ping;
    std::uint16_t reserved = 0;
    std::uint64_t session_id = 0;
    std::uint64_t nonce = 0;
};
#pragma pack(pop)

static_assert(sizeof(UdpPresencePacket) == 28);

using UdpPresenceDatagram = std::array<std::byte, sizeof(UdpPresencePacket)>;

UdpPresenceDatagram serialize_udp_presence_packet(const UdpPresencePacket& packet);
bool validate_udp_presence_packet(std::span<const std::byte> datagram, UdpPresencePacket& packet);

} // namespace lanspeak::core
