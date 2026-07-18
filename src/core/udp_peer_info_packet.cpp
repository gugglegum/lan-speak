#include "core/udp_peer_info_packet.h"

#include <cstring>

namespace lanspeak::core {
namespace {

constexpr std::uint32_t kMaximumLatencyComponentUs = 5'000'000;

bool valid_component(std::uint16_t flags, std::uint16_t flag, std::uint32_t value) {
    const bool present = (flags & flag) != 0;
    return present ? value > 0 && value <= kMaximumLatencyComponentUs : value == 0;
}

} // namespace

UdpPeerInfoDatagram serialize_udp_peer_info_packet(const UdpPeerInfoPacket& packet) {
    UdpPeerInfoDatagram datagram{};
    std::memcpy(datagram.data(), &packet, sizeof(packet));
    return datagram;
}

bool validate_udp_peer_info_packet(
    std::span<const std::byte> datagram,
    UdpPeerInfoPacket& packet) {
    if (datagram.size() != sizeof(UdpPeerInfoPacket)) return false;
    std::memcpy(&packet, datagram.data(), sizeof(packet));

    const bool valid_type = packet.type == UdpPeerInfoType::info ||
        packet.type == UdpPeerInfoType::request ||
        packet.type == UdpPeerInfoType::ack;
    if (packet.magic != kUdpPeerInfoMagic ||
        packet.version != kUdpPeerInfoVersion ||
        packet.packet_size != sizeof(UdpPeerInfoPacket) ||
        !valid_type || packet.session_id == 0 || packet.revision == 0 ||
        packet.reserved != 0 || (packet.flags & ~kUdpPeerInfoKnownFlags) != 0) {
        return false;
    }

    if (packet.type != UdpPeerInfoType::info) {
        return packet.flags == 0 && packet.capture_to_send_us == 0 &&
            packet.render_latency_us == 0 && packet.packet_duration_us == 0 &&
            packet.receive_buffer_us == 0;
    }

    return valid_component(packet.flags, kPeerInfoCaptureToSendValid, packet.capture_to_send_us) &&
        valid_component(packet.flags, kPeerInfoRenderLatencyValid, packet.render_latency_us) &&
        valid_component(packet.flags, kPeerInfoPacketDurationValid, packet.packet_duration_us) &&
        valid_component(packet.flags, kPeerInfoReceiveBufferValid, packet.receive_buffer_us);
}

} // namespace lanspeak::core
