#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "common/text_codec.h"

#include <windows.h>

namespace lanspeak::common {

std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
            result.data(), required, nullptr, nullptr) <= 0) {
        return {};
    }
    return result;
}

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
            result.data(), required) <= 0) {
        return {};
    }
    return result;
}

std::wstring escape_field(std::wstring_view value) {
    std::wstring escaped;
    escaped.reserve(value.size());
    for (const wchar_t ch : value) {
        switch (ch) {
        case L'\\': escaped += L"\\\\"; break;
        case L'\t': escaped += L"\\t"; break;
        case L'\r': escaped += L"\\r"; break;
        case L'\n': escaped += L"\\n"; break;
        default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

std::wstring unescape_field(std::wstring_view value) {
    std::wstring unescaped;
    unescaped.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const wchar_t ch = value[index];
        if (ch != L'\\' || index + 1 >= value.size()) {
            unescaped.push_back(ch);
            continue;
        }
        const wchar_t next = value[++index];
        switch (next) {
        case L't': unescaped.push_back(L'\t'); break;
        case L'r': unescaped.push_back(L'\r'); break;
        case L'n': unescaped.push_back(L'\n'); break;
        case L'\\': unescaped.push_back(L'\\'); break;
        default: unescaped.push_back(next); break;
        }
    }
    return unescaped;
}

std::vector<std::wstring> split_fields(std::wstring_view line, wchar_t separator) {
    std::vector<std::wstring> fields;
    std::size_t start = 0;
    for (;;) {
        const std::size_t position = line.find(separator, start);
        if (position == std::wstring_view::npos) {
            fields.emplace_back(line.substr(start));
            return fields;
        }
        fields.emplace_back(line.substr(start, position - start));
        start = position + 1;
    }
}

} // namespace lanspeak::common
