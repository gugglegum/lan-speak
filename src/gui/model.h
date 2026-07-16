#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lanspeak::gui {

struct Hotkey {
    std::uint32_t modifiers = 0;
    UINT vk = 0;

    [[nodiscard]] bool valid() const {
        return vk != 0;
    }

    auto operator<=>(const Hotkey&) const = default;
};

enum class LanguageSetting {
    automatic,
    russian,
    english
};

struct Contact {
    std::wstring name;
    std::wstring host = L"127.0.0.1";
    std::uint16_t port = 49740;
    double gain = 1.0;
    bool muted = false;
    bool global_ptt_enabled = true;
    bool self_duck = true;
    double duck_db = 12.0;
    double duck_threshold = 0.02;
    int duck_attack_ms = 8;
    int duck_hold_ms = 80;
    int duck_release_ms = 120;
    Hotkey ptt_hotkey;
};

struct AppSettings {
    int window_width = 600;
    int window_height = 650;
    LanguageSetting language = LanguageSetting::automatic;
    Hotkey ptt_all_hotkey;
    bool debug_console_visible = false;
    std::uint16_t local_port = 49740;
    std::wstring capture_device_selector;
    std::wstring render_device_selector;
    std::vector<Contact> contacts;
};

} // namespace lanspeak::gui
