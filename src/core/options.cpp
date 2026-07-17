#include "core/options.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <string>

namespace lanspeak::core {

std::optional<int> parse_positive_seconds(const std::wstring& text) {
    if (text.empty()) {
        return std::nullopt;
    }

    int value = 0;
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<int>(ch - L'0');
        if (value > 3600) {
            return std::nullopt;
        }
    }

    if (value <= 0) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint16_t> parse_udp_port(const std::wstring& text) {
    if (text.empty()) {
        return std::nullopt;
    }

    int value = 0;
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<int>(ch - L'0');
        if (value > 65535) {
            return std::nullopt;
        }
    }

    if (value <= 0) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

std::optional<UINT> parse_zero_based_index(const std::wstring& text) {
    if (text.empty()) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(ch - L'0');
        if (value > std::numeric_limits<UINT>::max()) {
            return std::nullopt;
        }
    }

    return static_cast<UINT>(value);
}

std::optional<double> parse_gain_value(const std::wstring& text) {
    if (text.empty()) {
        return std::nullopt;
    }

    wchar_t* end = nullptr;
    const double value = std::wcstod(text.c_str(), &end);
    if (end == text.c_str() || *end != L'\0' || !std::isfinite(value) || value < 0.0 || value > 8.0) {
        return std::nullopt;
    }

    return value;
}

std::optional<double> parse_double_range(const std::wstring& text, double min_value, double max_value) {
    if (text.empty()) {
        return std::nullopt;
    }

    wchar_t* end = nullptr;
    const double value = std::wcstod(text.c_str(), &end);
    if (end == text.c_str() || *end != L'\0' || !std::isfinite(value) ||
        value < min_value || value > max_value) {
        return std::nullopt;
    }

    return value;
}

std::optional<int> parse_int_range(const std::wstring& text, int min_value, int max_value) {
    if (text.empty() || min_value > max_value) {
        return std::nullopt;
    }

    int value = 0;
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<int>(ch - L'0');
        if (value > max_value) {
            return std::nullopt;
        }
    }

    if (value < min_value) {
        return std::nullopt;
    }

    return value;
}

std::optional<std::uintptr_t> parse_handle_value(const std::wstring& text) {
    if (text.empty()) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') {
            return std::nullopt;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - L'0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }

    if (value == 0 || value > static_cast<std::uint64_t>(std::numeric_limits<std::uintptr_t>::max())) {
        return std::nullopt;
    }

    return static_cast<std::uintptr_t>(value);
}

std::optional<ERole> parse_role_value(const std::wstring& text) {
    if (text == L"console") {
        return eConsole;
    }
    if (text == L"multimedia" || text == L"media") {
        return eMultimedia;
    }
    if (text == L"communications" || text == L"communication" || text == L"comm") {
        return eCommunications;
    }

    return std::nullopt;
}

std::wstring lowercase_copy(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return text;
}

bool set_mode(ProbeOptions& options, ProbeOptions::Mode mode, const wchar_t* mode_name) {
    if (options.mode != ProbeOptions::Mode::probe) {
        std::wcout << L"Only one runtime mode can be selected; conflict at " << mode_name << L"\n";
        options.show_help = true;
        return false;
    }
    options.mode = mode;
    return true;
}

