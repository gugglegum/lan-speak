#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lanspeak::core {

std::uint32_t ipv4_broadcast_host_order(std::uint32_t address, std::uint8_t prefix_length);
std::vector<sockaddr_in> enumerate_ipv4_broadcast_targets(
    std::wstring_view bind_address,
    std::uint16_t port);
std::wstring computer_network_name();

class DiscoveryReplyCache {
public:
    bool should_reply(
        std::uint32_t source_address,
        std::uint64_t request_id,
        std::uint64_t now_ms);

private:
    struct Entry {
        std::uint32_t source_address = 0;
        std::uint64_t request_id = 0;
        std::uint64_t expires_at_ms = 0;
    };
    std::vector<Entry> entries_;
};

} // namespace lanspeak::core
