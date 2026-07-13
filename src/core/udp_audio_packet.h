#pragma once

#include <cstddef>
#include <cstdint>

namespace lanspeak::core {

inline constexpr std::uint32_t kUdpAudioMagic = 0x3141504c; // "LPA1" in little-endian.
inline constexpr std::uint16_t kUdpAudioVersion = 1;
inline constexpr std::uint32_t kUdpSourceFlagTalkActive = 1u << 31u;

#pragma pack(push, 1)
struct UdpAudioPacketHeader {
    std::uint32_t magic = kUdpAudioMagic;
    std::uint16_t version = kUdpAudioVersion;
    std::uint16_t header_size = sizeof(UdpAudioPacketHeader);
    std::uint64_t sequence = 0;
    std::uint64_t send_qpc = 0;
    std::uint64_t send_qpc_frequency = 0;
    std::uint64_t capture_device_position = 0;
    std::uint64_t capture_qpc_position = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 1;
    std::uint16_t bits_per_sample = 16;
    std::uint16_t format_tag = 1; // WAVE_FORMAT_PCM without a Windows header dependency.
    std::uint16_t source_channels = 0;
    std::uint32_t source_flags = 0;
    std::uint32_t frames = 0;
    std::uint32_t payload_bytes = 0;
};
#pragma pack(pop)

static_assert(sizeof(UdpAudioPacketHeader) == 72);

bool validate_udp_audio_packet(
    const std::byte* datagram,
    std::size_t datagram_size,
    std::uint32_t expected_sample_rate,
    UdpAudioPacketHeader& header);

} // namespace lanspeak::core
