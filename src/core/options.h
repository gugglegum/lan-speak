#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mmdeviceapi.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lanspeak::core {

inline constexpr std::uint16_t kDefaultUdpPort = 49740;

struct RoomPeerOptions {
    std::wstring host = L"127.0.0.1";
    std::uint16_t port = kDefaultUdpPort;
    double gain = 1.0;
    double self_duck_db = 0.0;
    double self_duck_threshold = 0.02;
    int self_duck_attack_ms = 8;
    int self_duck_hold_ms = 80;
    int self_duck_release_ms = 120;
    bool global_ptt_enabled = true;
};

struct ProbeOptions {
    enum class Mode {
        probe,
        list_devices,
        capture_test,
        udp_recv,
        udp_send,
        udp_loopback,
        udp_play,
        udp_audio_loopback,
        duplex,
        room,
        tone_test
    };

    bool include_exclusive_checks = false;
    bool show_help = false;
    Mode mode = Mode::probe;
    int capture_test_seconds = 0;
    int udp_seconds = 5;
    std::wstring udp_host = L"127.0.0.1";
    std::uint16_t udp_port = kDefaultUdpPort;
    std::wstring peer_host = L"127.0.0.1";
    std::uint16_t peer_port = kDefaultUdpPort;
    std::uint16_t local_port = kDefaultUdpPort;
    double input_gain = 1.0;
    double output_gain = 1.0;
    double tone_frequency = 440.0;
    ERole capture_role = eCommunications;
    ERole render_role = eCommunications;
    std::optional<std::wstring> capture_device_selector;
    std::optional<std::wstring> render_device_selector;
    std::vector<RoomPeerOptions> room_peers;
    bool room_listen_only = false;
    bool input_muted = false;
    std::uintptr_t telemetry_handle_value = 0;
    std::uintptr_t control_handle_value = 0;
    double self_duck_db = 0.0;
    int self_duck_hold_ms = 80;
    int self_duck_attack_ms = 8;
    int self_duck_release_ms = 120;
    double self_duck_threshold = 0.02;
};

ProbeOptions parse_options(int argc, wchar_t* argv[]);
void print_usage();
std::optional<UINT> parse_zero_based_index(const std::wstring& text);
std::wstring lowercase_copy(std::wstring text);

} // namespace lanspeak::core