ProbeOptions parse_options(int argc, wchar_t* argv[]) {
    ProbeOptions options;

    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index] != nullptr ? argv[index] : L"";
        if (argument == L"--exclusive") {
            options.include_exclusive_checks = true;
        } else if (argument == L"--list-devices") {
            set_mode(options, ProbeOptions::Mode::list_devices, L"--list-devices");
        } else if (argument == L"--test-seconds") {
            if (index + 1 >= argc) {
                std::wcout << L"--test-seconds requires <seconds>\n";
                options.show_help = true;
                continue;
            }
            const std::optional<int> seconds = parse_positive_seconds(argv[++index]);
            if (!seconds) {
                std::wcout << L"Invalid --test-seconds value; expected 1..3600\n";
                options.show_help = true;
            } else {
                options.udp_seconds = *seconds;
            }
        } else if (argument == L"--bind-address") {
            if (index + 1 >= argc) {
                std::wcout << L"--bind-address requires an IPv4 address\n";
                options.show_help = true;
                continue;
            }
            options.bind_address = argv[++index] != nullptr ? argv[index] : L"";
            if (options.bind_address.empty()) {
                std::wcout << L"--bind-address cannot be empty\n";
                options.show_help = true;
            }
        } else if (argument.rfind(L"--bind-address=", 0) == 0) {
            options.bind_address = argument.substr(std::wstring(L"--bind-address=").size());
            if (options.bind_address.empty()) {
                std::wcout << L"--bind-address cannot be empty\n";
                options.show_help = true;
            }
        } else if (argument == L"--input-gain") {
            if (index + 1 >= argc) {
                std::wcout << L"--input-gain requires <0..8>\n";
                options.show_help = true;
                continue;
            }
            const std::optional<double> gain = parse_gain_value(argv[++index]);
            if (!gain) {
                std::wcout << L"Invalid --input-gain value; expected 0..8 using dot decimal separator\n";
                options.show_help = true;
            } else {
                options.input_gain = *gain;
            }
        } else if (argument.rfind(L"--input-gain=", 0) == 0) {
            const std::optional<double> gain =
                parse_gain_value(argument.substr(std::wstring(L"--input-gain=").size()));
            if (!gain) {
                std::wcout << L"Invalid --input-gain value; expected 0..8 using dot decimal separator\n";
                options.show_help = true;
            } else {
                options.input_gain = *gain;
            }
        } else if (argument == L"--output-gain") {
            if (index + 1 >= argc) {
                std::wcout << L"--output-gain requires <0..8>\n";
                options.show_help = true;
                continue;
            }
            const std::optional<double> gain = parse_gain_value(argv[++index]);
            if (!gain) {
                std::wcout << L"Invalid --output-gain value; expected 0..8 using dot decimal separator\n";
                options.show_help = true;
            } else {
                options.output_gain = *gain;
            }
        } else if (argument.rfind(L"--output-gain=", 0) == 0) {
            const std::optional<double> gain =
                parse_gain_value(argument.substr(std::wstring(L"--output-gain=").size()));
            if (!gain) {
                std::wcout << L"Invalid --output-gain value; expected 0..8 using dot decimal separator\n";
                options.show_help = true;
            } else {
                options.output_gain = *gain;
            }
        } else if (argument == L"--capture-role") {
            if (index + 1 >= argc) {
                std::wcout << L"--capture-role requires console, multimedia, or communications\n";
                options.show_help = true;
                continue;
            }
            const std::optional<ERole> role = parse_role_value(argv[++index]);
            if (!role) {
                std::wcout << L"Invalid --capture-role value; expected console, multimedia, or communications\n";
                options.show_help = true;
            } else {
                options.capture_role = *role;
            }
        } else if (argument.rfind(L"--capture-role=", 0) == 0) {
            const std::optional<ERole> role =
                parse_role_value(argument.substr(std::wstring(L"--capture-role=").size()));
            if (!role) {
                std::wcout << L"Invalid --capture-role value; expected console, multimedia, or communications\n";
                options.show_help = true;
            } else {
                options.capture_role = *role;
            }
        } else if (argument == L"--render-role") {
            if (index + 1 >= argc) {
                std::wcout << L"--render-role requires console, multimedia, or communications\n";
                options.show_help = true;
                continue;
            }
            const std::optional<ERole> role = parse_role_value(argv[++index]);
            if (!role) {
                std::wcout << L"Invalid --render-role value; expected console, multimedia, or communications\n";
                options.show_help = true;
            } else {
                options.render_role = *role;
            }
        } else if (argument.rfind(L"--render-role=", 0) == 0) {
            const std::optional<ERole> role =
                parse_role_value(argument.substr(std::wstring(L"--render-role=").size()));
            if (!role) {
                std::wcout << L"Invalid --render-role value; expected console, multimedia, or communications\n";
                options.show_help = true;
            } else {
                options.render_role = *role;
            }
        } else if (argument == L"--capture-device") {
            if (index + 1 >= argc) {
                std::wcout << L"--capture-device requires index, name fragment, or endpoint id\n";
                options.show_help = true;
                continue;
            }
            options.capture_device_selector = argv[++index] != nullptr ? argv[index] : L"";
            if (options.capture_device_selector->empty()) {
                std::wcout << L"--capture-device cannot be empty\n";
                options.show_help = true;
            }
        } else if (argument.rfind(L"--capture-device=", 0) == 0) {
            options.capture_device_selector = argument.substr(std::wstring(L"--capture-device=").size());
            if (options.capture_device_selector->empty()) {
                std::wcout << L"--capture-device cannot be empty\n";
                options.show_help = true;
            }
        } else if (argument == L"--render-device") {
            if (index + 1 >= argc) {
                std::wcout << L"--render-device requires index, name fragment, or endpoint id\n";
                options.show_help = true;
                continue;
            }
            options.render_device_selector = argv[++index] != nullptr ? argv[index] : L"";
            if (options.render_device_selector->empty()) {
                std::wcout << L"--render-device cannot be empty\n";
                options.show_help = true;
            }
        } else if (argument.rfind(L"--render-device=", 0) == 0) {
            options.render_device_selector = argument.substr(std::wstring(L"--render-device=").size());
            if (options.render_device_selector->empty()) {
                std::wcout << L"--render-device cannot be empty\n";
                options.show_help = true;
            }
        } else if (argument == L"--telemetry-handle") {
            if (index + 1 >= argc) {
                std::wcout << L"--telemetry-handle requires an inherited handle value\n";
                options.show_help = true;
                continue;
            }
            const std::optional<std::uintptr_t> value = parse_handle_value(argv[++index]);
            if (!value) {
                std::wcout << L"Invalid --telemetry-handle value\n";
                options.show_help = true;
            } else {
                options.telemetry_handle_value = *value;
            }
        } else if (argument == L"--control-handle") {
            if (index + 1 >= argc) {
                std::wcout << L"--control-handle requires an inherited handle value\n";
                options.show_help = true;
                continue;
            }
            const std::optional<std::uintptr_t> value = parse_handle_value(argv[++index]);
            if (!value) {
                std::wcout << L"Invalid --control-handle value\n";
                options.show_help = true;
            } else {
                options.control_handle_value = *value;
            }
        } else if (argument == L"--input-muted") {
            options.input_muted = true;
        } else if (argument == L"--self-duck-db") {
            if (index + 1 >= argc) {
                std::wcout << L"--self-duck-db requires <0..60>\n";
                options.show_help = true;
                continue;
            }
            const std::optional<double> value = parse_double_range(argv[++index], 0.0, 60.0);
            if (!value) {
                std::wcout << L"Invalid --self-duck-db value; expected 0..60 using dot decimal separator\n";
                options.show_help = true;
            } else {
                options.self_duck_db = *value;
            }
        } else if (argument.rfind(L"--self-duck-db=", 0) == 0) {
            const std::optional<double> value =
                parse_double_range(argument.substr(std::wstring(L"--self-duck-db=").size()), 0.0, 60.0);
            if (!value) {
                std::wcout << L"Invalid --self-duck-db value; expected 0..60 using dot decimal separator\n";
                options.show_help = true;
            } else {
                options.self_duck_db = *value;
            }
        } else if (argument == L"--self-duck-hold-ms") {
            if (index + 1 >= argc) {
                std::wcout << L"--self-duck-hold-ms requires <0..1000>\n";
                options.show_help = true;
                continue;
            }
            const std::optional<int> value = parse_int_range(argv[++index], 0, 1000);
            if (!value) {
                std::wcout << L"Invalid --self-duck-hold-ms value; expected 0..1000\n";
                options.show_help = true;
            } else {
                options.self_duck_hold_ms = *value;
            }
        } else if (argument.rfind(L"--self-duck-hold-ms=", 0) == 0) {
            const std::optional<int> value =
                parse_int_range(argument.substr(std::wstring(L"--self-duck-hold-ms=").size()), 0, 1000);
            if (!value) {
                std::wcout << L"Invalid --self-duck-hold-ms value; expected 0..1000\n";
                options.show_help = true;
            } else {
                options.self_duck_hold_ms = *value;
            }
        } else if (argument == L"--self-duck-attack-ms") {
            if (index + 1 >= argc) {
                std::wcout << L"--self-duck-attack-ms requires <0..1000>\n";
                options.show_help = true;
                continue;
            }
            const std::optional<int> value = parse_int_range(argv[++index], 0, 1000);
            if (!value) {
                std::wcout << L"Invalid --self-duck-attack-ms value; expected 0..1000\n";
                options.show_help = true;
            } else {
                options.self_duck_attack_ms = *value;
            }
        } else if (argument.rfind(L"--self-duck-attack-ms=", 0) == 0) {
            const std::optional<int> value =
                parse_int_range(argument.substr(std::wstring(L"--self-duck-attack-ms=").size()), 0, 1000);
            if (!value) {
                std::wcout << L"Invalid --self-duck-attack-ms value; expected 0..1000\n";
                options.show_help = true;
            } else {
                options.self_duck_attack_ms = *value;
            }
        } else if (argument == L"--self-duck-release-ms") {
            if (index + 1 >= argc) {
                std::wcout << L"--self-duck-release-ms requires <0..5000>\n";
                options.show_help = true;
                continue;
            }
            const std::optional<int> value = parse_int_range(argv[++index], 0, 5000);
            if (!value) {
                std::wcout << L"Invalid --self-duck-release-ms value; expected 0..5000\n";
                options.show_help = true;
            } else {
                options.self_duck_release_ms = *value;
            }
        } else if (argument.rfind(L"--self-duck-release-ms=", 0) == 0) {
            const std::optional<int> value =
                parse_int_range(argument.substr(std::wstring(L"--self-duck-release-ms=").size()), 0, 5000);
            if (!value) {
                std::wcout << L"Invalid --self-duck-release-ms value; expected 0..5000\n";
                options.show_help = true;
            } else {
                options.self_duck_release_ms = *value;
            }
        } else if (argument == L"--self-duck-threshold") {
            if (index + 1 >= argc) {
                std::wcout << L"--self-duck-threshold requires <0..1>\n";
                options.show_help = true;
                continue;
            }
            const std::optional<double> value = parse_double_range(argv[++index], 0.0, 1.0);
            if (!value) {
                std::wcout << L"Invalid --self-duck-threshold value; expected 0..1 using dot decimal separator\n";
                options.show_help = true;
            } else {
                options.self_duck_threshold = *value;
            }
        } else if (argument.rfind(L"--self-duck-threshold=", 0) == 0) {
            const std::optional<double> value =
                parse_double_range(argument.substr(std::wstring(L"--self-duck-threshold=").size()), 0.0, 1.0);
            if (!value) {
                std::wcout << L"Invalid --self-duck-threshold value; expected 0..1 using dot decimal separator\n";
                options.show_help = true;
            } else {
                options.self_duck_threshold = *value;
            }
        } else if (argument == L"--capture-test") {
            if (!set_mode(options, ProbeOptions::Mode::capture_test, L"--capture-test")) {
                continue;
            }
            options.capture_test_seconds = 5;
            if (index + 1 < argc) {
                const std::wstring next = argv[index + 1] != nullptr ? argv[index + 1] : L"";
                if (!next.empty() && next[0] != L'-') {
                    const std::optional<int> seconds = parse_positive_seconds(next);
                    if (!seconds) {
                        std::wcout << L"Invalid --capture-test seconds: " << next << L"\n";
                        options.show_help = true;
                    } else {
                        options.capture_test_seconds = *seconds;
                        ++index;
                    }
                }
            }
        } else if (argument.rfind(L"--capture-test=", 0) == 0) {
            if (!set_mode(options, ProbeOptions::Mode::capture_test, L"--capture-test")) {
                continue;
            }
            const std::wstring value = argument.substr(std::wstring(L"--capture-test=").size());
            const std::optional<int> seconds = parse_positive_seconds(value);
            if (!seconds) {
                std::wcout << L"Invalid --capture-test seconds: " << value << L"\n";
                options.show_help = true;
            } else {
                options.capture_test_seconds = *seconds;
            }
        } else if (argument == L"--tone-test") {
            if (!set_mode(options, ProbeOptions::Mode::tone_test, L"--tone-test")) {
                continue;
            }
            options.udp_seconds = 5;
            if (index + 1 < argc) {
                const std::wstring next = argv[index + 1] != nullptr ? argv[index + 1] : L"";
                if (!next.empty() && next[0] != L'-') {
                    const std::optional<int> seconds = parse_positive_seconds(next);
                    if (!seconds) {
                        std::wcout << L"Invalid --tone-test seconds: " << next << L"\n";
                        options.show_help = true;
                    } else {
                        options.udp_seconds = *seconds;
                        ++index;
                    }
                }
            }
        } else if (argument == L"--udp-recv") {
            if (!set_mode(options, ProbeOptions::Mode::udp_recv, L"--udp-recv")) {
                continue;
            }
            if (index + 1 >= argc) {
                std::wcout << L"--udp-recv requires <port>\n";
                options.show_help = true;
                continue;
            }
            const std::optional<std::uint16_t> port = parse_udp_port(argv[++index]);
            if (!port) {
                std::wcout << L"Invalid UDP port for --udp-recv\n";
                options.show_help = true;
                continue;
            }
            options.udp_port = *port;
            options.udp_seconds = 10;
            if (index + 1 < argc) {
                const std::wstring next = argv[index + 1] != nullptr ? argv[index + 1] : L"";
                if (!next.empty() && next[0] != L'-') {
                    const std::optional<int> seconds = parse_positive_seconds(next);
                    if (!seconds) {
                        std::wcout << L"Invalid --udp-recv seconds: " << next << L"\n";
                        options.show_help = true;
                    } else {
                        options.udp_seconds = *seconds;
                        ++index;
                    }
                }
            }
        } else if (argument == L"--udp-send") {
            if (!set_mode(options, ProbeOptions::Mode::udp_send, L"--udp-send")) {
                continue;
            }
            if (index + 2 >= argc) {
                std::wcout << L"--udp-send requires <host> <port>\n";
                options.show_help = true;
                continue;
            }
            options.udp_host = argv[++index] != nullptr ? argv[index] : L"";
            const std::optional<std::uint16_t> port = parse_udp_port(argv[++index]);
            if (!port || options.udp_host.empty()) {
                std::wcout << L"Invalid host or UDP port for --udp-send\n";
                options.show_help = true;
                continue;
            }
            options.udp_port = *port;
            options.udp_seconds = 5;
            if (index + 1 < argc) {
                const std::wstring next = argv[index + 1] != nullptr ? argv[index + 1] : L"";
                if (!next.empty() && next[0] != L'-') {
                    const std::optional<int> seconds = parse_positive_seconds(next);
                    if (!seconds) {
                        std::wcout << L"Invalid --udp-send seconds: " << next << L"\n";
                        options.show_help = true;
                    } else {
                        options.udp_seconds = *seconds;
                        ++index;
                    }
                }
            }
        } else if (argument == L"--udp-loopback") {
            if (!set_mode(options, ProbeOptions::Mode::udp_loopback, L"--udp-loopback")) {
                continue;
            }
            options.udp_seconds = 5;
            options.udp_host = L"127.0.0.1";
            options.udp_port = kDefaultUdpPort;
            if (index + 1 < argc) {
                const std::wstring next = argv[index + 1] != nullptr ? argv[index + 1] : L"";
                if (!next.empty() && next[0] != L'-') {
                    const std::optional<int> seconds = parse_positive_seconds(next);
                    if (!seconds) {
                        std::wcout << L"Invalid --udp-loopback seconds: " << next << L"\n";
                        options.show_help = true;
                    } else {
                        options.udp_seconds = *seconds;
                        ++index;
                    }
                }
            }
            if (index + 1 < argc) {
                const std::wstring next = argv[index + 1] != nullptr ? argv[index + 1] : L"";
                if (!next.empty() && next[0] != L'-') {
                    const std::optional<std::uint16_t> port = parse_udp_port(next);
                    if (!port) {
                        std::wcout << L"Invalid --udp-loopback port: " << next << L"\n";
                        options.show_help = true;
                    } else {
                        options.udp_port = *port;
                        ++index;
                    }
                }
            }
        } else if (argument == L"--udp-play") {
            if (!set_mode(options, ProbeOptions::Mode::udp_play, L"--udp-play")) {
                continue;
            }
            if (index + 1 >= argc) {
                std::wcout << L"--udp-play requires <port>\n";
                options.show_help = true;
                continue;
            }
            const std::optional<std::uint16_t> port = parse_udp_port(argv[++index]);
            if (!port) {
                std::wcout << L"Invalid UDP port for --udp-play\n";
                options.show_help = true;
                continue;
            }
            options.udp_port = *port;
            options.udp_seconds = 10;
            if (index + 1 < argc) {
                const std::wstring next = argv[index + 1] != nullptr ? argv[index + 1] : L"";
                if (!next.empty() && next[0] != L'-') {
                    const std::optional<int> seconds = parse_positive_seconds(next);
                    if (!seconds) {
                        std::wcout << L"Invalid --udp-play seconds: " << next << L"\n";
                        options.show_help = true;
                    } else {
                        options.udp_seconds = *seconds;
                        ++index;
                    }
                }
            }
        } else if (argument == L"--udp-audio-loopback") {
            if (!set_mode(options, ProbeOptions::Mode::udp_audio_loopback, L"--udp-audio-loopback")) {
                continue;
            }
            options.udp_seconds = 5;
            options.udp_host = L"127.0.0.1";
            options.udp_port = kDefaultUdpPort;
            if (index + 1 < argc) {
                const std::wstring next = argv[index + 1] != nullptr ? argv[index + 1] : L"";
                if (!next.empty() && next[0] != L'-') {
                    const std::optional<int> seconds = parse_positive_seconds(next);
                    if (!seconds) {
                        std::wcout << L"Invalid --udp-audio-loopback seconds: " << next << L"\n";
                        options.show_help = true;
                    } else {
                        options.udp_seconds = *seconds;
                        ++index;
                    }
                }
            }
            if (index + 1 < argc) {
                const std::wstring next = argv[index + 1] != nullptr ? argv[index + 1] : L"";
                if (!next.empty() && next[0] != L'-') {
                    const std::optional<std::uint16_t> port = parse_udp_port(next);
                    if (!port) {
                        std::wcout << L"Invalid --udp-audio-loopback port: " << next << L"\n";
                        options.show_help = true;
                    } else {
                        options.udp_port = *port;
                        ++index;
                    }
                }
            }
        } else if (argument == L"--duplex") {
            if (!set_mode(options, ProbeOptions::Mode::duplex, L"--duplex")) {
                continue;
            }
            if (index + 3 >= argc) {
                std::wcout << L"--duplex requires <local-port> <peer-host> <peer-port>\n";
                options.show_help = true;
                continue;
            }

            const std::optional<std::uint16_t> local_port = parse_udp_port(argv[++index]);
            options.peer_host = argv[++index] != nullptr ? argv[index] : L"";
            const std::optional<std::uint16_t> peer_port = parse_udp_port(argv[++index]);
            if (!local_port || !peer_port || options.peer_host.empty()) {
                std::wcout << L"Invalid --duplex local port, peer host, or peer port\n";
                options.show_help = true;
                continue;
            }

            options.local_port = *local_port;
            options.peer_port = *peer_port;
            options.udp_seconds = 0;
            if (index + 1 < argc) {
                const std::wstring next = argv[index + 1] != nullptr ? argv[index + 1] : L"";
                if (!next.empty() && next[0] != L'-') {
                    const std::optional<int> seconds = parse_positive_seconds(next);
                    if (!seconds) {
                        std::wcout << L"Invalid --duplex seconds: " << next << L"\n";
                        options.show_help = true;
                    } else {
                        options.udp_seconds = *seconds;
                        ++index;
                    }
                }
            }
        } else if (argument == L"--room") {
            if (!set_mode(options, ProbeOptions::Mode::room, L"--room")) {
                continue;
            }
            if (index + 1 >= argc) {
                std::wcout << L"--room requires <local-port>\n";
                options.show_help = true;
                continue;
            }

            const std::optional<std::uint16_t> local_port = parse_udp_port(argv[++index]);
            if (!local_port) {
                std::wcout << L"Invalid --room local port\n";
                options.show_help = true;
                continue;
            }
            options.local_port = *local_port;
            options.udp_seconds = 0;
        } else if (argument == L"--listen-only") {
            options.room_listen_only = true;
        } else if (argument == L"--peer") {
            if (index + 8 >= argc) {
                std::wcout << L"--peer requires <host> <port> <gain> <duck-db> <threshold> <attack-ms> <hold-ms> <release-ms> [global-ptt] [receive-buffer-ms]\n";
                options.show_help = true;
                continue;
            }

            RoomPeerOptions peer;
            peer.host = argv[++index] != nullptr ? argv[index] : L"";
            const std::optional<std::uint16_t> port = parse_udp_port(argv[++index]);
            const std::optional<double> gain = parse_gain_value(argv[++index]);
            const std::optional<double> duck_db = parse_double_range(argv[++index], 0.0, 60.0);
            const std::optional<double> threshold = parse_double_range(argv[++index], 0.0, 1.0);
            const std::optional<int> attack_ms = parse_int_range(argv[++index], 0, 1000);
            const std::optional<int> hold_ms = parse_int_range(argv[++index], 0, 1000);
            const std::optional<int> release_ms = parse_int_range(argv[++index], 0, 5000);
            bool global_ptt_enabled = true;
            if (index + 1 < argc) {
                const std::wstring next = argv[index + 1] != nullptr ? argv[index + 1] : L"";
                if (next == L"0" || next == L"1") {
                    global_ptt_enabled = next == L"1";
                    ++index;
                }
            }
            int receive_buffer_ms = kDefaultReceiveBufferMs;
            if (index + 1 < argc) {
                const std::wstring next = argv[index + 1] != nullptr ? argv[index + 1] : L"";
                if (const std::optional<int> parsed = parse_int_range(next, 1, 500)) {
                    receive_buffer_ms = *parsed;
                    ++index;
                }
            }

            if (peer.host.empty() || !port || !gain || !duck_db || !threshold ||
                !attack_ms || !hold_ms || !release_ms) {
                std::wcout << L"Invalid --peer values\n";
                options.show_help = true;
                continue;
            }

            peer.port = *port;
            peer.gain = *gain;
            peer.self_duck_db = *duck_db;
            peer.self_duck_threshold = *threshold;
            peer.self_duck_attack_ms = *attack_ms;
            peer.self_duck_hold_ms = *hold_ms;
            peer.self_duck_release_ms = *release_ms;
            peer.receive_buffer_ms = receive_buffer_ms;
            peer.global_ptt_enabled = global_ptt_enabled;
            options.room_peers.push_back(peer);
        } else if (argument == L"--help" || argument == L"-h" || argument == L"/?") {
            options.show_help = true;
        } else {
            std::wcout << L"Unknown argument: " << argument << L"\n";
            options.show_help = true;
        }
    }

    return options;
}

