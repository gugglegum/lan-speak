#include "gui/settings_store.h"

#include "common/text_codec.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace lanspeak::gui {
namespace {

std::wstring language_value(LanguageSetting setting) {
    switch (setting) {
    case LanguageSetting::russian: return L"ru";
    case LanguageSetting::english: return L"en";
    case LanguageSetting::automatic: return L"auto";
    }
    return L"auto";
}

LanguageSetting parse_language(std::wstring_view value) {
    if (value == L"ru" || value == L"russian") return LanguageSetting::russian;
    if (value == L"en" || value == L"english") return LanguageSetting::english;
    return LanguageSetting::automatic;
}

bool parse_bool(std::wstring_view value, bool fallback = false) {
    if (value == L"1" || value == L"true" || value == L"yes") return true;
    if (value == L"0" || value == L"false" || value == L"no") return false;
    return fallback;
}

long parse_long(std::wstring_view value, long fallback, long minimum, long maximum) {
    const std::wstring owned(value);
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(owned.c_str(), &end, 10);
    if (end == owned.c_str() || *end != L'\0') return fallback;
    return std::clamp(parsed, minimum, maximum);
}

double parse_double(std::wstring_view value, double fallback, double minimum, double maximum) {
    const std::wstring owned(value);
    wchar_t* end = nullptr;
    const double parsed = std::wcstod(owned.c_str(), &end);
    if (end == owned.c_str() || *end != L'\0' || !std::isfinite(parsed)) return fallback;
    return std::clamp(parsed, minimum, maximum);
}

std::wstring format_double(double value, int precision) {
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(precision) << value;
    std::wstring result = stream.str();
    while (result.size() > 1 && result.back() == L'0') result.pop_back();
    if (!result.empty() && result.back() == L'.') result.pop_back();
    return result;
}

std::string encoded_line(std::string_view key, std::wstring_view value) {
    return std::string(key) + "=" +
        common::wide_to_utf8(common::escape_field(value)) + "\n";
}

Contact parse_contact(const std::vector<std::wstring>& fields, bool legacy) {
    Contact contact;
    std::size_t offset = 0;
    if (!legacy) {
        contact.name = common::unescape_field(fields[0]);
        offset = 1;
    }
    contact.host = common::unescape_field(fields[offset]);
    if (legacy) contact.name = contact.host;
    contact.port = static_cast<std::uint16_t>(parse_long(fields[offset + 1], 49740, 1, 65535));
    contact.gain = parse_double(fields[offset + 2], 1.0, 0.0, 3.0);
    contact.self_duck = parse_bool(fields[offset + 3], true);
    contact.duck_db = parse_double(fields[offset + 4], 12.0, 0.0, 60.0);
    contact.duck_threshold = parse_double(fields[offset + 5], 0.2, 0.0, 1.0);
    contact.duck_attack_ms = static_cast<int>(parse_long(fields[offset + 6], 8, 0, 1000));
    contact.duck_hold_ms = static_cast<int>(parse_long(fields[offset + 7], 80, 0, 1000));
    contact.duck_release_ms = static_cast<int>(parse_long(fields[offset + 8], 120, 0, 5000));
    if (!legacy && fields.size() >= 11) contact.muted = parse_bool(fields[10]);
    if (!legacy && fields.size() >= 12) {
        contact.ptt_hotkey = decode_hotkey(common::unescape_field(fields[11]));
    }
    if (!legacy && fields.size() >= 13) contact.global_ptt_enabled = parse_bool(fields[12], true);
    if (!legacy && fields.size() >= 14) {
        contact.receive_buffer_ms = static_cast<int>(parse_long(fields[13], 20, 5, 100));
    }
    return contact;
}

} // namespace

std::wstring encode_hotkey(const Hotkey& hotkey) {
    if (!hotkey.valid()) return {};
    return std::to_wstring(hotkey.modifiers) + L":" + std::to_wstring(hotkey.vk);
}

Hotkey decode_hotkey(std::wstring_view value) {
    const std::size_t colon = value.find(L':');
    if (colon == std::wstring_view::npos || colon == 0 || colon + 1 >= value.size()) return {};
    const std::wstring modifiers_text(value.substr(0, colon));
    const std::wstring key_text(value.substr(colon + 1));
    wchar_t* modifiers_end = nullptr;
    wchar_t* key_end = nullptr;
    const unsigned long modifiers = std::wcstoul(modifiers_text.c_str(), &modifiers_end, 10);
    const unsigned long key = std::wcstoul(key_text.c_str(), &key_end, 10);
    if (*modifiers_end != L'\0' || *key_end != L'\0' || key == 0 || key > 0xff) return {};
    return Hotkey{static_cast<std::uint32_t>(modifiers) & 0x0fu, static_cast<UINT>(key)};
}

