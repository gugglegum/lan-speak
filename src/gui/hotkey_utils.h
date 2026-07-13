#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "gui/model.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace lanspeak::gui {

inline constexpr std::uint32_t kHotkeyShift = 1u << 0u;
inline constexpr std::uint32_t kHotkeyCtrl = 1u << 1u;
inline constexpr std::uint32_t kHotkeyAlt = 1u << 2u;
inline constexpr std::uint32_t kHotkeyWin = 1u << 3u;

bool is_modifier_key(UINT vk);
std::uint32_t modifier_mask_for_vk(UINT vk);
std::uint32_t current_hotkey_modifiers();
std::wstring hotkey_key_name(UINT vk);
std::wstring format_hotkey(const Hotkey& hotkey, std::wstring_view not_set_text);
bool same_hotkey(const Hotkey& left, const Hotkey& right);

} // namespace lanspeak::gui
