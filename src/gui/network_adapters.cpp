#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include "gui/network_adapters.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace lanspeak::gui {
namespace {

std::wstring widen_adapter_id(const char* value) {
    if (value == nullptr) {
        return {};
    }
    std::wstring result;
    while (*value != '\0') {
        result.push_back(static_cast<unsigned char>(*value));
        ++value;
    }
    return result;
}

std::wstring first_ipv4_address(const IP_ADAPTER_ADDRESSES& adapter) {
    for (const IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter.FirstUnicastAddress;
         unicast != nullptr;
         unicast = unicast->Next) {
        if (unicast->Address.lpSockaddr == nullptr ||
            unicast->Address.lpSockaddr->sa_family != AF_INET) {
            continue;
        }
        const auto* address = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
        if (address->sin_addr.s_addr == htonl(INADDR_LOOPBACK) ||
            address->sin_addr.s_addr == htonl(INADDR_ANY)) {
            continue;
        }
        wchar_t buffer[INET_ADDRSTRLEN]{};
        if (InetNtopW(AF_INET, &address->sin_addr, buffer, static_cast<DWORD>(std::size(buffer))) != nullptr) {
            return buffer;
        }
    }
    return {};
}

} // namespace

std::vector<NetworkAdapterInfo> enumerate_active_ipv4_adapters() {
    constexpr ULONG flags =
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW ||
        size == 0) {
        return {};
    }

    std::vector<std::byte> storage(size);
    auto* first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, first, &size) != NO_ERROR) {
        return {};
    }

    std::vector<NetworkAdapterInfo> result;
    for (const IP_ADAPTER_ADDRESSES* adapter = first;
         adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        std::wstring address = first_ipv4_address(*adapter);
        std::wstring id = widen_adapter_id(adapter->AdapterName);
        if (address.empty() || id.empty()) {
            continue;
        }
        std::wstring friendly_name = adapter->FriendlyName != nullptr
            ? adapter->FriendlyName
            : id;
        result.push_back({std::move(id), std::move(friendly_name), std::move(address)});
    }

    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        const int name_order = _wcsicmp(left.friendly_name.c_str(), right.friendly_name.c_str());
        if (name_order != 0) {
            return name_order < 0;
        }
        return left.id < right.id;
    });
    return result;
}

std::optional<std::wstring> resolve_network_bind_address(
    const std::vector<NetworkAdapterInfo>& adapters,
    std::wstring_view adapter_id) {
    if (adapter_id.empty()) {
        return std::wstring(L"0.0.0.0");
    }
    const auto found = std::find_if(adapters.begin(), adapters.end(), [&](const auto& adapter) {
        return adapter.id == adapter_id;
    });
    if (found == adapters.end()) {
        return std::nullopt;
    }
    return found->ipv4_address;
}

} // namespace lanspeak::gui
