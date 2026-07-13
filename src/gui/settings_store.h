#pragma once

#include "gui/model.h"

#include <string>
#include <string_view>

namespace lanspeak::gui {

std::wstring encode_hotkey(const Hotkey& hotkey);
Hotkey decode_hotkey(std::wstring_view value);

std::string serialize_settings(const AppSettings& settings);
AppSettings parse_settings(std::string_view serialized);
bool save_settings_file(const std::wstring& path, const AppSettings& settings);
bool load_settings_file(const std::wstring& path, AppSettings& settings);

} // namespace lanspeak::gui
