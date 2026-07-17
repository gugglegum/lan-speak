#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lanspeak::gui {

struct NetworkAdapterInfo {
    std::wstring id;
    std::wstring friendly_name;
    std::wstring ipv4_address;
};

std::vector<NetworkAdapterInfo> enumerate_active_ipv4_adapters();

std::optional<std::wstring> resolve_network_bind_address(
    const std::vector<NetworkAdapterInfo>& adapters,
    std::wstring_view adapter_id);

} // namespace lanspeak::gui
