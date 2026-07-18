#include "core/udp_presence_packet.h"

#include <cstring>

namespace lanspeak::core {

UdpPresenceDatagram serialize_udp_presence_packet(const UdpPresencePacket& packet) {
    UdpPresenceDatagram datagram{};
    std::memcpy(datagram.data(), &packet, sizeof(packet));
    return datagram;
}

bool validate_udp_presence_packet(
    std::span<const std::byte> datagram,
    UdpPresencePacket& packet) {
    if (datagram.size() != sizeof(UdpPresencePacket)) {
        return false;
    }

    std::memcpy(&packet, datagram.data(), sizeof(packet));
    const bool valid_type = packet.type == UdpPresenceType::ping ||
        packet.type == UdpPresenceType::pong || packet.type == UdpPresenceType::goodbye;
    if (packet.magic != kUdpPresenceMagic ||
        packet.version != kUdpPresenceVersion ||
        packet.header_size != sizeof(UdpPresencePacket) ||
        !valid_type || packet.reserved != 0 || packet.session_id == 0) {
        return false;
    }

    if (packet.type == UdpPresenceType::goodbye) {
        return packet.nonce == 0;
    }
    return packet.nonce != 0;
}

} // namespace lanspeak::core
