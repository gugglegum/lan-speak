#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "gui/model.h"
#include "gui/network_adapters.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lanspeak::gui {

struct NetworkSettingsValue {
    std::uint16_t local_port = 49740;
    std::wstring adapter_id;
};

bool show_network_settings_dialog(
    HINSTANCE instance,
    HWND owner,
    HICON large_icon,
    HICON small_icon,
    LanguageSetting language,
    const std::vector<NetworkAdapterInfo>& adapters,
    const NetworkSettingsValue& current,
    NetworkSettingsValue& result);

} // namespace lanspeak::gui