std::string serialize_settings(const AppSettings& settings) {
    std::string output = "version=1\n";
    output += encoded_line("window_width", std::to_wstring(settings.window_width));
    output += encoded_line("window_height", std::to_wstring(settings.window_height));
    output += encoded_line("language", language_value(settings.language));
    output += encoded_line("hotkey_ptt_all", encode_hotkey(settings.ptt_all_hotkey));
    output += encoded_line("debug_console", settings.debug_console_visible ? L"1" : L"0");
    output += encoded_line("local_port", std::to_wstring(settings.local_port));
    output += encoded_line("network_adapter", settings.network_adapter_id);
    output += encoded_line("mic_gain", L"1.0");
    output += encoded_line("capture_device", settings.capture_device_selector);
    output += encoded_line("render_device", settings.render_device_selector);

    for (const Contact& contact : settings.contacts) {
        const std::wstring fields =
            common::escape_field(contact.name) + L"\t" +
            common::escape_field(contact.host) + L"\t" +
            std::to_wstring(contact.port) + L"\t" +
            format_double(contact.gain, 3) + L"\t" +
            (contact.self_duck ? L"1" : L"0") + L"\t" +
            format_double(contact.duck_db, 3) + L"\t" +
            format_double(contact.duck_threshold, 5) + L"\t" +
            std::to_wstring(contact.duck_attack_ms) + L"\t" +
            std::to_wstring(contact.duck_hold_ms) + L"\t" +
            std::to_wstring(contact.duck_release_ms) + L"\t" +
            (contact.muted ? L"1" : L"0") + L"\t" +
            common::escape_field(encode_hotkey(contact.ptt_hotkey)) + L"\t" +
            (contact.global_ptt_enabled ? L"1" : L"0") + L"\t" +
            std::to_wstring(contact.receive_buffer_ms);
        output += "contact=" + common::wide_to_utf8(fields) + "\n";
    }
    return output;
}

AppSettings parse_settings(std::string_view serialized) {
    AppSettings settings;
    std::istringstream input{std::string(serialized)};
    std::string raw_line;
    while (std::getline(input, raw_line)) {
        if (!raw_line.empty() && raw_line.back() == '\r') raw_line.pop_back();
        const std::size_t equals = raw_line.find('=');
        if (equals == std::string::npos) continue;
        const std::string_view key(raw_line.data(), equals);
        const std::string_view raw_value(raw_line.data() + equals + 1, raw_line.size() - equals - 1);
        const std::wstring value = common::unescape_field(common::utf8_to_wide(raw_value));

        if (key == "window_width") settings.window_width = static_cast<int>(parse_long(value, 600, 1, 32767));
        else if (key == "window_height") settings.window_height = static_cast<int>(parse_long(value, 650, 1, 32767));
        else if (key == "language") settings.language = parse_language(value);
        else if (key == "hotkey_ptt_all") settings.ptt_all_hotkey = decode_hotkey(value);
        else if (key == "debug_console") settings.debug_console_visible = parse_bool(value);
        else if (key == "local_port") settings.local_port = static_cast<std::uint16_t>(parse_long(value, 49740, 1, 65535));
        else if (key == "network_adapter") settings.network_adapter_id = value;
        else if (key == "capture_device") settings.capture_device_selector = value;
        else if (key == "render_device") settings.render_device_selector = value;
        else if (key == "contact") {
            const std::vector<std::wstring> fields = common::split_fields(common::utf8_to_wide(raw_value), L'\t');
            if (fields.size() >= 10) settings.contacts.push_back(parse_contact(fields, false));
            else if (fields.size() >= 9) settings.contacts.push_back(parse_contact(fields, true));
        }
    }
    return settings;
}

bool save_settings_file(const std::wstring& path, const AppSettings& settings) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    const std::string serialized = serialize_settings(settings);
    file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    return file.good();
}

bool load_settings_file(const std::wstring& path, AppSettings& settings) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream contents;
    contents << file.rdbuf();
    settings = parse_settings(contents.str());
    return true;
}

} // namespace lanspeak::gui
