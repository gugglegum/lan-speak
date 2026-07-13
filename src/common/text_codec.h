#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace lanspeak::common {

std::string wide_to_utf8(std::wstring_view text);
std::wstring utf8_to_wide(std::string_view text);
std::wstring escape_field(std::wstring_view value);
std::wstring unescape_field(std::wstring_view value);
std::vector<std::wstring> split_fields(std::wstring_view line, wchar_t separator);

} // namespace lanspeak::common