void print_usage() {
    std::wcout << L"Usage: LanSpeakCore.exe [--exclusive] [--input-gain N] [--output-gain N]\n";
    std::wcout << L"                     [--capture-role ROLE] [--render-role ROLE]\n";
    std::wcout << L"                     [--capture-device SELECTOR] [--render-device SELECTOR]\n";
    std::wcout << L"                     [--bind-address IPV4]\n";
    std::wcout << L"                     [--telemetry-handle HANDLE] [--control-handle HANDLE] [--input-muted]\n";
    std::wcout << L"                     [--self-duck-db DB] [--self-duck-hold-ms MS]\n";
    std::wcout << L"                     [--self-duck-attack-ms MS] [--self-duck-release-ms MS]\n";
    std::wcout << L"                     [--self-duck-threshold LEVEL]\n";
    std::wcout << L"       LanSpeakCore.exe --list-devices\n";
    std::wcout << L"       LanSpeakCore.exe --capture-test [seconds]\n";
    std::wcout << L"       LanSpeakCore.exe [gain options] --tone-test [seconds]\n";
    std::wcout << L"       LanSpeakCore.exe [gain options] --udp-loopback [seconds] [port]\n";
    std::wcout << L"       LanSpeakCore.exe --udp-recv <port> [seconds]\n";
    std::wcout << L"       LanSpeakCore.exe [gain options] --udp-send <host> <port> [seconds]\n";
    std::wcout << L"       LanSpeakCore.exe [gain options] --udp-play <port> [seconds]\n";
    std::wcout << L"       LanSpeakCore.exe [gain options] --udp-audio-loopback [seconds] [port]\n";
    std::wcout << L"       LanSpeakCore.exe [gain options] --duplex <local-port> <peer-host> <peer-port> [seconds]\n\n";
    std::wcout << L"       LanSpeakCore.exe --room <local-port> [--listen-only] --peer <host> <port> <gain> <duck-db> <threshold> <attack-ms> <hold-ms> <release-ms> [global-ptt] [receive-buffer-ms]...\n\n";
    std::wcout << L"Default mode checks shared WASAPI paths only, so it should coexist with CS2 voice.\n";
    std::wcout << L"--list-devices prints active WASAPI endpoints in a stable GUI-friendly format.\n";
    std::wcout << L"--capture-test opens the selected/default capture role in shared/event mode and measures callbacks.\n";
    std::wcout << L"--tone-test plays a direct 440 Hz tone through the selected/default render role.\n";
    std::wcout << L"--udp-loopback runs capture -> UDP -> receiver telemetry on localhost.\n";
    std::wcout << L"--udp-play receives UDP audio and renders it through the selected/default render role.\n";
    std::wcout << L"--udp-audio-loopback runs capture -> UDP -> jitter buffer -> render on localhost.\n";
    std::wcout << L"--duplex sends microphone audio to a peer and plays incoming peer audio until Ctrl+C.\n";
    std::wcout << L"  Add optional [seconds] to --duplex only for bounded tests.\n";
    std::wcout << L"--room is a multi-peer full-mesh voice mode. Repeat --peer for each contact.\n";
    std::wcout << L"  --bind-address selects the local IPv4 address for room receive and send sockets.\n";
    std::wcout << L"  Add --listen-only to --room to receive and play contacts without sending microphone audio.\n";
    std::wcout << L"  Add --input-muted to start room sending with microphone audio muted until a control command opens it.\n";
    std::wcout << L"  Use --test-seconds N with --room only for bounded tests.\n";
    std::wcout << L"--input-gain and --output-gain accept 0..8, default 1.0.\n";
    std::wcout << L"--capture-role and --render-role accept console, multimedia, communications; default console.\n";
    std::wcout << L"--capture-device and --render-device accept an active endpoint index, name fragment, or endpoint id.\n";
    std::wcout << L"--self-duck-db accepts 0..60 dB, default 0/off; attack 8 ms, hold 80 ms, release 120 ms.\n";
    std::wcout << L"--self-duck-threshold accepts 0..1, default 0.02.\n";
    std::wcout << L"Use --exclusive only for lab diagnostics when no game or voice app needs the same devices.\n";
}


} // namespace lanspeak::core
