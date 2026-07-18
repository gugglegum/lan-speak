#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lanspeak::core {

inline constexpr std::uint32_t kUdpPeerInfoMagic = 0x3149534c; // "LSI1" in little-endian.
inline constexpr std::uint16_t kUdpPeerInfoVersion = 1;

enum class UdpPeerInfoType : std::uint16_t {
    info = 1,
    request = 2,
    ack = 3
};

enum UdpPeerInfoFlags : std::uint16_t {
    kPeerInfoCaptureToSendValid = 1u << 0u,
    kPeerInfoRenderLatencyValid = 1u << 1u,
    kPeerInfoPacketDurationValid = 1u << 2u,
    kPeerInfoReceiveBufferValid = 1u << 3u
};

inline constexpr std::uint16_t kUdpPeerInfoKnownFlags =
    kPeerInfoCaptureToSendValid |
    kPeerInfoRenderLatencyValid |
    kPeerInfoPacketDurationValid |
    kPeerInfoReceiveBufferValid;

#pragma pack(push, 1)
struct UdpPeerInfoPacket {
    std::uint32_t magic = kUdpPeerInfoMagic;
    std::uint16_t version = kUdpPeerInfoVersion;
    std::uint16_t packet_size = sizeof(UdpPeerInfoPacket);
    UdpPeerInfoType type = UdpPeerInfoType::info;
    std::uint16_t flags = 0;
    std::uint64_t session_id = 0;
    std::uint32_t revision = 0;
    std::uint32_t reserved = 0;
    std::uint32_t capture_to_send_us = 0;
    std::uint32_t render_latency_us = 0;
    std::uint32_t packet_duration_us = 0;
    std::uint32_t receive_buffer_us = 0;
};
#pragma pack(pop)

static_assert(sizeof(UdpPeerInfoPacket) == 44);

using UdpPeerInfoDatagram = std::array<std::byte, sizeof(UdpPeerInfoPacket)>;

UdpPeerInfoDatagram serialize_udp_peer_info_packet(const UdpPeerInfoPacket& packet);
bool validate_udp_peer_info_packet(
    std::span<const std::byte> datagram,
    UdpPeerInfoPacket& packet);

} // namespace lanspeak::core
