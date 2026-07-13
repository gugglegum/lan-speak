#include "core/udp_audio_packet.h"

#include <cstring>
#include <limits>

namespace lanspeak::core {

bool validate_udp_audio_packet(
    const std::byte* datagram,
    std::size_t datagram_size,
    std::uint32_t expected_sample_rate,
    UdpAudioPacketHeader& header) {
    if (datagram == nullptr || datagram_size < sizeof(UdpAudioPacketHeader)) {
        return false;
    }

    std::memcpy(&header, datagram, sizeof(header));
    if (header.magic != kUdpAudioMagic ||
        header.version != kUdpAudioVersion ||
        header.header_size < sizeof(UdpAudioPacketHeader) ||
        header.header_size > datagram_size ||
        header.payload_bytes > datagram_size - header.header_size ||
        header.sample_rate < 8000 || header.sample_rate > 384000 ||
        expected_sample_rate < 8000 || expected_sample_rate > 384000 ||
        header.channels != 1 ||
        header.bits_per_sample != 16 ||
        header.format_tag != 1) {
        return false;
    }

    constexpr std::uint32_t bytes_per_frame = sizeof(std::int16_t);
    if (header.frames > std::numeric_limits<std::uint32_t>::max() / bytes_per_frame) {
        return false;
    }
    return header.payload_bytes == header.frames * bytes_per_frame;
}

} // namespace lanspeak::core
