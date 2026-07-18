#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include "core/network_discovery.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace lanspeak::core {
namespace {

constexpr std::uint64_t kReplyCacheTtlMs = 10'000;
constexpr std::size_t kMaximumReplyCacheEntries = 64;

bool parse_ipv4(std::wstring_view text, IN_ADDR& address) {
    const std::wstring copy(text);
    return InetPtonW(AF_INET, copy.c_str(), &address) == 1;
}

} // namespace

std::uint32_t ipv4_broadcast_host_order(
    std::uint32_t address,
    std::uint8_t prefix_length) {
    if (prefix_length >= 32) return address;
    const std::uint32_t mask = prefix_length == 0
        ? 0u
        : 0xffffffffu << (32u - prefix_length);
    return (address & mask) | ~mask;
}

std::vector<sockaddr_in> enumerate_ipv4_broadcast_targets(
    std::wstring_view bind_address,
    std::uint16_t port) {
    IN_ADDR requested{};
    const bool all_interfaces = bind_address.empty() || bind_address == L"0.0.0.0";
    if (!all_interfaces && !parse_ipv4(bind_address, requested)) return {};

    constexpr ULONG flags =
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW ||
        size == 0) {
        return {};
    }
    std::vector<std::byte> storage(size);
    auto* first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, first, &size) != NO_ERROR) return {};

    std::vector<sockaddr_in> targets;
    for (const IP_ADAPTER_ADDRESSES* adapter = first; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (const IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
             unicast != nullptr;
             unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr ||
                unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto* local = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            if (local->sin_addr.s_addr == htonl(INADDR_LOOPBACK) ||
                (!all_interfaces && local->sin_addr.s_addr != requested.s_addr)) {
                continue;
            }
            const std::uint32_t host_address = ntohl(local->sin_addr.s_addr);
            const std::uint32_t broadcast = ipv4_broadcast_host_order(
                host_address,
                unicast->OnLinkPrefixLength);
            sockaddr_in target{};
            target.sin_family = AF_INET;
            target.sin_port = htons(port);
            target.sin_addr.s_addr = htonl(broadcast);
            if (std::none_of(targets.begin(), targets.end(), [&](const sockaddr_in& existing) {
                    return existing.sin_addr.s_addr == target.sin_addr.s_addr;
                })) {
                targets.push_back(target);
            }
        }
    }
    return targets;
}

std::wstring computer_network_name() {
    std::array<wchar_t, 256> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    if (GetComputerNameExW(ComputerNameDnsHostname, buffer.data(), &size) && size != 0) {
        return std::wstring(buffer.data(), size);
    }
    size = static_cast<DWORD>(buffer.size());
    if (GetComputerNameW(buffer.data(), &size) && size != 0) {
        return std::wstring(buffer.data(), size);
    }
    return L"LanSpeak";
}

bool DiscoveryReplyCache::should_reply(
    std::uint32_t source_address,
    std::uint64_t request_id,
    std::uint64_t now_ms) {
    std::erase_if(entries_, [&](const Entry& entry) {
        return entry.expires_at_ms <= now_ms;
    });
    const auto duplicate = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
        return entry.source_address == source_address && entry.request_id == request_id;
    });
    if (duplicate != entries_.end()) return false;
    if (entries_.size() >= kMaximumReplyCacheEntries) entries_.erase(entries_.begin());
    entries_.push_back(Entry{source_address, request_id, now_ms + kReplyCacheTtlMs});
    return true;
}

} // namespace lanspeak::core
