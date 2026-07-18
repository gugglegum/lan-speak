#include "core/udp_discovery_packet.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

namespace lanspeak::core {
namespace {

bool valid_utf8(std::string_view value) {
    return !value.empty() && MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0) > 0;
}

} // namespace

std::vector<std::byte> serialize_udp_discovery_packet(const UdpDiscoveryPacket& packet) {
    UdpDiscoveryHeader header = packet.header;
    const bool response = header.type == UdpDiscoveryType::response;
    const std::size_t name_size = response ? packet.computer_name_utf8.size() : 0;
    if (name_size > kMaximumDiscoveryNameBytes) return {};
    header.name_bytes = static_cast<std::uint16_t>(name_size);
    header.packet_size = static_cast<std::uint32_t>(sizeof(header) + name_size);

    std::vector<std::byte> datagram(header.packet_size);
    std::memcpy(datagram.data(), &header, sizeof(header));
    if (name_size != 0) {
        std::memcpy(datagram.data() + sizeof(header), packet.computer_name_utf8.data(), name_size);
    }
    return datagram;
}

bool validate_udp_discovery_packet(
    std::span<const std::byte> datagram,
    UdpDiscoveryPacket& packet) {
    if (datagram.size() < sizeof(UdpDiscoveryHeader)) return false;
    UdpDiscoveryHeader header{};
    std::memcpy(&header, datagram.data(), sizeof(header));
    const bool valid_type = header.type == UdpDiscoveryType::query ||
        header.type == UdpDiscoveryType::response;
    if (header.magic != kUdpDiscoveryMagic || header.version != kUdpDiscoveryVersion ||
        header.header_size != sizeof(UdpDiscoveryHeader) || !valid_type ||
        header.packet_size != datagram.size() || header.request_id == 0 ||
        header.session_id == 0 || header.voice_port == 0 || header.flags != 0 ||
        header.reserved != 0 || header.name_bytes > kMaximumDiscoveryNameBytes ||
        header.packet_size != sizeof(header) + header.name_bytes) {
        return false;
    }

    const std::string name(
        reinterpret_cast<const char*>(datagram.data() + sizeof(header)),
        header.name_bytes);
    if (header.type == UdpDiscoveryType::query) {
        if (header.name_bytes != 0) return false;
    } else if (!valid_utf8(name)) {
        return false;
    }

    packet.header = header;
    packet.computer_name_utf8 = name;
    return true;
}

} // namespace lanspeak::core
