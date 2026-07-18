#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propkey.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include "common/line_buffer.h"
#include "common/text_codec.h"
#include "core/application.h"
#include "core/audio_math.h"
#include "core/diagnostics.h"
#include "core/jitter_buffer.h"
#include "core/options.h"
#include "core/pcm_audio.h"
#include "core/peer_router.h"
#include "core/presence_tracker.h"
#include "core/room_mixer.h"
#include "core/room_session.h"
#include "core/telemetry_snapshot.h"
#include "core/udp_audio_packet.h"
#include "core/udp_presence_packet.h"
#include "core/wasapi_audio.h"
#include "core/wasapi_devices.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

using lanspeak::core::JitterBuffer;
using lanspeak::core::JitterStats;
using lanspeak::core::EndpointCandidate;
using lanspeak::core::ProbeOptions;
using lanspeak::core::PeerRouter;
using lanspeak::core::PeerPresenceState;
using lanspeak::core::PresenceAction;
using lanspeak::core::PresenceTracker;
using lanspeak::core::RoomMixer;
using lanspeak::core::RoomPeerControl;
using lanspeak::core::RoomPeerOptions;
using lanspeak::core::SampleKind;
using lanspeak::core::TelemetrySnapshot;
using lanspeak::core::UdpAudioPacketHeader;
using lanspeak::core::UdpPresencePacket;
using lanspeak::core::UdpPresenceType;
using lanspeak::core::audio_level_dbfs;
using lanspeak::core::build_mono_pcm16_payload;
using lanspeak::core::collect_active_endpoints;
using lanspeak::core::consume_room_control_bytes;
using lanspeak::core::fill_render_buffer_from_mono_pcm16;
using lanspeak::core::fill_render_buffer_with_tone;
using lanspeak::core::flow_name;
using lanspeak::core::float_to_pcm16;
using lanspeak::core::frames_to_ms;
using lanspeak::core::frames_to_reference_time;
using lanspeak::core::hresult_text;
using lanspeak::core::hresult_hex;
using lanspeak::core::get_device_flow;
using lanspeak::core::get_device_id;
using lanspeak::core::get_friendly_name;
using lanspeak::core::is_wave_subformat;
using lanspeak::core::initialize_room_peer_control;
using lanspeak::core::kDefaultUdpPort;
using lanspeak::core::kSequenceRestartPacketGap;
using lanspeak::core::kUdpAudioMagic;
using lanspeak::core::kUdpAudioVersion;
using lanspeak::core::kUdpSourceFlagTalkActive;
using lanspeak::core::smooth_toward;
using lanspeak::core::packet_peak_abs_sample;
using lanspeak::core::parse_options;
using lanspeak::core::parse_zero_based_index;
using lanspeak::core::print_usage;
using lanspeak::core::lowercase_copy;
using lanspeak::core::reference_time_to_ms;
using lanspeak::core::role_name;
using lanspeak::core::sample_kind;
using lanspeak::core::select_audio_endpoint;
using lanspeak::core::write_room_mix_to_render_buffer;
using lanspeak::core::win32_message;
using lanspeak::core::winsock_error_text;

constexpr double kRoomTelemetryIntervalMs = 33.0;
constexpr std::uint64_t kRoomStreamActivityHoldMs = 200;

struct ComRuntime {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ~ComRuntime() {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
};

struct WinsockRuntime {
    WSADATA data{};
    int result = WSAStartup(MAKEWORD(2, 2), &data);

    WinsockRuntime() = default;

    ~WinsockRuntime() {
        if (result == 0) {
            WSACleanup();
        }
    }

    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};

struct SocketHandle {
    SOCKET value = INVALID_SOCKET;

    SocketHandle() = default;
    explicit SocketHandle(SOCKET socket) : value(socket) {}

    ~SocketHandle() {
        if (value != INVALID_SOCKET) {
            closesocket(value);
        }
    }

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    SocketHandle(SocketHandle&& other) noexcept : value(other.value) {
        other.value = INVALID_SOCKET;
    }

    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this != &other) {
            if (value != INVALID_SOCKET) {
                closesocket(value);
            }
            value = other.value;
            other.value = INVALID_SOCKET;
        }
        return *this;
    }
};

struct CoTaskMemDeleter {
    void operator()(void* ptr) const {
        CoTaskMemFree(ptr);
    }
};

struct Handle {
    HANDLE value = nullptr;

    explicit Handle(HANDLE handle) : value(handle) {}

    ~Handle() {
        if (value != nullptr) {
            CloseHandle(value);
        }
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct MmcssTask {
    HANDLE value = nullptr;

    explicit MmcssTask(const wchar_t* task_name) {
        DWORD task_index = 0;
        value = AvSetMmThreadCharacteristicsW(task_name, &task_index);
    }

    ~MmcssTask() {
        if (value != nullptr) {
            AvRevertMmThreadCharacteristics(value);
        }
    }

    MmcssTask(const MmcssTask&) = delete;
    MmcssTask& operator=(const MmcssTask&) = delete;
};

using WaveFormatPtr = std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter>;

struct FormatCandidate {
    std::wstring name;
    std::vector<std::byte> storage;

    const WAVEFORMATEX& format() const {
        return *reinterpret_cast<const WAVEFORMATEX*>(storage.data());
    }
};

struct SelfDuckingSettings {
    double duck_db = 0.0;
    int hold_ms = 80;
    int attack_ms = 8;
    int release_ms = 120;
    double threshold = 0.02;

    bool enabled() const {
        return duck_db > 0.0 && threshold > 0.0;
    }

    double duck_gain() const {
        return std::pow(10.0, -duck_db / 20.0);
    }
};

struct SelfDuckingState {
    std::atomic<std::uint64_t> active_until_qpc{0};
};

void atomic_extend_to(std::atomic<std::uint64_t>& target, std::uint64_t value) {
    std::uint64_t current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

bool has_duration_limit(int seconds) {
    return seconds > 0;
}

std::wstring duration_text(int seconds) {
    if (!has_duration_limit(seconds)) {
        return L"unlimited";
    }
    return std::to_wstring(seconds) + L" seconds";
}

DWORD wait_ms_for_duration(int seconds, double duration_ms, double elapsed_ms) {
    if (!has_duration_limit(seconds)) {
        return 1000;
    }
    return static_cast<DWORD>(std::min(1000.0, duration_ms - elapsed_ms + 50.0));
}

bool should_stop_for_duration(int seconds, double duration_ms, double elapsed_ms) {
    return has_duration_limit(seconds) && elapsed_ms >= duration_ms;
}

bool should_print_progress_second(int current_second, int last_report_second, int seconds) {
    return current_second > last_report_second && (!has_duration_limit(seconds) || current_second < seconds);
}

struct TimingStats {
    std::uint64_t count = 0;
    double sum_ms = 0.0;
    double min_ms = std::numeric_limits<double>::max();
    double max_ms = 0.0;

    void add(double value_ms) {
        ++count;
        sum_ms += value_ms;
        min_ms = std::min(min_ms, value_ms);
        max_ms = std::max(max_ms, value_ms);
    }

    double average_ms() const {
        return count == 0 ? 0.0 : sum_ms / static_cast<double>(count);
    }

    double printable_min_ms() const {
        return count == 0 ? 0.0 : min_ms;
    }
};

struct CaptureStats {
    std::uint64_t wait_events = 0;
    std::uint64_t wait_timeouts = 0;
    std::uint64_t packets = 0;
    std::uint64_t data_packets = 0;
    std::uint64_t silent_packets = 0;
    std::uint64_t discontinuity_packets = 0;
    std::uint64_t timestamp_error_packets = 0;
    std::uint64_t frames = 0;
    std::uint64_t data_frames = 0;
    std::uint64_t silent_frames = 0;
    UINT32 min_packet_frames = std::numeric_limits<UINT32>::max();
    UINT32 max_packet_frames = 0;
    double peak_abs_sample = 0.0;
    TimingStats callback_intervals;

    void add_packet(UINT32 packet_frames) {
        ++packets;
        frames += packet_frames;
        min_packet_frames = std::min(min_packet_frames, packet_frames);
        max_packet_frames = std::max(max_packet_frames, packet_frames);
    }

    UINT32 printable_min_packet_frames() const {
        return packets == 0 ? 0 : min_packet_frames;
    }
};

struct UdpSendStats {
    std::uint64_t packets_sent = 0;
    std::uint64_t packets_suppressed = 0;
    std::uint64_t send_errors = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t payload_bytes_sent = 0;
    std::uint64_t frames_sent = 0;
    std::uint64_t frames_suppressed = 0;
    TimingStats send_intervals;
    CaptureStats capture;
};

struct UdpReceiveStats {
    std::uint64_t datagrams = 0;
    std::uint64_t valid_packets = 0;
    std::uint64_t invalid_packets = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t payload_bytes_received = 0;
    std::uint64_t frames_received = 0;
    std::uint64_t resampled_packets = 0;
    std::uint64_t resampled_input_frames = 0;
    std::uint64_t resampled_output_frames = 0;
    std::uint32_t last_source_sample_rate = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t out_of_order_or_duplicate = 0;
    std::uint64_t sequence_restarts = 0;
    std::uint64_t expected_sequence = 0;
    bool have_sequence = false;
    TimingStats interarrival;
    TimingStats sender_to_receiver_latency;
};

struct UdpPlayStats {
    UdpReceiveStats receive;
    std::uint64_t render_wait_timeouts = 0;
    std::uint64_t render_wait_failures = 0;
    std::uint64_t render_padding_min = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t render_padding_max = 0;
    std::uint64_t ducked_render_events = 0;
    std::uint64_t ducked_render_frames = 0;
    TimingStats render_event_intervals;
};

class NoGroupingNumpunct : public std::numpunct<wchar_t> {
protected:
    std::string do_grouping() const override {
        return {};
    }
};

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        result.data(),
        required,
        nullptr,
        nullptr);
    return result;
}

std::wstring state_name(DWORD state) {
    std::vector<std::wstring> parts;
    if ((state & DEVICE_STATE_ACTIVE) != 0) {
        parts.emplace_back(L"active");
    }
    if ((state & DEVICE_STATE_DISABLED) != 0) {
        parts.emplace_back(L"disabled");
    }
    if ((state & DEVICE_STATE_NOTPRESENT) != 0) {
        parts.emplace_back(L"not-present");
    }
    if ((state & DEVICE_STATE_UNPLUGGED) != 0) {
        parts.emplace_back(L"unplugged");
    }
    if (parts.empty()) {
        return L"unknown";
    }

    std::wstring text = parts.front();
    for (size_t index = 1; index < parts.size(); ++index) {
        text += L"|";
        text += parts[index];
    }
    return text;
}

std::wstring guid_text(const GUID& guid) {
    wchar_t buffer[64]{};
    if (StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer))) == 0) {
        return L"<guid-format-error>";
    }
    return buffer;
}

GUID wave_subformat_guid(WORD tag) {
    return GUID{
        static_cast<unsigned long>(tag),
        0x0000,
        0x0010,
        {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
}

DWORD default_channel_mask(WORD channels) {
    switch (channels) {
    case 1:
        return 0x4; // SPEAKER_FRONT_CENTER
    case 2:
        return 0x3; // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
    default:
        return 0;
    }
}

std::wstring wave_tag_name(WORD tag) {
    switch (tag) {
    case WAVE_FORMAT_PCM:
        return L"PCM";
    case WAVE_FORMAT_IEEE_FLOAT:
        return L"IEEE_FLOAT";
    case WAVE_FORMAT_EXTENSIBLE:
        return L"EXTENSIBLE";
    default:
        return L"tag " + std::to_wstring(tag);
    }
}

std::wstring extensible_subformat_name(const GUID& guid) {
    if (is_wave_subformat(guid, WAVE_FORMAT_PCM)) {
        return L"PCM";
    }
    if (is_wave_subformat(guid, WAVE_FORMAT_IEEE_FLOAT)) {
        return L"IEEE_FLOAT";
    }
    return guid_text(guid);
}

void print_wave_format(const WAVEFORMATEX& format, const std::wstring& indent) {
    std::wcout << indent << L"format: " << wave_tag_name(format.wFormatTag)
               << L", " << format.nSamplesPerSec << L" Hz"
               << L", channels=" << format.nChannels
               << L", bits=" << format.wBitsPerSample
               << L", blockAlign=" << format.nBlockAlign
               << L", avgBytes/sec=" << format.nAvgBytesPerSec
               << L", cbSize=" << format.cbSize << L"\n";

    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        std::wcout << indent << L"  extensible: subformat="
                   << extensible_subformat_name(extensible.SubFormat)
                   << L", validBits=" << extensible.Samples.wValidBitsPerSample
                   << L", channelMask=0x" << std::hex << std::uppercase
                   << extensible.dwChannelMask << std::dec << L"\n";
    }
}

SocketHandle create_udp_socket() {
    return SocketHandle(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
}

bool bind_udp_socket(
    SocketHandle& socket_handle,
    std::uint16_t port,
    bool loopback_only,
    const std::wstring& bind_address = L"0.0.0.0") {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (loopback_only) {
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (bind_address.empty() || bind_address == L"0.0.0.0") {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (InetPtonW(AF_INET, bind_address.c_str(), &address.sin_addr) != 1) {
        std::wcout << L"Invalid local IPv4 bind address: " << bind_address << L"\n";
        return false;
    }

    if (bind(socket_handle.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        std::wcout << L"bind UDP " << bind_address << L":" << port
                   << L" failed: " << winsock_error_text() << L"\n";
        return false;
    }

    return true;
}

bool resolve_udp_target(
    const std::wstring& host,
    std::uint16_t port,
    sockaddr_storage& storage,
    int& address_length) {
    const std::string host_utf8 = wide_to_utf8(host);
    const std::string service = std::to_string(port);

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* result = nullptr;
    const int rc = getaddrinfo(host_utf8.c_str(), service.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        std::wcout << L"getaddrinfo(" << host << L":" << port << L") failed: WSA "
                   << rc << L"\n";
        return false;
    }

    std::memset(&storage, 0, sizeof(storage));
    std::memcpy(&storage, result->ai_addr, result->ai_addrlen);
    address_length = static_cast<int>(result->ai_addrlen);
    freeaddrinfo(result);
    return true;
}

std::wstring sockaddr_ip_key(const sockaddr_storage& address) {
    wchar_t buffer[INET6_ADDRSTRLEN] = {};
    if (address.ss_family == AF_INET) {
        const auto& ipv4 = reinterpret_cast<const sockaddr_in&>(address);
        if (InetNtopW(AF_INET, const_cast<IN_ADDR*>(&ipv4.sin_addr), buffer, INET6_ADDRSTRLEN) != nullptr) {
            return buffer;
        }
    }

    if (address.ss_family == AF_INET6) {
        const auto& ipv6 = reinterpret_cast<const sockaddr_in6&>(address);
        if (InetNtopW(AF_INET6, const_cast<IN6_ADDR*>(&ipv6.sin6_addr), buffer, INET6_ADDRSTRLEN) != nullptr) {
            return buffer;
        }
    }

    return L"";
}

struct IpAddressKey {
    ADDRESS_FAMILY family = AF_UNSPEC;
    std::array<std::byte, 16> bytes{};

    auto operator<=>(const IpAddressKey&) const = default;
};

struct IpEndpointKey {
    IpAddressKey address;
    std::uint16_t port = 0;

    auto operator<=>(const IpEndpointKey&) const = default;
};

std::optional<IpAddressKey> binary_ip_key(const sockaddr_storage& address) {
    IpAddressKey key;
    key.family = address.ss_family;
    if (address.ss_family == AF_INET) {
        const auto& ipv4 = reinterpret_cast<const sockaddr_in&>(address);
        std::memcpy(key.bytes.data(), &ipv4.sin_addr, sizeof(ipv4.sin_addr));
        return key;
    }
    if (address.ss_family == AF_INET6) {
        const auto& ipv6 = reinterpret_cast<const sockaddr_in6&>(address);
        std::memcpy(key.bytes.data(), &ipv6.sin6_addr, sizeof(ipv6.sin6_addr));
        return key;
    }
    return std::nullopt;
}

std::optional<IpEndpointKey> binary_endpoint_key(const sockaddr_storage& address) {
    const std::optional<IpAddressKey> key = binary_ip_key(address);
    if (!key) return std::nullopt;
    if (address.ss_family == AF_INET) {
        const auto& ipv4 = reinterpret_cast<const sockaddr_in&>(address);
        return IpEndpointKey{*key, ntohs(ipv4.sin_port)};
    }
    if (address.ss_family == AF_INET6) {
        const auto& ipv6 = reinterpret_cast<const sockaddr_in6&>(address);
        return IpEndpointKey{*key, ntohs(ipv6.sin6_port)};
    }
    return std::nullopt;
}

bool wait_udp_readable(SOCKET socket, int timeout_ms) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket, &read_set);

    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    const int rc = select(0, &read_set, nullptr, nullptr, &timeout);
    if (rc == SOCKET_ERROR) {
        std::wcout << L"select failed: " << winsock_error_text() << L"\n";
        return false;
    }
    return rc > 0 && FD_ISSET(socket, &read_set);
}

FormatCandidate copy_format_candidate(std::wstring name, const WAVEFORMATEX& format) {
    FormatCandidate candidate;
    candidate.name = std::move(name);
    candidate.storage.resize(sizeof(WAVEFORMATEX) + format.cbSize);
    std::memcpy(candidate.storage.data(), &format, candidate.storage.size());
    return candidate;
}

FormatCandidate make_wave_format_candidate(
    std::wstring name,
    WORD tag,
    DWORD sample_rate,
    WORD channels,
    WORD bits_per_sample) {
    FormatCandidate candidate;
    candidate.name = std::move(name);
    candidate.storage.resize(sizeof(WAVEFORMATEX));

    auto& format = *reinterpret_cast<WAVEFORMATEX*>(candidate.storage.data());
    format.wFormatTag = tag;
    format.nChannels = channels;
    format.nSamplesPerSec = sample_rate;
    format.wBitsPerSample = bits_per_sample;
    format.nBlockAlign = static_cast<WORD>(channels * bits_per_sample / 8);
    format.nAvgBytesPerSec = sample_rate * format.nBlockAlign;
    format.cbSize = 0;
    return candidate;
}

FormatCandidate make_extensible_format_candidate(
    std::wstring name,
    DWORD sample_rate,
    WORD channels,
    WORD container_bits,
    WORD valid_bits,
    WORD subformat_tag) {
    FormatCandidate candidate;
    candidate.name = std::move(name);
    candidate.storage.resize(sizeof(WAVEFORMATEXTENSIBLE));

    auto& format = *reinterpret_cast<WAVEFORMATEXTENSIBLE*>(candidate.storage.data());
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = channels;
    format.Format.nSamplesPerSec = sample_rate;
    format.Format.wBitsPerSample = container_bits;
    format.Format.nBlockAlign = static_cast<WORD>(channels * container_bits / 8);
    format.Format.nAvgBytesPerSec = sample_rate * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = valid_bits;
    format.dwChannelMask = default_channel_mask(channels);
    format.SubFormat = wave_subformat_guid(subformat_tag);
    return candidate;
}

std::vector<FormatCandidate> exclusive_format_candidates(const WAVEFORMATEX& mix_format) {
    const DWORD sample_rate = mix_format.nSamplesPerSec;
    const WORD channels = mix_format.nChannels;

    std::vector<FormatCandidate> candidates;
    candidates.push_back(copy_format_candidate(L"mix format", mix_format));
    candidates.push_back(make_wave_format_candidate(L"IEEE_FLOAT 32-bit", WAVE_FORMAT_IEEE_FLOAT, sample_rate, channels, 32));
    candidates.push_back(make_wave_format_candidate(L"PCM 16-bit", WAVE_FORMAT_PCM, sample_rate, channels, 16));
    candidates.push_back(make_extensible_format_candidate(
        L"PCM 24-bit in 32-bit container",
        sample_rate,
        channels,
        32,
        24,
        WAVE_FORMAT_PCM));
    candidates.push_back(make_wave_format_candidate(L"PCM 32-bit", WAVE_FORMAT_PCM, sample_rate, channels, 32));
    return candidates;
}

HRESULT activate_audio_client(IMMDevice& device, ComPtr<IAudioClient>& client) {
    return device.Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
}

void print_iaudioclient3_periods(IMMDevice& device, const WAVEFORMATEX& mix_format) {
    ComPtr<IAudioClient> base_client;
    HRESULT hr = activate_audio_client(device, base_client);
    if (FAILED(hr)) {
        std::wcout << L"  IAudioClient3: Activate failed: " << hresult_text(hr) << L"\n";
        return;
    }

    ComPtr<IAudioClient3> client3;
    hr = base_client.As(&client3);
    if (FAILED(hr)) {
        std::wcout << L"  IAudioClient3: not available: " << hresult_text(hr) << L"\n";
        return;
    }

    UINT32 default_frames = 0;
    UINT32 fundamental_frames = 0;
    UINT32 min_frames = 0;
    UINT32 max_frames = 0;
    hr = client3->GetSharedModeEnginePeriod(
        &mix_format, &default_frames, &fundamental_frames, &min_frames, &max_frames);
    if (FAILED(hr)) {
        std::wcout << L"  shared engine periods: failed: " << hresult_text(hr) << L"\n";
        return;
    }

    const UINT32 sample_rate = mix_format.nSamplesPerSec;
    std::wcout << L"  shared engine periods:\n";
    std::wcout << L"    default:     " << default_frames << L" frames, "
               << frames_to_ms(default_frames, sample_rate) << L" ms\n";
    std::wcout << L"    fundamental: " << fundamental_frames << L" frames, "
               << frames_to_ms(fundamental_frames, sample_rate) << L" ms\n";
    std::wcout << L"    min:         " << min_frames << L" frames, "
               << frames_to_ms(min_frames, sample_rate) << L" ms\n";
    std::wcout << L"    max:         " << max_frames << L" frames, "
               << frames_to_ms(max_frames, sample_rate) << L" ms\n";

    ComPtr<IAudioClient> init_base;
    hr = activate_audio_client(device, init_base);
    if (FAILED(hr)) {
        std::wcout << L"  shared min-period init: Activate failed: " << hresult_text(hr) << L"\n";
        return;
    }

    ComPtr<IAudioClient3> init_client;
    hr = init_base.As(&init_client);
    if (FAILED(hr)) {
        std::wcout << L"  shared min-period init: IAudioClient3 unavailable: " << hresult_text(hr) << L"\n";
        return;
    }

    hr = init_client->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, min_frames, &mix_format, nullptr);
    if (FAILED(hr)) {
        std::wcout << L"  shared min-period init: failed: " << hresult_text(hr) << L"\n";
        return;
    }

    Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (event.value != nullptr) {
        const HRESULT event_hr = init_client->SetEventHandle(event.value);
        if (FAILED(event_hr)) {
            std::wcout << L"  shared min-period event handle: failed: " << hresult_text(event_hr) << L"\n";
        }
    }

    UINT32 buffer_frames = 0;
    hr = init_client->GetBufferSize(&buffer_frames);
    if (SUCCEEDED(hr)) {
        std::wcout << L"  shared min-period init: ok, buffer=" << buffer_frames << L" frames, "
                   << frames_to_ms(buffer_frames, sample_rate) << L" ms\n";
    } else {
        std::wcout << L"  shared min-period buffer size: failed: " << hresult_text(hr) << L"\n";
    }

    REFERENCE_TIME latency = 0;
    hr = init_client->GetStreamLatency(&latency);
    if (SUCCEEDED(hr)) {
        std::wcout << L"  shared min-period reported latency: " << reference_time_to_ms(latency) << L" ms\n";
    }
}

void print_basic_periods(IAudioClient& client) {
    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME min_period = 0;
    const HRESULT hr = client.GetDevicePeriod(&default_period, &min_period);
    if (FAILED(hr)) {
        std::wcout << L"  device periods: failed: " << hresult_text(hr) << L"\n";
        return;
    }

    std::wcout << L"  device periods:\n";
    std::wcout << L"    default: " << reference_time_to_ms(default_period) << L" ms\n";
    std::wcout << L"    min:     " << reference_time_to_ms(min_period) << L" ms\n";
}

void probe_shared_event_init(IMMDevice& device, const WAVEFORMATEX& mix_format) {
    ComPtr<IAudioClient> client;
    HRESULT hr = activate_audio_client(device, client);
    if (FAILED(hr)) {
        std::wcout << L"  shared event init: Activate failed: " << hresult_text(hr) << L"\n";
        return;
    }

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, &mix_format, nullptr);
    if (FAILED(hr)) {
        std::wcout << L"  shared event init: failed: " << hresult_text(hr) << L"\n";
        return;
    }

    Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (event.value != nullptr) {
        const HRESULT event_hr = client->SetEventHandle(event.value);
        if (FAILED(event_hr)) {
            std::wcout << L"  shared event handle: failed: " << hresult_text(event_hr) << L"\n";
        }
    }

    UINT32 buffer_frames = 0;
    hr = client->GetBufferSize(&buffer_frames);
    if (FAILED(hr)) {
        std::wcout << L"  shared event buffer size: failed: " << hresult_text(hr) << L"\n";
        return;
    }

    std::wcout << L"  shared event init: ok, buffer=" << buffer_frames << L" frames, "
               << frames_to_ms(buffer_frames, mix_format.nSamplesPerSec) << L" ms\n";

    REFERENCE_TIME latency = 0;
    hr = client->GetStreamLatency(&latency);
    if (SUCCEEDED(hr)) {
        std::wcout << L"  shared event reported latency: " << reference_time_to_ms(latency) << L" ms\n";
    }
}

std::vector<UINT32> exclusive_candidate_frames(UINT32 sample_rate) {
    const std::vector<double> candidate_ms = {2.5, 5.0, 10.0, 20.0};
    std::vector<UINT32> frames;
    frames.reserve(candidate_ms.size());

    for (const double milliseconds : candidate_ms) {
        const auto candidate = static_cast<UINT32>(std::llround(sample_rate * milliseconds / 1000.0));
        if (candidate > 0) {
            frames.push_back(candidate);
        }
    }

    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

void probe_exclusive_event_init(IMMDevice& device, const WAVEFORMATEX& mix_format) {
    std::wcout << L"  exclusive format candidates:\n";

    bool any_supported_format = false;
    for (const FormatCandidate& candidate : exclusive_format_candidates(mix_format)) {
        ComPtr<IAudioClient> support_client;
        HRESULT hr = activate_audio_client(device, support_client);
        if (FAILED(hr)) {
            std::wcout << L"    " << candidate.name << L": Activate failed: " << hresult_text(hr) << L"\n";
            continue;
        }

        hr = support_client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &candidate.format(), nullptr);
        if (hr != S_OK) {
            std::wcout << L"    " << candidate.name << L": no";
            if (FAILED(hr)) {
                std::wcout << L": " << hresult_text(hr);
            } else {
                std::wcout << L": " << hresult_hex(hr);
            }
            std::wcout << L"\n";
            continue;
        }

        any_supported_format = true;
        std::wcout << L"    " << candidate.name << L": yes\n";

        for (const UINT32 frames : exclusive_candidate_frames(candidate.format().nSamplesPerSec)) {
            ComPtr<IAudioClient> client;
            hr = activate_audio_client(device, client);
            if (FAILED(hr)) {
                std::wcout << L"      " << frames << L" frames: Activate failed: " << hresult_text(hr) << L"\n";
                continue;
            }

            const REFERENCE_TIME period = frames_to_reference_time(frames, candidate.format().nSamplesPerSec);
            hr = client->Initialize(
                AUDCLNT_SHAREMODE_EXCLUSIVE,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                period,
                period,
                &candidate.format(),
                nullptr);

            if (FAILED(hr)) {
                std::wcout << L"      " << frames << L" frames, "
                           << frames_to_ms(frames, candidate.format().nSamplesPerSec)
                           << L" ms: failed: " << hresult_text(hr) << L"\n";
                continue;
            }

            Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            if (event.value != nullptr) {
                const HRESULT event_hr = client->SetEventHandle(event.value);
                if (FAILED(event_hr)) {
                    std::wcout << L"      " << frames << L" frames: SetEventHandle failed: "
                               << hresult_text(event_hr) << L"\n";
                }
            }

            UINT32 actual_buffer_frames = 0;
            hr = client->GetBufferSize(&actual_buffer_frames);
            if (SUCCEEDED(hr)) {
                std::wcout << L"      " << frames << L" frames, "
                           << frames_to_ms(frames, candidate.format().nSamplesPerSec)
                           << L" ms: ok, buffer=" << actual_buffer_frames << L" frames, "
                           << frames_to_ms(actual_buffer_frames, candidate.format().nSamplesPerSec) << L" ms\n";
            } else {
                std::wcout << L"      " << frames << L" frames: ok, GetBufferSize failed: "
                           << hresult_text(hr) << L"\n";
            }
        }
    }

    if (!any_supported_format) {
        std::wcout << L"    no tested exclusive format was accepted\n";
    }
}

double qpc_elapsed_ms(const LARGE_INTEGER& start, const LARGE_INTEGER& end, const LARGE_INTEGER& frequency) {
    return static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 /
           static_cast<double>(frequency.QuadPart);
}

void print_capture_progress(double elapsed_ms, const CaptureStats& stats) {
    std::wcout << L"  t=" << elapsed_ms / 1000.0 << L"s"
               << L", events=" << stats.wait_events
               << L", packets=" << stats.packets
               << L", frames=" << stats.frames
               << L", avg_event=" << stats.callback_intervals.average_ms() << L" ms"
               << L", max_event=" << stats.callback_intervals.max_ms << L" ms"
               << L", discontinuities=" << stats.discontinuity_packets
               << L", peak=" << stats.peak_abs_sample << L"\n";
}

HRESULT initialize_shared_capture_client(
    IMMDevice& device,
    const WAVEFORMATEX& mix_format,
    ComPtr<IAudioClient>& client,
    bool& used_audio_client3,
    UINT32& requested_period_frames) {
    used_audio_client3 = false;
    requested_period_frames = 0;

    ComPtr<IAudioClient> base_client;
    HRESULT hr = activate_audio_client(device, base_client);
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IAudioClient3> client3;
    hr = base_client.As(&client3);
    if (SUCCEEDED(hr)) {
        UINT32 default_frames = 0;
        UINT32 fundamental_frames = 0;
        UINT32 min_frames = 0;
        UINT32 max_frames = 0;
        hr = client3->GetSharedModeEnginePeriod(
            &mix_format, &default_frames, &fundamental_frames, &min_frames, &max_frames);
        if (SUCCEEDED(hr)) {
            hr = client3->InitializeSharedAudioStream(
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                min_frames,
                &mix_format,
                nullptr);
            if (SUCCEEDED(hr)) {
                client = base_client;
                used_audio_client3 = true;
                requested_period_frames = min_frames;
                return S_OK;
            }

            std::wcout << L"  IAudioClient3 min-period initialize failed, falling back: "
                       << hresult_text(hr) << L"\n";
        } else {
            std::wcout << L"  IAudioClient3 periods unavailable, falling back: "
                       << hresult_text(hr) << L"\n";
        }
    }

    ComPtr<IAudioClient> fallback_client;
    hr = activate_audio_client(device, fallback_client);
    if (FAILED(hr)) {
        return hr;
    }

    hr = fallback_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        0,
        0,
        &mix_format,
        nullptr);
    if (FAILED(hr)) {
        return hr;
    }

    client = fallback_client;
    return S_OK;
}

int run_capture_timing_test(
    IMMDeviceEnumerator& enumerator,
    int seconds,
    ERole capture_role,
    const std::optional<std::wstring>& capture_device_selector) {
    std::wcout << L"== Shared capture timing test ==\n";
    std::wcout << L"Duration: " << seconds << L" seconds\n";
    std::wcout << L"Capture role: " << role_name(capture_role) << L"\n";

    ComPtr<IMMDevice> device;
    if (!select_audio_endpoint(enumerator, eCapture, capture_role, capture_device_selector, L"capture", device)) {
        return 1;
    }

    std::wcout << L"Device: " << get_friendly_name(*device.Get()) << L"\n";
    std::wcout << L"Id: " << get_device_id(*device.Get()) << L"\n";

    ComPtr<IAudioClient> format_client;
    HRESULT hr = activate_audio_client(*device.Get(), format_client);
    if (FAILED(hr)) {
        std::wcout << L"Activate(IAudioClient) failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    WAVEFORMATEX* raw_mix_format = nullptr;
    hr = format_client->GetMixFormat(&raw_mix_format);
    if (FAILED(hr)) {
        std::wcout << L"GetMixFormat failed: " << hresult_text(hr) << L"\n";
        return 1;
    }
    WaveFormatPtr mix_format(raw_mix_format);
    print_wave_format(*mix_format, L"  ");

    print_basic_periods(*format_client.Get());
    print_iaudioclient3_periods(*device.Get(), *mix_format);

    ComPtr<IAudioClient> client;
    bool used_audio_client3 = false;
    UINT32 requested_period_frames = 0;
    hr = initialize_shared_capture_client(
        *device.Get(),
        *mix_format,
        client,
        used_audio_client3,
        requested_period_frames);
    if (FAILED(hr)) {
        std::wcout << L"Initialize shared capture failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (event.value == nullptr) {
        std::wcout << L"CreateEvent failed: " << GetLastError() << L"\n";
        return 1;
    }

    hr = client->SetEventHandle(event.value);
    if (FAILED(hr)) {
        std::wcout << L"SetEventHandle failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    UINT32 buffer_frames = 0;
    hr = client->GetBufferSize(&buffer_frames);
    if (FAILED(hr)) {
        std::wcout << L"GetBufferSize failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    REFERENCE_TIME latency = 0;
    hr = client->GetStreamLatency(&latency);
    if (FAILED(hr)) {
        latency = 0;
    }

    std::wcout << L"Initialized: "
               << (used_audio_client3 ? L"IAudioClient3 min-period shared stream" : L"IAudioClient shared stream")
               << L"\n";
    if (requested_period_frames != 0) {
        std::wcout << L"Requested period: " << requested_period_frames << L" frames, "
                   << frames_to_ms(requested_period_frames, mix_format->nSamplesPerSec) << L" ms\n";
    }
    std::wcout << L"Actual buffer: " << buffer_frames << L" frames, "
               << frames_to_ms(buffer_frames, mix_format->nSamplesPerSec) << L" ms\n";
    std::wcout << L"Reported stream latency: " << reference_time_to_ms(latency) << L" ms\n";

    ComPtr<IAudioCaptureClient> capture_client;
    hr = client->GetService(IID_PPV_ARGS(&capture_client));
    if (FAILED(hr)) {
        std::wcout << L"GetService(IAudioCaptureClient) failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    MmcssTask mmcss(L"Audio");
    if (mmcss.value == nullptr) {
        std::wcout << L"MMCSS Audio priority: unavailable, GetLastError=" << GetLastError() << L"\n";
    } else {
        std::wcout << L"MMCSS Audio priority: enabled\n";
    }

    LARGE_INTEGER frequency{};
    LARGE_INTEGER start_time{};
    LARGE_INTEGER previous_event_time{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start_time);
    bool have_previous_event = false;

    hr = client->Start();
    if (FAILED(hr)) {
        std::wcout << L"Start failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    CaptureStats stats;
    int last_report_second = 0;
    const double duration_ms = static_cast<double>(seconds) * 1000.0;

    while (true) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed_ms = qpc_elapsed_ms(start_time, now, frequency);
        if (elapsed_ms >= duration_ms) {
            break;
        }

        const DWORD wait_ms = static_cast<DWORD>(std::min(1000.0, duration_ms - elapsed_ms + 50.0));
        const DWORD wait_result = WaitForSingleObject(event.value, wait_ms);
        if (wait_result == WAIT_TIMEOUT) {
            ++stats.wait_timeouts;
        } else if (wait_result == WAIT_OBJECT_0) {
            ++stats.wait_events;

            LARGE_INTEGER event_time{};
            QueryPerformanceCounter(&event_time);
            if (have_previous_event) {
                stats.callback_intervals.add(qpc_elapsed_ms(previous_event_time, event_time, frequency));
            }
            previous_event_time = event_time;
            have_previous_event = true;

            UINT32 next_packet_frames = 0;
            hr = capture_client->GetNextPacketSize(&next_packet_frames);
            if (FAILED(hr)) {
                std::wcout << L"GetNextPacketSize failed: " << hresult_text(hr) << L"\n";
                client->Stop();
                return 1;
            }

            while (next_packet_frames > 0) {
                BYTE* data = nullptr;
                UINT32 packet_frames = 0;
                DWORD flags = 0;
                UINT64 device_position = 0;
                UINT64 qpc_position = 0;
                hr = capture_client->GetBuffer(
                    &data,
                    &packet_frames,
                    &flags,
                    &device_position,
                    &qpc_position);
                if (FAILED(hr)) {
                    std::wcout << L"GetBuffer failed: " << hresult_text(hr) << L"\n";
                    client->Stop();
                    return 1;
                }

                stats.add_packet(packet_frames);
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
                    ++stats.silent_packets;
                    stats.silent_frames += packet_frames;
                } else {
                    ++stats.data_packets;
                    stats.data_frames += packet_frames;
                    stats.peak_abs_sample = std::max(
                        stats.peak_abs_sample,
                        packet_peak_abs_sample(data, packet_frames, *mix_format));
                }
                if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                    ++stats.discontinuity_packets;
                }
                if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0) {
                    ++stats.timestamp_error_packets;
                }

                hr = capture_client->ReleaseBuffer(packet_frames);
                if (FAILED(hr)) {
                    std::wcout << L"ReleaseBuffer failed: " << hresult_text(hr) << L"\n";
                    client->Stop();
                    return 1;
                }

                hr = capture_client->GetNextPacketSize(&next_packet_frames);
                if (FAILED(hr)) {
                    std::wcout << L"GetNextPacketSize failed: " << hresult_text(hr) << L"\n";
                    client->Stop();
                    return 1;
                }
            }
        } else if (wait_result == WAIT_FAILED) {
            std::wcout << L"WaitForSingleObject failed: " << GetLastError() << L"\n";
            client->Stop();
            return 1;
        }

        QueryPerformanceCounter(&now);
        const double after_wait_elapsed_ms = qpc_elapsed_ms(start_time, now, frequency);
        const int current_second = static_cast<int>(after_wait_elapsed_ms / 1000.0);
        if (current_second > last_report_second && current_second < seconds) {
            last_report_second = current_second;
            print_capture_progress(after_wait_elapsed_ms, stats);
        }
    }

    hr = client->Stop();
    if (FAILED(hr)) {
        std::wcout << L"Stop failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    LARGE_INTEGER finish_time{};
    QueryPerformanceCounter(&finish_time);
    const double actual_duration_ms = qpc_elapsed_ms(start_time, finish_time, frequency);
    const double captured_ms = frames_to_ms(
        static_cast<UINT32>(std::min<std::uint64_t>(stats.frames, std::numeric_limits<UINT32>::max())),
        mix_format->nSamplesPerSec);

    std::wcout << L"\nCapture summary:\n";
    std::wcout << L"  actual duration: " << actual_duration_ms << L" ms\n";
    std::wcout << L"  wait events:     " << stats.wait_events << L"\n";
    std::wcout << L"  wait timeouts:   " << stats.wait_timeouts << L"\n";
    std::wcout << L"  packets:         " << stats.packets << L"\n";
    std::wcout << L"  frames:          " << stats.frames << L" (" << captured_ms << L" ms of audio)\n";
    std::wcout << L"  packet frames:   min=" << stats.printable_min_packet_frames()
               << L", max=" << stats.max_packet_frames << L"\n";
    std::wcout << L"  event interval:  min=" << stats.callback_intervals.printable_min_ms()
               << L" ms, avg=" << stats.callback_intervals.average_ms()
               << L" ms, max=" << stats.callback_intervals.max_ms << L" ms"
               << L", samples=" << stats.callback_intervals.count << L"\n";
    std::wcout << L"  data packets:    " << stats.data_packets << L", frames=" << stats.data_frames << L"\n";
    std::wcout << L"  silent packets:  " << stats.silent_packets << L", frames=" << stats.silent_frames << L"\n";
    std::wcout << L"  discontinuities: " << stats.discontinuity_packets << L"\n";
    std::wcout << L"  timestamp errs:  " << stats.timestamp_error_packets << L"\n";
    std::wcout << L"  peak abs sample: " << stats.peak_abs_sample << L"\n";
    return 0;
}

int run_tone_render_test(
    IMMDeviceEnumerator& enumerator,
    int seconds,
    double output_gain,
    double frequency_hz,
    ERole render_role,
    const std::optional<std::wstring>& render_device_selector) {
    std::wcout << L"== Shared render tone test ==\n";
    std::wcout << L"Duration: " << seconds << L" seconds\n";
    std::wcout << L"Frequency: " << frequency_hz << L" Hz\n";
    std::wcout << L"Output gain: " << output_gain << L"\n";
    std::wcout << L"Render role: " << role_name(render_role) << L"\n";

    ComPtr<IMMDevice> device;
    if (!select_audio_endpoint(enumerator, eRender, render_role, render_device_selector, L"render", device)) {
        return 1;
    }

    std::wcout << L"Render device: " << get_friendly_name(*device.Get()) << L"\n";

    ComPtr<IAudioClient> format_client;
    HRESULT hr = activate_audio_client(*device.Get(), format_client);
    if (FAILED(hr)) {
        std::wcout << L"Activate render IAudioClient failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    WAVEFORMATEX* raw_mix_format = nullptr;
    hr = format_client->GetMixFormat(&raw_mix_format);
    if (FAILED(hr)) {
        std::wcout << L"Render GetMixFormat failed: " << hresult_text(hr) << L"\n";
        return 1;
    }
    WaveFormatPtr mix_format(raw_mix_format);
    print_wave_format(*mix_format, L"  ");

    if (sample_kind(*mix_format) == SampleKind::unknown) {
        std::wcout << L"Unsupported render mix format for tone test\n";
        return 1;
    }

    ComPtr<IAudioClient> client;
    bool used_audio_client3 = false;
    UINT32 requested_period_frames = 0;
    hr = initialize_shared_capture_client(
        *device.Get(),
        *mix_format,
        client,
        used_audio_client3,
        requested_period_frames);
    if (FAILED(hr)) {
        std::wcout << L"Initialize shared render failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (event.value == nullptr) {
        std::wcout << L"CreateEvent failed: " << GetLastError() << L"\n";
        return 1;
    }

    hr = client->SetEventHandle(event.value);
    if (FAILED(hr)) {
        std::wcout << L"Render SetEventHandle failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    UINT32 buffer_frames = 0;
    hr = client->GetBufferSize(&buffer_frames);
    if (FAILED(hr)) {
        std::wcout << L"Render GetBufferSize failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    ComPtr<IAudioRenderClient> render_client;
    hr = client->GetService(IID_PPV_ARGS(&render_client));
    if (FAILED(hr)) {
        std::wcout << L"GetService(IAudioRenderClient) failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    std::wcout << L"Initialized render: "
               << (used_audio_client3 ? L"IAudioClient3 min-period shared stream" : L"IAudioClient shared stream")
               << L"\n";
    if (requested_period_frames != 0) {
        std::wcout << L"Requested render period: " << requested_period_frames << L" frames, "
                   << frames_to_ms(requested_period_frames, mix_format->nSamplesPerSec) << L" ms\n";
    }
    std::wcout << L"Render buffer: " << buffer_frames << L" frames, "
               << frames_to_ms(buffer_frames, mix_format->nSamplesPerSec) << L" ms\n";

    const UINT32 prefill_frames = std::min(
        buffer_frames,
        requested_period_frames != 0 ? requested_period_frames : std::max<UINT32>(1, mix_format->nSamplesPerSec / 100));
    BYTE* prefill = nullptr;
    hr = render_client->GetBuffer(prefill_frames, &prefill);
    if (FAILED(hr)) {
        std::wcout << L"Render prefill GetBuffer failed: " << hresult_text(hr) << L"\n";
        return 1;
    }
    std::memset(prefill, 0, static_cast<size_t>(prefill_frames) * mix_format->nBlockAlign);
    hr = render_client->ReleaseBuffer(prefill_frames, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) {
        std::wcout << L"Render prefill ReleaseBuffer failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    MmcssTask mmcss(L"Audio");
    if (mmcss.value == nullptr) {
        std::wcout << L"MMCSS Audio priority: unavailable, GetLastError=" << GetLastError() << L"\n";
    } else {
        std::wcout << L"MMCSS Audio priority: enabled\n";
    }

    LARGE_INTEGER frequency{};
    LARGE_INTEGER start_time{};
    LARGE_INTEGER previous_render_event{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start_time);
    bool have_previous_render_event = false;
    int last_report_second = 0;
    const double duration_ms = static_cast<double>(seconds) * 1000.0;

    TimingStats render_intervals;
    std::uint64_t render_events = 0;
    std::uint64_t render_wait_timeouts = 0;
    std::uint64_t render_wait_failures = 0;
    std::uint64_t rendered_frames = 0;
    std::uint64_t render_padding_min = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t render_padding_max = 0;
    double tone_phase = 0.0;
    double peak_abs_sample = 0.0;

    hr = client->Start();
    if (FAILED(hr)) {
        std::wcout << L"Render Start failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    while (true) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed_ms = qpc_elapsed_ms(start_time, now, frequency);
        if (elapsed_ms >= duration_ms) {
            break;
        }

        const DWORD wait_ms = static_cast<DWORD>(std::min(1000.0, duration_ms - elapsed_ms + 50.0));
        const DWORD wait_result = WaitForSingleObject(event.value, wait_ms);
        if (wait_result == WAIT_TIMEOUT) {
            ++render_wait_timeouts;
        } else if (wait_result == WAIT_OBJECT_0) {
            ++render_events;
            LARGE_INTEGER render_event_time{};
            QueryPerformanceCounter(&render_event_time);
            if (have_previous_render_event) {
                render_intervals.add(qpc_elapsed_ms(previous_render_event, render_event_time, frequency));
            }
            previous_render_event = render_event_time;
            have_previous_render_event = true;

            UINT32 padding = 0;
            hr = client->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                std::wcout << L"Render GetCurrentPadding failed: " << hresult_text(hr) << L"\n";
                client->Stop();
                return 1;
            }
            render_padding_min = std::min<std::uint64_t>(render_padding_min, padding);
            render_padding_max = std::max<std::uint64_t>(render_padding_max, padding);

            const UINT32 available_frames = buffer_frames > padding ? buffer_frames - padding : 0;
            if (available_frames > 0) {
                BYTE* render_data = nullptr;
                hr = render_client->GetBuffer(available_frames, &render_data);
                if (FAILED(hr)) {
                    std::wcout << L"Render GetBuffer failed: " << hresult_text(hr) << L"\n";
                    client->Stop();
                    return 1;
                }

                if (!fill_render_buffer_with_tone(
                        render_data,
                        available_frames,
                        *mix_format,
                        frequency_hz,
                        output_gain,
                        tone_phase,
                        peak_abs_sample)) {
                    render_client->ReleaseBuffer(available_frames, AUDCLNT_BUFFERFLAGS_SILENT);
                    client->Stop();
                    std::wcout << L"Failed to fill render buffer with tone\n";
                    return 1;
                }

                hr = render_client->ReleaseBuffer(available_frames, 0);
                if (FAILED(hr)) {
                    std::wcout << L"Render ReleaseBuffer failed: " << hresult_text(hr) << L"\n";
                    client->Stop();
                    return 1;
                }
                rendered_frames += available_frames;
            }
        } else if (wait_result == WAIT_FAILED) {
            ++render_wait_failures;
            std::wcout << L"Render WaitForSingleObject failed: " << GetLastError() << L"\n";
            client->Stop();
            return 1;
        }

        QueryPerformanceCounter(&now);
        const double after_wait_elapsed_ms = qpc_elapsed_ms(start_time, now, frequency);
        const int current_second = static_cast<int>(after_wait_elapsed_ms / 1000.0);
        if (current_second > last_report_second && current_second < seconds) {
            last_report_second = current_second;
            std::wcout << L"  tone t=" << after_wait_elapsed_ms / 1000.0
                       << L"s, rendered=" << rendered_frames
                       << L", generated_peak=" << peak_abs_sample << L"\n";
        }
    }

    hr = client->Stop();
    if (FAILED(hr)) {
        std::wcout << L"Render Stop failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    std::wcout << L"\nTone render summary:\n";
    std::wcout << L"  render events:   " << render_events << L"\n";
    std::wcout << L"  rendered frames: " << rendered_frames << L" ("
               << frames_to_ms(
                      static_cast<UINT32>(std::min<std::uint64_t>(
                          rendered_frames,
                          std::numeric_limits<UINT32>::max())),
                      mix_format->nSamplesPerSec)
               << L" ms)\n";
    std::wcout << L"  generated peak:  " << peak_abs_sample << L"\n";
    std::wcout << L"  render waits:    timeouts=" << render_wait_timeouts
               << L", failures=" << render_wait_failures << L"\n";
    std::wcout << L"  render interval: min=" << render_intervals.printable_min_ms()
               << L" ms, avg=" << render_intervals.average_ms()
               << L" ms, max=" << render_intervals.max_ms
               << L" ms, samples=" << render_intervals.count << L"\n";
    if (render_padding_min != std::numeric_limits<std::uint64_t>::max()) {
        std::wcout << L"  render padding:  min=" << render_padding_min
                   << L", max=" << render_padding_max << L" frames\n";
    }

    return 0;
}

void update_udp_sequence_stats(UdpReceiveStats& stats, std::uint64_t sequence) {
    if (!stats.have_sequence) {
        stats.expected_sequence = sequence + 1;
        stats.have_sequence = true;
        return;
    }

    if (sequence == stats.expected_sequence) {
        ++stats.expected_sequence;
    } else if (sequence > stats.expected_sequence) {
        const std::uint64_t gap = sequence - stats.expected_sequence;
        if (gap > kSequenceRestartPacketGap) {
            ++stats.sequence_restarts;
        } else {
            stats.sequence_gaps += gap;
        }
        stats.expected_sequence = sequence + 1;
    } else {
        const std::uint64_t rewind = stats.expected_sequence - sequence;
        if (rewind > kSequenceRestartPacketGap) {
            ++stats.sequence_restarts;
            stats.expected_sequence = sequence + 1;
        } else {
            ++stats.out_of_order_or_duplicate;
        }
    }
}

void print_udp_receive_progress(double elapsed_ms, const UdpReceiveStats& stats) {
    std::wcout << L"  recv t=" << elapsed_ms / 1000.0 << L"s"
               << L", packets=" << stats.valid_packets
               << L", gaps=" << stats.sequence_gaps
               << L", interarrival_avg=" << stats.interarrival.average_ms() << L" ms"
               << L", interarrival_max=" << stats.interarrival.max_ms << L" ms";
    if (stats.sender_to_receiver_latency.count > 0) {
        std::wcout << L", qpc_latency_avg=" << stats.sender_to_receiver_latency.average_ms() << L" ms"
                   << L", qpc_latency_max=" << stats.sender_to_receiver_latency.max_ms << L" ms";
    }
    std::wcout << L"\n";
}

void print_udp_receive_summary(const UdpReceiveStats& stats) {
    std::wcout << L"\nUDP receive summary:\n";
    std::wcout << L"  datagrams:       " << stats.datagrams << L"\n";
    std::wcout << L"  valid packets:   " << stats.valid_packets << L"\n";
    std::wcout << L"  invalid packets: " << stats.invalid_packets << L"\n";
    std::wcout << L"  bytes received:  " << stats.bytes_received << L"\n";
    std::wcout << L"  payload bytes:   " << stats.payload_bytes_received << L"\n";
    std::wcout << L"  frames received: " << stats.frames_received << L"\n";
    std::wcout << L"  resampled:       " << stats.resampled_packets << L" packets";
    if (stats.resampled_packets > 0) {
        std::wcout << L", source_rate=" << stats.last_source_sample_rate
                   << L" Hz, input_frames=" << stats.resampled_input_frames
                   << L", output_frames=" << stats.resampled_output_frames;
    }
    std::wcout << L"\n";
    std::wcout << L"  sequence gaps:   " << stats.sequence_gaps << L"\n";
    std::wcout << L"  old/duplicate:   " << stats.out_of_order_or_duplicate << L"\n";
    std::wcout << L"  seq restarts:    " << stats.sequence_restarts << L"\n";
    std::wcout << L"  interarrival:    min=" << stats.interarrival.printable_min_ms()
               << L" ms, avg=" << stats.interarrival.average_ms()
               << L" ms, max=" << stats.interarrival.max_ms
               << L" ms, samples=" << stats.interarrival.count << L"\n";
    if (stats.sender_to_receiver_latency.count > 0) {
        std::wcout << L"  qpc latency:     min=" << stats.sender_to_receiver_latency.printable_min_ms()
                   << L" ms, avg=" << stats.sender_to_receiver_latency.average_ms()
                   << L" ms, max=" << stats.sender_to_receiver_latency.max_ms
                   << L" ms, samples=" << stats.sender_to_receiver_latency.count << L"\n";
    } else {
        std::wcout << L"  qpc latency:     unavailable (expected on different machines)\n";
    }
}

int run_udp_receive_test(std::uint16_t port, int seconds, bool loopback_only, bool quiet_progress) {
    WinsockRuntime winsock;
    if (winsock.result != 0) {
        std::wcout << L"WSAStartup failed: " << winsock_error_text(winsock.result) << L"\n";
        return 1;
    }

    SocketHandle socket_handle = create_udp_socket();
    if (socket_handle.value == INVALID_SOCKET) {
        std::wcout << L"socket(AF_INET, SOCK_DGRAM) failed: " << winsock_error_text() << L"\n";
        return 1;
    }

    if (!bind_udp_socket(socket_handle, port, loopback_only)) {
        return 1;
    }

    std::wcout << L"== UDP receive telemetry ==\n";
    std::wcout << L"Listening on " << (loopback_only ? L"127.0.0.1" : L"0.0.0.0")
               << L":" << port << L" for " << seconds << L" seconds\n";

    LARGE_INTEGER frequency{};
    LARGE_INTEGER start_time{};
    LARGE_INTEGER previous_receive_time{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start_time);
    bool have_previous_receive = false;
    int last_report_second = 0;

    UdpReceiveStats stats;
    std::vector<char> buffer(65536);
    const double duration_ms = static_cast<double>(seconds) * 1000.0;

    while (true) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed_ms = qpc_elapsed_ms(start_time, now, frequency);
        if (elapsed_ms >= duration_ms) {
            break;
        }

        const int wait_ms = static_cast<int>(std::min(100.0, duration_ms - elapsed_ms));
        if (!wait_udp_readable(socket_handle.value, wait_ms)) {
            continue;
        }

        sockaddr_storage from{};
        int from_length = sizeof(from);
        const int bytes = recvfrom(
            socket_handle.value,
            buffer.data(),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &from_length);
        if (bytes == SOCKET_ERROR) {
            std::wcout << L"recvfrom failed: " << winsock_error_text() << L"\n";
            return 1;
        }

        ++stats.datagrams;
        stats.bytes_received += static_cast<std::uint64_t>(bytes);

        LARGE_INTEGER receive_time{};
        QueryPerformanceCounter(&receive_time);
        if (have_previous_receive) {
            stats.interarrival.add(qpc_elapsed_ms(previous_receive_time, receive_time, frequency));
        }
        previous_receive_time = receive_time;
        have_previous_receive = true;

        if (bytes < static_cast<int>(sizeof(UdpAudioPacketHeader))) {
            ++stats.invalid_packets;
            continue;
        }

        UdpAudioPacketHeader header{};
        std::memcpy(&header, buffer.data(), sizeof(header));
        const bool header_ok =
            header.magic == kUdpAudioMagic &&
            header.version == kUdpAudioVersion &&
            header.header_size >= sizeof(UdpAudioPacketHeader) &&
            header.header_size <= static_cast<std::uint16_t>(bytes) &&
            header.payload_bytes <= static_cast<std::uint32_t>(bytes - header.header_size);

        if (!header_ok) {
            ++stats.invalid_packets;
            continue;
        }

        ++stats.valid_packets;
        stats.payload_bytes_received += header.payload_bytes;
        stats.frames_received += header.frames;
        update_udp_sequence_stats(stats, header.sequence);

        if (header.send_qpc_frequency == static_cast<std::uint64_t>(frequency.QuadPart) &&
            header.send_qpc != 0 &&
            static_cast<std::uint64_t>(receive_time.QuadPart) >= header.send_qpc) {
            const std::uint64_t delta = static_cast<std::uint64_t>(receive_time.QuadPart) - header.send_qpc;
            const std::uint64_t max_reasonable_delta = static_cast<std::uint64_t>(frequency.QuadPart) * 60;
            if (delta <= max_reasonable_delta) {
                stats.sender_to_receiver_latency.add(
                    static_cast<double>(delta) * 1000.0 / static_cast<double>(frequency.QuadPart));
            }
        }

        if (!quiet_progress) {
            QueryPerformanceCounter(&now);
            const double after_receive_elapsed_ms = qpc_elapsed_ms(start_time, now, frequency);
            const int current_second = static_cast<int>(after_receive_elapsed_ms / 1000.0);
            if (current_second > last_report_second && current_second < seconds) {
                last_report_second = current_second;
                print_udp_receive_progress(after_receive_elapsed_ms, stats);
            }
        }
    }

    print_udp_receive_summary(stats);
    return 0;
}

void print_udp_send_progress(double elapsed_ms, const UdpSendStats& stats) {
    std::wcout << L"  send t=" << elapsed_ms / 1000.0 << L"s"
               << L", packets=" << stats.packets_sent
               << L", suppressed=" << stats.packets_suppressed
               << L", send_errors=" << stats.send_errors
               << L", frames=" << stats.frames_sent
               << L", avg_send_interval=" << stats.send_intervals.average_ms() << L" ms"
               << L", max_send_interval=" << stats.send_intervals.max_ms << L" ms"
               << L", capture_peak=" << stats.capture.peak_abs_sample << L"\n";
}

void print_udp_send_summary(const UdpSendStats& stats) {
    std::wcout << L"\nUDP send summary:\n";
    std::wcout << L"  packets sent:    " << stats.packets_sent << L"\n";
    std::wcout << L"  packets skipped: " << stats.packets_suppressed << L"\n";
    std::wcout << L"  send errors:     " << stats.send_errors << L"\n";
    std::wcout << L"  bytes sent:      " << stats.bytes_sent << L"\n";
    std::wcout << L"  payload bytes:   " << stats.payload_bytes_sent << L"\n";
    std::wcout << L"  frames sent:     " << stats.frames_sent << L"\n";
    std::wcout << L"  frames skipped:  " << stats.frames_suppressed << L"\n";
    std::wcout << L"  send interval:   min=" << stats.send_intervals.printable_min_ms()
               << L" ms, avg=" << stats.send_intervals.average_ms()
               << L" ms, max=" << stats.send_intervals.max_ms
               << L" ms, samples=" << stats.send_intervals.count << L"\n";
    std::wcout << L"  capture packets: " << stats.capture.packets
               << L", discontinuities=" << stats.capture.discontinuity_packets
               << L", timestamp_errs=" << stats.capture.timestamp_error_packets
               << L", peak=" << stats.capture.peak_abs_sample << L"\n";
}

int run_udp_send_test(
    IMMDeviceEnumerator& enumerator,
    const std::wstring& host,
    std::uint16_t port,
    int seconds,
    double input_gain,
    ERole capture_role,
    const std::optional<std::wstring>& capture_device_selector,
    const SelfDuckingSettings& self_ducking,
    SelfDuckingState* self_ducking_state,
    const std::atomic_bool* stop_signal = nullptr) {
    WinsockRuntime winsock;
    if (winsock.result != 0) {
        std::wcout << L"WSAStartup failed: " << winsock_error_text(winsock.result) << L"\n";
        return 1;
    }

    SocketHandle socket_handle = create_udp_socket();
    if (socket_handle.value == INVALID_SOCKET) {
        std::wcout << L"socket(AF_INET, SOCK_DGRAM) failed: " << winsock_error_text() << L"\n";
        return 1;
    }

    sockaddr_storage target{};
    int target_length = 0;
    if (!resolve_udp_target(host, port, target, target_length)) {
        return 1;
    }

    std::wcout << L"== UDP capture sender ==\n";
    std::wcout << L"Target: " << host << L":" << port << L"\n";
    std::wcout << L"Duration: " << duration_text(seconds) << L"\n";
    std::wcout << L"Capture role: " << role_name(capture_role) << L"\n";

    ComPtr<IMMDevice> device;
    if (!select_audio_endpoint(enumerator, eCapture, capture_role, capture_device_selector, L"capture", device)) {
        return 1;
    }

    std::wcout << L"Capture device: " << get_friendly_name(*device.Get()) << L"\n";

    ComPtr<IAudioClient> format_client;
    HRESULT hr = activate_audio_client(*device.Get(), format_client);
    if (FAILED(hr)) {
        std::wcout << L"Activate(IAudioClient) failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    WAVEFORMATEX* raw_mix_format = nullptr;
    hr = format_client->GetMixFormat(&raw_mix_format);
    if (FAILED(hr)) {
        std::wcout << L"GetMixFormat failed: " << hresult_text(hr) << L"\n";
        return 1;
    }
    WaveFormatPtr mix_format(raw_mix_format);
    print_wave_format(*mix_format, L"  ");

    ComPtr<IAudioClient> client;
    bool used_audio_client3 = false;
    UINT32 requested_period_frames = 0;
    hr = initialize_shared_capture_client(
        *device.Get(),
        *mix_format,
        client,
        used_audio_client3,
        requested_period_frames);
    if (FAILED(hr)) {
        std::wcout << L"Initialize shared capture failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (event.value == nullptr) {
        std::wcout << L"CreateEvent failed: " << GetLastError() << L"\n";
        return 1;
    }

    hr = client->SetEventHandle(event.value);
    if (FAILED(hr)) {
        std::wcout << L"SetEventHandle failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    UINT32 buffer_frames = 0;
    hr = client->GetBufferSize(&buffer_frames);
    if (FAILED(hr)) {
        std::wcout << L"GetBufferSize failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    std::wcout << L"Initialized: "
               << (used_audio_client3 ? L"IAudioClient3 min-period shared stream" : L"IAudioClient shared stream")
               << L"\n";
    if (requested_period_frames != 0) {
        std::wcout << L"Requested period: " << requested_period_frames << L" frames, "
                   << frames_to_ms(requested_period_frames, mix_format->nSamplesPerSec) << L" ms\n";
    }
    std::wcout << L"Actual buffer: " << buffer_frames << L" frames, "
               << frames_to_ms(buffer_frames, mix_format->nSamplesPerSec) << L" ms\n";
    std::wcout << L"UDP payload: mono PCM16, " << mix_format->nSamplesPerSec << L" Hz\n";
    std::wcout << L"Input gain: " << input_gain << L"\n";
    if (self_ducking.enabled() && self_ducking_state != nullptr) {
        std::wcout << L"Self duck detector: threshold=" << self_ducking.threshold
                   << L", hold=" << self_ducking.hold_ms << L" ms\n";
    }

    ComPtr<IAudioCaptureClient> capture_client;
    hr = client->GetService(IID_PPV_ARGS(&capture_client));
    if (FAILED(hr)) {
        std::wcout << L"GetService(IAudioCaptureClient) failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    MmcssTask mmcss(L"Audio");
    if (mmcss.value == nullptr) {
        std::wcout << L"MMCSS Audio priority: unavailable, GetLastError=" << GetLastError() << L"\n";
    } else {
        std::wcout << L"MMCSS Audio priority: enabled\n";
    }

    LARGE_INTEGER frequency{};
    LARGE_INTEGER start_time{};
    LARGE_INTEGER previous_send_time{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start_time);
    bool have_previous_send = false;
    const std::uint64_t self_duck_hold_ticks =
        self_ducking.enabled()
            ? (static_cast<std::uint64_t>(frequency.QuadPart) * static_cast<std::uint64_t>(self_ducking.hold_ms) +
               999) /
                  1000
            : 0;

    hr = client->Start();
    if (FAILED(hr)) {
        std::wcout << L"Start failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    UdpSendStats stats;
    std::uint64_t sequence = 0;
    int last_report_second = 0;
    const double duration_ms = static_cast<double>(seconds) * 1000.0;
    std::vector<BYTE> payload;
    std::vector<BYTE> datagram;
    payload.reserve(static_cast<size_t>(buffer_frames) * sizeof(std::int16_t));
    datagram.reserve(sizeof(UdpAudioPacketHeader) + payload.capacity());

    while (true) {
        if (stop_signal != nullptr && stop_signal->load(std::memory_order_relaxed)) {
            break;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed_ms = qpc_elapsed_ms(start_time, now, frequency);
        if (should_stop_for_duration(seconds, duration_ms, elapsed_ms)) {
            break;
        }

        const DWORD wait_ms = wait_ms_for_duration(seconds, duration_ms, elapsed_ms);
        const DWORD wait_result = WaitForSingleObject(event.value, wait_ms);
        if (wait_result == WAIT_TIMEOUT) {
            ++stats.capture.wait_timeouts;
        } else if (wait_result == WAIT_OBJECT_0) {
            ++stats.capture.wait_events;

            UINT32 next_packet_frames = 0;
            hr = capture_client->GetNextPacketSize(&next_packet_frames);
            if (FAILED(hr)) {
                std::wcout << L"GetNextPacketSize failed: " << hresult_text(hr) << L"\n";
                client->Stop();
                return 1;
            }

            while (next_packet_frames > 0) {
                BYTE* data = nullptr;
                UINT32 packet_frames = 0;
                DWORD flags = 0;
                UINT64 device_position = 0;
                UINT64 qpc_position = 0;
                hr = capture_client->GetBuffer(
                    &data,
                    &packet_frames,
                    &flags,
                    &device_position,
                    &qpc_position);
                if (FAILED(hr)) {
                    std::wcout << L"GetBuffer failed: " << hresult_text(hr) << L"\n";
                    client->Stop();
                    return 1;
                }

                stats.capture.add_packet(packet_frames);
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                double packet_peak = 0.0;
                if (silent) {
                    ++stats.capture.silent_packets;
                    stats.capture.silent_frames += packet_frames;
                } else {
                    ++stats.capture.data_packets;
                    stats.capture.data_frames += packet_frames;
                    packet_peak = packet_peak_abs_sample(data, packet_frames, *mix_format);
                    stats.capture.peak_abs_sample = std::max(
                        stats.capture.peak_abs_sample,
                        packet_peak);
                }
                if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                    ++stats.capture.discontinuity_packets;
                }
                if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0) {
                    ++stats.capture.timestamp_error_packets;
                }

                if (!build_mono_pcm16_payload(data, packet_frames, *mix_format, silent, input_gain, payload)) {
                    std::wcout << L"Unsupported capture format for UDP PCM16 conversion\n";
                    capture_client->ReleaseBuffer(packet_frames);
                    client->Stop();
                    return 1;
                }

                UdpAudioPacketHeader header{};
                header.sequence = sequence++;
                header.send_qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
                header.capture_device_position = device_position;
                header.capture_qpc_position = qpc_position;
                header.sample_rate = mix_format->nSamplesPerSec;
                header.channels = 1;
                header.bits_per_sample = 16;
                header.format_tag = WAVE_FORMAT_PCM;
                header.source_channels = mix_format->nChannels;
                header.source_flags = flags;
                header.frames = packet_frames;
                header.payload_bytes = static_cast<std::uint32_t>(payload.size());

                LARGE_INTEGER send_time{};
                QueryPerformanceCounter(&send_time);
                header.send_qpc = static_cast<std::uint64_t>(send_time.QuadPart);
                if (self_ducking.enabled() &&
                    self_ducking_state != nullptr &&
                    packet_peak >= self_ducking.threshold) {
                    atomic_extend_to(
                        self_ducking_state->active_until_qpc,
                        static_cast<std::uint64_t>(send_time.QuadPart) + self_duck_hold_ticks);
                }

                datagram.resize(sizeof(header) + payload.size());
                std::memcpy(datagram.data(), &header, sizeof(header));
                std::memcpy(datagram.data() + sizeof(header), payload.data(), payload.size());

                const int sent = sendto(
                    socket_handle.value,
                    reinterpret_cast<const char*>(datagram.data()),
                    static_cast<int>(datagram.size()),
                    0,
                    reinterpret_cast<const sockaddr*>(&target),
                    target_length);
                if (sent == SOCKET_ERROR) {
                    ++stats.send_errors;
                } else {
                    ++stats.packets_sent;
                    stats.bytes_sent += static_cast<std::uint64_t>(sent);
                    stats.payload_bytes_sent += payload.size();
                    stats.frames_sent += packet_frames;
                    if (have_previous_send) {
                        stats.send_intervals.add(qpc_elapsed_ms(previous_send_time, send_time, frequency));
                    }
                    previous_send_time = send_time;
                    have_previous_send = true;
                }

                hr = capture_client->ReleaseBuffer(packet_frames);
                if (FAILED(hr)) {
                    std::wcout << L"ReleaseBuffer failed: " << hresult_text(hr) << L"\n";
                    client->Stop();
                    return 1;
                }

                hr = capture_client->GetNextPacketSize(&next_packet_frames);
                if (FAILED(hr)) {
                    std::wcout << L"GetNextPacketSize failed: " << hresult_text(hr) << L"\n";
                    client->Stop();
                    return 1;
                }
            }
        } else if (wait_result == WAIT_FAILED) {
            std::wcout << L"WaitForSingleObject failed: " << GetLastError() << L"\n";
            client->Stop();
            return 1;
        }

        QueryPerformanceCounter(&now);
        const double after_wait_elapsed_ms = qpc_elapsed_ms(start_time, now, frequency);
        const int current_second = static_cast<int>(after_wait_elapsed_ms / 1000.0);
        if (should_print_progress_second(current_second, last_report_second, seconds)) {
            last_report_second = current_second;
            print_udp_send_progress(after_wait_elapsed_ms, stats);
        }
    }

    hr = client->Stop();
    if (FAILED(hr)) {
        std::wcout << L"Stop failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    print_udp_send_summary(stats);
    return stats.send_errors == 0 ? 0 : 1;
}

int run_udp_loopback_test(
    IMMDeviceEnumerator& enumerator,
    int seconds,
    std::uint16_t port,
    double input_gain,
    ERole capture_role,
    const std::optional<std::wstring>& capture_device_selector,
    const SelfDuckingSettings& self_ducking) {
    std::wcout << L"== UDP localhost loopback test ==\n";
    std::wcout << L"Port: " << port << L", duration: " << seconds << L" seconds\n\n";
    std::wcout << L"Capture role: " << role_name(capture_role) << L"\n\n";

    int receive_result = 0;
    std::thread receiver([&receive_result, port, seconds]() {
        receive_result = run_udp_receive_test(port, seconds + 1, true, true);
    });

    Sleep(200);
    const int send_result = run_udp_send_test(
        enumerator,
        L"127.0.0.1",
        port,
        seconds,
        input_gain,
        capture_role,
        capture_device_selector,
        self_ducking,
        nullptr);
    receiver.join();
    return send_result != 0 ? send_result : receive_result;
}

void receive_udp_audio_into_jitter(
    std::uint16_t port,
    bool loopback_only,
    std::uint32_t expected_sample_rate,
    JitterBuffer& jitter_buffer,
    UdpReceiveStats& stats,
    std::atomic_bool& stop,
    std::atomic_bool& ready,
    std::atomic_bool& failed) {
    WinsockRuntime winsock;
    if (winsock.result != 0) {
        std::wcout << L"WSAStartup failed in audio receiver: " << winsock_error_text(winsock.result) << L"\n";
        failed = true;
        ready = true;
        return;
    }

    MmcssTask mmcss(L"Audio");
    if (mmcss.value == nullptr) {
        std::wcout << L"Audio receiver MMCSS priority unavailable, GetLastError="
                   << GetLastError() << L"\n";
    }

    SocketHandle socket_handle = create_udp_socket();
    if (socket_handle.value == INVALID_SOCKET) {
        std::wcout << L"audio receiver socket failed: " << winsock_error_text() << L"\n";
        failed = true;
        ready = true;
        return;
    }

    if (!bind_udp_socket(socket_handle, port, loopback_only)) {
        failed = true;
        ready = true;
        return;
    }

    LARGE_INTEGER frequency{};
    LARGE_INTEGER previous_receive_time{};
    QueryPerformanceFrequency(&frequency);
    bool have_previous_receive = false;
    std::vector<char> buffer(65536);
    std::vector<std::int16_t> decode_scratch;
    std::vector<std::int16_t> resample_scratch;
    ready = true;

    while (!stop.load()) {
        if (!wait_udp_readable(socket_handle.value, 50)) {
            continue;
        }

        sockaddr_storage from{};
        int from_length = sizeof(from);
        const int bytes = recvfrom(
            socket_handle.value,
            buffer.data(),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &from_length);
        if (bytes == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (!stop.load()) {
                std::wcout << L"audio receiver recvfrom failed: " << winsock_error_text(error) << L"\n";
                failed = true;
            }
            return;
        }

        ++stats.datagrams;
        stats.bytes_received += static_cast<std::uint64_t>(bytes);

        LARGE_INTEGER receive_time{};
        QueryPerformanceCounter(&receive_time);
        if (have_previous_receive) {
            stats.interarrival.add(qpc_elapsed_ms(previous_receive_time, receive_time, frequency));
        }
        previous_receive_time = receive_time;
        have_previous_receive = true;

        UdpAudioPacketHeader header{};
        if (bytes < 0 || !lanspeak::core::validate_udp_audio_packet(
                reinterpret_cast<const std::byte*>(buffer.data()),
                static_cast<std::size_t>(bytes),
                expected_sample_rate,
                header)) {
            ++stats.invalid_packets;
            continue;
        }

        const std::span<const std::byte> payload(
            reinterpret_cast<const std::byte*>(buffer.data() + header.header_size),
            header.payload_bytes);
        if (header.sample_rate == expected_sample_rate) {
            if (!jitter_buffer.push_pcm16_le(header.sequence, payload)) {
                ++stats.invalid_packets;
                continue;
            }
        } else {
            if (!lanspeak::core::decode_mono_pcm16_le(payload, decode_scratch) ||
                decode_scratch.size() != header.frames) {
                ++stats.invalid_packets;
                continue;
            }
            if (!lanspeak::core::resample_mono_pcm16_linear(
                    decode_scratch,
                    header.sample_rate,
                    expected_sample_rate,
                    resample_scratch)) {
                ++stats.invalid_packets;
                continue;
            }
            jitter_buffer.push(header.sequence, resample_scratch);
            ++stats.resampled_packets;
            stats.resampled_input_frames += decode_scratch.size();
            stats.resampled_output_frames += resample_scratch.size();
            stats.last_source_sample_rate = header.sample_rate;
        }

        ++stats.valid_packets;
        stats.payload_bytes_received += header.payload_bytes;
        stats.frames_received += header.frames;
        update_udp_sequence_stats(stats, header.sequence);

        if (header.send_qpc_frequency == static_cast<std::uint64_t>(frequency.QuadPart) &&
            header.send_qpc != 0 &&
            static_cast<std::uint64_t>(receive_time.QuadPart) >= header.send_qpc) {
            const std::uint64_t delta = static_cast<std::uint64_t>(receive_time.QuadPart) - header.send_qpc;
            const std::uint64_t max_reasonable_delta = static_cast<std::uint64_t>(frequency.QuadPart) * 60;
            if (delta <= max_reasonable_delta) {
                stats.sender_to_receiver_latency.add(
                    static_cast<double>(delta) * 1000.0 / static_cast<double>(frequency.QuadPart));
            }
        }
    }
}

void print_udp_play_progress(double elapsed_ms, const JitterStats& jitter, UINT32 sample_rate) {
    std::wcout << L"  play t=" << elapsed_ms / 1000.0 << L"s"
               << L", rendered=" << jitter.rendered_frames
               << L", jitter=" << frames_to_ms(static_cast<UINT32>(std::min<size_t>(jitter.current_frames, UINT32_MAX)), sample_rate) << L" ms"
               << L", after_start_underrun_frames=" << jitter.after_start_underrun_frames
               << L", after_start_underrun_events=" << jitter.after_start_underrun_events << L"\n";
}

void print_udp_play_summary(const JitterStats& jitter, const UdpPlayStats& stats, UINT32 sample_rate) {
    std::wcout << L"\nUDP play summary:\n";
    std::wcout << L"  render events:   " << jitter.render_events << L"\n";
    std::wcout << L"  rendered frames: " << jitter.rendered_frames
               << L" (" << frames_to_ms(static_cast<UINT32>(std::min<std::uint64_t>(jitter.rendered_frames, UINT32_MAX)), sample_rate) << L" ms)\n";
    std::wcout << L"  packets queued:  " << jitter.packets_pushed << L"\n";
    std::wcout << L"  frames queued:   " << jitter.frames_pushed << L"\n";
    std::wcout << L"  jitter current:  " << jitter.current_frames << L" frames, "
               << frames_to_ms(static_cast<UINT32>(std::min<size_t>(jitter.current_frames, UINT32_MAX)), sample_rate) << L" ms\n";
    std::wcout << L"  jitter max:      " << jitter.max_frames_seen << L" frames, "
               << frames_to_ms(static_cast<UINT32>(std::min<size_t>(jitter.max_frames_seen, UINT32_MAX)), sample_rate) << L" ms\n";
    std::wcout << L"  jitter gaps:     " << jitter.sequence_gaps << L"\n";
    std::wcout << L"  late/duplicate:  " << jitter.late_or_duplicate_packets << L"\n";
    std::wcout << L"  seq restarts:    " << jitter.sequence_restarts << L"\n";
    std::wcout << L"  overflow drops:  " << jitter.overflow_dropped_frames << L" frames\n";
    std::wcout << L"  underruns total: " << jitter.underrun_events << L" events, "
               << jitter.underrun_frames << L" frames\n";
    std::wcout << L"  startup silence: " << jitter.startup_underrun_events << L" events, "
               << jitter.startup_underrun_frames << L" frames\n";
    std::wcout << L"  after-start underruns: " << jitter.after_start_underrun_events << L" events, "
               << jitter.after_start_underrun_frames << L" frames\n";
    std::wcout << L"  render waits:    timeouts=" << stats.render_wait_timeouts
               << L", failures=" << stats.render_wait_failures << L"\n";
    std::wcout << L"  self ducking:    events=" << stats.ducked_render_events
               << L", frames=" << stats.ducked_render_frames << L"\n";
    std::wcout << L"  render interval: min=" << stats.render_event_intervals.printable_min_ms()
               << L" ms, avg=" << stats.render_event_intervals.average_ms()
               << L" ms, max=" << stats.render_event_intervals.max_ms
               << L" ms, samples=" << stats.render_event_intervals.count << L"\n";
    if (stats.render_padding_min != std::numeric_limits<std::uint64_t>::max()) {
        std::wcout << L"  render padding:  min=" << stats.render_padding_min
                   << L", max=" << stats.render_padding_max << L" frames\n";
    }
}

struct RoomPeerRuntime {
    RoomPeerOptions options;
    sockaddr_storage target{};
    int target_length = 0;
    std::wstring ip_key;
    IpAddressKey binary_ip_key{};
    IpEndpointKey binary_endpoint_key{};
    std::unique_ptr<JitterBuffer> jitter;
    UdpReceiveStats receive;
    std::atomic<std::uint64_t> debug_datagrams{0};
    std::atomic<std::uint64_t> debug_valid_packets{0};
    std::atomic<std::uint64_t> debug_invalid_packets{0};
    SelfDuckingSettings ducking;
    RoomPeerControl control;
    std::uint64_t send_sequence = 0;
    std::vector<std::int16_t> decode_scratch;
    std::vector<std::int16_t> resample_scratch;
    std::unique_ptr<SelfDuckingState> ducking_state;
    double current_duck_gain = 1.0;
};

struct TelemetryWriter {
    HANDLE handle = nullptr;

    bool enabled() const {
        return handle != nullptr && handle != INVALID_HANDLE_VALUE;
    }

    void write(const std::string& text) {
        if (!enabled() || text.empty()) {
            return;
        }

        DWORD written = 0;
        const BOOL ok = WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        if (!ok) {
            handle = nullptr;
        }
    }
};

void write_room_telemetry(
    TelemetryWriter& telemetry,
    TelemetrySnapshot& snapshot) {
    if (!telemetry.enabled()) {
        return;
    }
    telemetry.write(snapshot.serialize(GetTickCount64(), kRoomStreamActivityHoldMs));
}

std::optional<SelfDuckingSettings> room_peer_ducking_settings(const RoomPeerOptions& peer) {
    return SelfDuckingSettings{
        peer.self_duck_db,
        peer.self_duck_hold_ms,
        peer.self_duck_attack_ms,
        peer.self_duck_release_ms,
        peer.self_duck_threshold};
}

SelfDuckingSettings room_peer_runtime_ducking_settings(const RoomPeerRuntime& peer) {
    return SelfDuckingSettings{
        peer.control.duck_db.load(std::memory_order_relaxed),
        peer.control.duck_hold_ms.load(std::memory_order_relaxed),
        peer.control.duck_attack_ms.load(std::memory_order_relaxed),
        peer.control.duck_release_ms.load(std::memory_order_relaxed),
        peer.control.duck_threshold.load(std::memory_order_relaxed)};
}

void read_room_control_commands(
    HANDLE control_handle,
    std::vector<std::unique_ptr<RoomPeerRuntime>>& peers,
    std::atomic_bool& stop,
    std::atomic_bool* input_muted) {
    if (control_handle == nullptr || control_handle == INVALID_HANDLE_VALUE) {
        return;
    }

    std::vector<RoomPeerControl*> controls;
    controls.reserve(peers.size());
    for (const auto& peer : peers) {
        controls.push_back(&peer->control);
    }
    lanspeak::common::LineBuffer lines;
    char buffer[4096];
    while (!stop.load(std::memory_order_relaxed)) {
        DWORD available = 0;
        if (!PeekNamedPipe(control_handle, nullptr, 0, nullptr, &available, nullptr)) {
            break;
        }

        if (available == 0) {
            Sleep(15);
            continue;
        }

        DWORD bytes_read = 0;
        const DWORD bytes_to_read = std::min<DWORD>(sizeof(buffer), available);
        const BOOL ok = ReadFile(control_handle, buffer, bytes_to_read, &bytes_read, nullptr);
        if (!ok || bytes_read == 0) {
            break;
        }

        consume_room_control_bytes(
            std::string_view(buffer, bytes_read),
            lines,
            controls,
            input_muted,
            &stop,
            &std::wcout);
    }
}

bool prepare_room_peers(
    const std::vector<RoomPeerOptions>& options,
    UINT32 sample_rate,
    size_t max_jitter_frames,
    std::vector<std::unique_ptr<RoomPeerRuntime>>& peers) {
    peers.clear();

    WinsockRuntime winsock;
    if (winsock.result != 0) {
        std::wcout << L"WSAStartup failed while preparing room peers: " << winsock_error_text(winsock.result) << L"\n";
        return false;
    }

    for (const RoomPeerOptions& option : options) {
        auto peer = std::make_unique<RoomPeerRuntime>();
        peer->options = option;
        const size_t start_threshold_frames = std::max<size_t>(
            1,
            static_cast<size_t>(sample_rate) * static_cast<size_t>(option.receive_buffer_ms) / 1000);
        peer->jitter = std::make_unique<JitterBuffer>(start_threshold_frames, max_jitter_frames);
        peer->ducking = *room_peer_ducking_settings(option);
        initialize_room_peer_control(peer->control, option);
        peer->ducking_state = std::make_unique<SelfDuckingState>();

        if (!resolve_udp_target(option.host, option.port, peer->target, peer->target_length)) {
            return false;
        }

        peer->ip_key = sockaddr_ip_key(peer->target);
        const std::optional<IpAddressKey> address_key = binary_ip_key(peer->target);
        const std::optional<IpEndpointKey> endpoint_key = binary_endpoint_key(peer->target);
        if (peer->ip_key.empty() || !address_key || !endpoint_key) {
            std::wcout << L"Could not build IP key for peer " << option.host << L":" << option.port << L"\n";
            return false;
        }
        peer->binary_ip_key = *address_key;
        peer->binary_endpoint_key = *endpoint_key;

        peers.push_back(std::move(peer));
    }

    return true;
}

bool queue_udp_audio_packet_for_peer(
    const char* buffer,
    int bytes,
    std::uint32_t expected_sample_rate,
    JitterBuffer& jitter_buffer,
    std::vector<std::int16_t>& decode_scratch,
    std::vector<std::int16_t>& resample_scratch,
    UdpReceiveStats& stats,
    const LARGE_INTEGER& receive_time,
    const LARGE_INTEGER& frequency,
    bool& talk_active) {
    talk_active = false;
    ++stats.datagrams;
    stats.bytes_received += static_cast<std::uint64_t>(bytes);

    UdpAudioPacketHeader header{};
    if (bytes < 0 || !lanspeak::core::validate_udp_audio_packet(
            reinterpret_cast<const std::byte*>(buffer),
            static_cast<std::size_t>(bytes),
            expected_sample_rate,
            header)) {
        ++stats.invalid_packets;
        return false;
    }

    talk_active =
        (header.source_flags & kUdpSourceFlagTalkActive) != 0 ||
        (header.source_flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0;

    const std::span<const std::byte> payload(
        reinterpret_cast<const std::byte*>(buffer + header.header_size),
        header.payload_bytes);
    if (header.sample_rate == expected_sample_rate) {
        if (!jitter_buffer.push_pcm16_le(header.sequence, payload)) {
            ++stats.invalid_packets;
            return false;
        }
    } else {
        if (!lanspeak::core::decode_mono_pcm16_le(payload, decode_scratch) ||
            decode_scratch.size() != header.frames) {
            ++stats.invalid_packets;
            return false;
        }
        if (!lanspeak::core::resample_mono_pcm16_linear(
                decode_scratch,
                header.sample_rate,
                expected_sample_rate,
                resample_scratch)) {
            ++stats.invalid_packets;
            return false;
        }
        jitter_buffer.push(header.sequence, resample_scratch);
        ++stats.resampled_packets;
        stats.resampled_input_frames += decode_scratch.size();
        stats.resampled_output_frames += resample_scratch.size();
        stats.last_source_sample_rate = header.sample_rate;
    }

    ++stats.valid_packets;
    stats.payload_bytes_received += header.payload_bytes;
    stats.frames_received += header.frames;
    update_udp_sequence_stats(stats, header.sequence);

    if (header.send_qpc_frequency == static_cast<std::uint64_t>(frequency.QuadPart) &&
        header.send_qpc != 0 &&
        static_cast<std::uint64_t>(receive_time.QuadPart) >= header.send_qpc) {
        const std::uint64_t delta = static_cast<std::uint64_t>(receive_time.QuadPart) - header.send_qpc;
        const std::uint64_t max_reasonable_delta = static_cast<std::uint64_t>(frequency.QuadPart) * 60;
        if (delta <= max_reasonable_delta) {
            stats.sender_to_receiver_latency.add(
                static_cast<double>(delta) * 1000.0 / static_cast<double>(frequency.QuadPart));
        }
    }

    return true;
}

std::uint64_t presence_entropy() {
    std::random_device random;
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    std::uint64_t value = static_cast<std::uint64_t>(random()) << 32u;
    value ^= static_cast<std::uint64_t>(random());
    value ^= static_cast<std::uint64_t>(counter.QuadPart);
    value ^= GetTickCount64() << 17u;
    value ^= static_cast<std::uint64_t>(GetCurrentProcessId()) << 1u;
    return value == 0 ? 1 : value;
}

bool send_presence_action(
    SOCKET socket,
    const std::vector<std::unique_ptr<RoomPeerRuntime>>& peers,
    std::uint64_t session_id,
    const PresenceAction& action,
    std::uint64_t& send_errors) {
    if (action.peer_index >= peers.size()) return false;
    UdpPresencePacket packet{};
    packet.type = action.type;
    packet.session_id = session_id;
    packet.nonce = action.nonce;
    const lanspeak::core::UdpPresenceDatagram datagram =
        lanspeak::core::serialize_udp_presence_packet(packet);
    const RoomPeerRuntime& peer = *peers[action.peer_index];
    const int sent = sendto(
        socket,
        reinterpret_cast<const char*>(datagram.data()),
        static_cast<int>(datagram.size()),
        0,
        reinterpret_cast<const sockaddr*>(&peer.target),
        peer.target_length);
    if (sent == static_cast<int>(datagram.size())) return true;
    ++send_errors;
    if (send_errors <= 5 || send_errors % 100 == 0) {
        std::wcout << L"Presence send failed for peer [" << action.peer_index
                   << L"]: " << winsock_error_text() << L", count=" << send_errors << L"\n";
    }
    return false;
}

void update_presence_telemetry(
    const PresenceTracker& tracker,
    TelemetrySnapshot& telemetry_snapshot,
    std::size_t peer_count) {
    for (std::size_t index = 0; index < peer_count; ++index) {
        const lanspeak::core::PeerPresenceSnapshot snapshot = tracker.snapshot(index);
        telemetry_snapshot.update_peer_presence(index, snapshot.state, snapshot.rtt_ms);
    }
}

void receive_room_udp_audio(
    std::uint16_t port,
    const std::wstring& bind_address,
    std::uint32_t expected_sample_rate,
    std::vector<std::unique_ptr<RoomPeerRuntime>>& peers,
    TelemetrySnapshot& telemetry_snapshot,
    std::atomic_bool& presence_enabled,
    std::atomic_bool& stop,
    std::atomic_bool& ready,
    std::atomic_bool& failed) {
    WinsockRuntime winsock;
    if (winsock.result != 0) {
        std::wcout << L"WSAStartup failed in room receiver: " << winsock_error_text(winsock.result) << L"\n";
        failed = true;
        ready = true;
        return;
    }

    MmcssTask mmcss(L"Audio");
    if (mmcss.value == nullptr) {
        std::wcout << L"Room receiver MMCSS Audio priority unavailable, GetLastError="
                   << GetLastError() << L"\n";
    }

    SocketHandle socket_handle = create_udp_socket();
    if (socket_handle.value == INVALID_SOCKET) {
        std::wcout << L"room receiver socket failed: " << winsock_error_text() << L"\n";
        failed = true;
        ready = true;
        return;
    }

    if (!bind_udp_socket(socket_handle, port, false, bind_address)) {
        failed = true;
        ready = true;
        return;
    }

    std::map<IpAddressKey, size_t> peer_by_ip;
    std::map<IpEndpointKey, size_t> peer_by_endpoint;
    for (size_t index = 0; index < peers.size(); ++index) {
        const IpAddressKey& key = peers[index]->binary_ip_key;
        if (peer_by_ip.find(key) == peer_by_ip.end()) {
            peer_by_ip[key] = index;
        }
        peer_by_endpoint[peers[index]->binary_endpoint_key] = index;
    }

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    std::vector<LARGE_INTEGER> previous_receive_times(peers.size());
    std::vector<bool> have_previous_receive(peers.size(), false);
    std::vector<char> buffer(65536);
    std::uint64_t unknown_datagrams = 0;
    std::uint64_t presence_send_errors = 0;
    const std::uint64_t session_id = presence_entropy();
    PresenceTracker presence(peers.size(), session_id, presence_entropy());
    bool presence_started = false;
    ready = true;

    while (!stop.load()) {
        const std::uint64_t before_wait_ms = GetTickCount64();
        if (!presence_started && presence_enabled.load(std::memory_order_relaxed)) {
            presence.start(before_wait_ms);
            presence_started = true;
        }
        if (presence_started) {
            for (const PresenceAction& action : presence.tick(before_wait_ms)) {
                send_presence_action(
                    socket_handle.value,
                    peers,
                    presence.session_id(),
                    action,
                    presence_send_errors);
            }
            update_presence_telemetry(presence, telemetry_snapshot, peers.size());
        }

        if (!wait_udp_readable(socket_handle.value, 50)) {
            continue;
        }

        sockaddr_storage from{};
        int from_length = sizeof(from);
        const int bytes = recvfrom(
            socket_handle.value,
            buffer.data(),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &from_length);
        if (bytes == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (!stop.load()) {
                std::wcout << L"room receiver recvfrom failed: " << winsock_error_text(error) << L"\n";
                failed = true;
            }
            break;
        }

        UdpPresencePacket presence_packet{};
        const bool is_presence_packet = lanspeak::core::validate_udp_presence_packet(
            std::span(
                reinterpret_cast<const std::byte*>(buffer.data()),
                static_cast<std::size_t>(std::max(bytes, 0))),
            presence_packet);
        if (is_presence_packet) {
            const std::optional<IpEndpointKey> endpoint_key = binary_endpoint_key(from);
            const auto endpoint_it = endpoint_key
                ? peer_by_endpoint.find(*endpoint_key)
                : peer_by_endpoint.end();
            if (endpoint_it == peer_by_endpoint.end()) {
                ++unknown_datagrams;
                continue;
            }
            if (presence_started) {
                const std::uint64_t now_ms = GetTickCount64();
                const std::optional<PresenceAction> response = presence.on_packet(
                    endpoint_it->second,
                    presence_packet,
                    now_ms);
                if (response) {
                    send_presence_action(
                        socket_handle.value,
                        peers,
                        presence.session_id(),
                        *response,
                        presence_send_errors);
                }
                update_presence_telemetry(presence, telemetry_snapshot, peers.size());
            }
            continue;
        }

        const std::optional<IpAddressKey> from_binary_key = binary_ip_key(from);
        const auto peer_it = from_binary_key ? peer_by_ip.find(*from_binary_key) : peer_by_ip.end();
        if (peer_it == peer_by_ip.end()) {
            ++unknown_datagrams;
            if (unknown_datagrams <= 5 || unknown_datagrams % 100 == 0) {
                const std::wstring from_key = sockaddr_ip_key(from);
                std::wcout << L"room receiver ignored datagram from unknown IP: "
                           << (from_key.empty() ? L"<unknown>" : from_key)
                           << L", count=" << unknown_datagrams << L"\n";
            }
            continue;
        }

        const size_t peer_index = peer_it->second;
        RoomPeerRuntime& peer = *peers[peer_index];
        peer.debug_datagrams.fetch_add(1, std::memory_order_relaxed);

        LARGE_INTEGER receive_time{};
        QueryPerformanceCounter(&receive_time);
        if (have_previous_receive[peer_index]) {
            peer.receive.interarrival.add(
                qpc_elapsed_ms(previous_receive_times[peer_index], receive_time, frequency));
        }
        previous_receive_times[peer_index] = receive_time;
        have_previous_receive[peer_index] = true;

        bool talk_active = false;
        const bool queued = queue_udp_audio_packet_for_peer(
            buffer.data(),
            bytes,
            expected_sample_rate,
            *peer.jitter,
            peer.decode_scratch,
            peer.resample_scratch,
            peer.receive,
            receive_time,
            frequency,
            talk_active);
        if (queued) {
            peer.debug_valid_packets.fetch_add(1, std::memory_order_relaxed);
            if (presence_started) {
                presence.on_audio_packet(peer_index, GetTickCount64());
                update_presence_telemetry(presence, telemetry_snapshot, peers.size());
            }
            if (talk_active) {
                telemetry_snapshot.mark_peer_stream(peer_index, GetTickCount64());
            }
        } else {
            peer.debug_invalid_packets.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (presence_started) {
        for (const PresenceAction& action : presence.shutdown()) {
            send_presence_action(
                socket_handle.value,
                peers,
                presence.session_id(),
                action,
                presence_send_errors);
        }
    }
}

bool fill_render_buffer_from_room_peers(
    BYTE* destination,
    UINT32 frames,
    const WAVEFORMATEX& render_format,
    std::vector<std::unique_ptr<RoomPeerRuntime>>& peers,
    double output_gain,
    const LARGE_INTEGER& render_event_time,
    double buffer_ms,
    UdpPlayStats& stats,
    RoomMixer& mixer,
    TelemetrySnapshot& telemetry_snapshot) {
    mixer.begin(frames);
    std::span<double> mixed = mixer.mixed_frames();
    std::span<std::int16_t> mono_frames = mixer.mono_scratch();

    for (size_t peer_index = 0; peer_index < peers.size(); ++peer_index) {
        const std::unique_ptr<RoomPeerRuntime>& peer_ptr = peers[peer_index];
        RoomPeerRuntime& peer = *peer_ptr;
        peer.jitter->pop_into(mono_frames);
        const double peer_gain = peer.control.gain.load(std::memory_order_relaxed);
        const SelfDuckingSettings ducking = room_peer_runtime_ducking_settings(peer);

        const bool duck_active =
            ducking.enabled() &&
            peer.ducking_state != nullptr &&
            peer.ducking_state->active_until_qpc.load(std::memory_order_relaxed) >=
                static_cast<std::uint64_t>(render_event_time.QuadPart);
        const double target_duck_gain = duck_active ? ducking.duck_gain() : 1.0;
        const int smoothing_ms =
            target_duck_gain < peer.current_duck_gain
                ? ducking.attack_ms
                : ducking.release_ms;
        const double next_duck_gain = ducking.enabled()
            ? smooth_toward(peer.current_duck_gain, target_duck_gain, buffer_ms, smoothing_ms)
            : 1.0;
        const bool duck_applied =
            duck_active || peer.current_duck_gain < 0.999 || next_duck_gain < 0.999;
        if (duck_applied) {
            ++stats.ducked_render_events;
            stats.ducked_render_frames += frames;
        }

        double meter_sum_squares = 0.0;
        double meter_peak = 0.0;
        for (UINT32 frame = 0; frame < frames; ++frame) {
            const double ramp =
                frames > 1 ? static_cast<double>(frame) / static_cast<double>(frames - 1) : 1.0;
            const double duck_gain =
                peer.current_duck_gain + (next_duck_gain - peer.current_duck_gain) * ramp;
            const double source_sample = static_cast<double>(mono_frames[frame]) / 32768.0;
            const double gain = output_gain * peer_gain * duck_gain;
            const double sample = source_sample * gain;
            mixed[frame] += sample;
            const double abs_source_sample = std::abs(source_sample);
            meter_peak = std::max(meter_peak, abs_source_sample);
            meter_sum_squares += source_sample * source_sample;
        }

        peer.current_duck_gain = next_duck_gain;
        const double meter_rms =
            frames > 0 ? std::sqrt(meter_sum_squares / static_cast<double>(frames)) : 0.0;
        const double render_level_db = audio_level_dbfs(meter_rms);
        telemetry_snapshot.update_peer_meter(
            peer_index,
            render_level_db,
            meter_peak >= 0.015 || render_level_db >= -42.0);
    }

    return write_room_mix_to_render_buffer(destination, frames, render_format, mixer.mixed_frames());
}

int run_udp_play_test(
    IMMDeviceEnumerator& enumerator,
    std::uint16_t port,
    int seconds,
    bool loopback_only,
    double output_gain,
    ERole render_role,
    const std::optional<std::wstring>& render_device_selector,
    const SelfDuckingSettings& self_ducking,
    const SelfDuckingState* self_ducking_state,
    const std::atomic_bool* stop_signal = nullptr) {
    std::wcout << L"== UDP audio player ==\n";
    std::wcout << L"Listening on " << (loopback_only ? L"127.0.0.1" : L"0.0.0.0")
               << L":" << port << L" for " << duration_text(seconds) << L"\n";
    std::wcout << L"Render role: " << role_name(render_role) << L"\n";

    ComPtr<IMMDevice> device;
    if (!select_audio_endpoint(enumerator, eRender, render_role, render_device_selector, L"render", device)) {
        return 1;
    }

    std::wcout << L"Render device: " << get_friendly_name(*device.Get()) << L"\n";

    ComPtr<IAudioClient> format_client;
    HRESULT hr = activate_audio_client(*device.Get(), format_client);
    if (FAILED(hr)) {
        std::wcout << L"Activate render IAudioClient failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    WAVEFORMATEX* raw_mix_format = nullptr;
    hr = format_client->GetMixFormat(&raw_mix_format);
    if (FAILED(hr)) {
        std::wcout << L"Render GetMixFormat failed: " << hresult_text(hr) << L"\n";
        return 1;
    }
    WaveFormatPtr mix_format(raw_mix_format);
    print_wave_format(*mix_format, L"  ");

    if (sample_kind(*mix_format) == SampleKind::unknown) {
        std::wcout << L"Unsupported render mix format for first UDP player prototype\n";
        return 1;
    }

    ComPtr<IAudioClient> client;
    bool used_audio_client3 = false;
    UINT32 requested_period_frames = 0;
    hr = initialize_shared_capture_client(
        *device.Get(),
        *mix_format,
        client,
        used_audio_client3,
        requested_period_frames);
    if (FAILED(hr)) {
        std::wcout << L"Initialize shared render failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (event.value == nullptr) {
        std::wcout << L"CreateEvent failed: " << GetLastError() << L"\n";
        return 1;
    }

    hr = client->SetEventHandle(event.value);
    if (FAILED(hr)) {
        std::wcout << L"Render SetEventHandle failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    UINT32 buffer_frames = 0;
    hr = client->GetBufferSize(&buffer_frames);
    if (FAILED(hr)) {
        std::wcout << L"Render GetBufferSize failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    ComPtr<IAudioRenderClient> render_client;
    hr = client->GetService(IID_PPV_ARGS(&render_client));
    if (FAILED(hr)) {
        std::wcout << L"GetService(IAudioRenderClient) failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    const UINT32 sample_rate = mix_format->nSamplesPerSec;
    const size_t start_threshold_frames = static_cast<size_t>(sample_rate / 50); // 20 ms.
    const size_t max_jitter_frames = static_cast<size_t>(sample_rate / 2);       // 500 ms safety cap.
    JitterBuffer jitter_buffer(start_threshold_frames, max_jitter_frames);
    UdpPlayStats stats;
    std::vector<std::int16_t> render_mono_scratch;
    render_mono_scratch.reserve(buffer_frames);
    std::atomic_bool receiver_stop{false};
    std::atomic_bool receiver_ready{false};
    std::atomic_bool receiver_failed{false};

    std::thread receiver([&]() {
        receive_udp_audio_into_jitter(
            port,
            loopback_only,
            sample_rate,
            jitter_buffer,
            stats.receive,
            receiver_stop,
            receiver_ready,
            receiver_failed);
    });

    for (int attempt = 0; attempt < 100 && !receiver_ready.load(); ++attempt) {
        Sleep(10);
    }

    if (!receiver_ready.load() || receiver_failed.load()) {
        receiver_stop = true;
        receiver.join();
        std::wcout << L"UDP audio receiver failed to start\n";
        return 1;
    }

    std::wcout << L"Initialized render: "
               << (used_audio_client3 ? L"IAudioClient3 min-period shared stream" : L"IAudioClient shared stream")
               << L"\n";
    if (requested_period_frames != 0) {
        std::wcout << L"Requested render period: " << requested_period_frames << L" frames, "
                   << frames_to_ms(requested_period_frames, sample_rate) << L" ms\n";
    }
    std::wcout << L"Render buffer: " << buffer_frames << L" frames, "
               << frames_to_ms(buffer_frames, sample_rate) << L" ms\n";
    std::wcout << L"Jitter start threshold: " << start_threshold_frames << L" frames, "
               << frames_to_ms(static_cast<UINT32>(start_threshold_frames), sample_rate) << L" ms\n";
    std::wcout << L"Output gain: " << output_gain << L"\n";
    if (self_ducking.enabled() && self_ducking_state != nullptr) {
        std::wcout << L"Self ducking: " << self_ducking.duck_db << L" dB, hold="
                   << self_ducking.hold_ms << L" ms, attack=" << self_ducking.attack_ms
                   << L" ms, release=" << self_ducking.release_ms
                   << L" ms, threshold=" << self_ducking.threshold << L"\n";
    } else {
        std::wcout << L"Self ducking: off\n";
    }

    const UINT32 prefill_frames = std::min(
        buffer_frames,
        requested_period_frames != 0 ? requested_period_frames : std::max<UINT32>(1, sample_rate / 100));
    BYTE* prefill = nullptr;
    hr = render_client->GetBuffer(prefill_frames, &prefill);
    if (FAILED(hr)) {
        receiver_stop = true;
        receiver.join();
        std::wcout << L"Render prefill GetBuffer failed: " << hresult_text(hr) << L"\n";
        return 1;
    }
    std::memset(prefill, 0, static_cast<size_t>(prefill_frames) * mix_format->nBlockAlign);
    hr = render_client->ReleaseBuffer(prefill_frames, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) {
        receiver_stop = true;
        receiver.join();
        std::wcout << L"Render prefill ReleaseBuffer failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    MmcssTask mmcss(L"Audio");
    if (mmcss.value == nullptr) {
        std::wcout << L"MMCSS Audio priority: unavailable, GetLastError=" << GetLastError() << L"\n";
    } else {
        std::wcout << L"MMCSS Audio priority: enabled\n";
    }

    LARGE_INTEGER frequency{};
    LARGE_INTEGER start_time{};
    LARGE_INTEGER previous_render_event{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start_time);
    bool have_previous_render_event = false;
    int last_report_second = 0;
    const double duration_ms = static_cast<double>(seconds) * 1000.0;
    double current_self_duck_gain = 1.0;

    hr = client->Start();
    if (FAILED(hr)) {
        receiver_stop = true;
        receiver.join();
        std::wcout << L"Render Start failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    while (true) {
        if (stop_signal != nullptr && stop_signal->load(std::memory_order_relaxed)) {
            break;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed_ms = qpc_elapsed_ms(start_time, now, frequency);
        if (should_stop_for_duration(seconds, duration_ms, elapsed_ms)) {
            break;
        }

        const DWORD wait_ms = wait_ms_for_duration(seconds, duration_ms, elapsed_ms);
        const DWORD wait_result = WaitForSingleObject(event.value, wait_ms);
        if (wait_result == WAIT_TIMEOUT) {
            ++stats.render_wait_timeouts;
        } else if (wait_result == WAIT_OBJECT_0) {
            LARGE_INTEGER render_event_time{};
            QueryPerformanceCounter(&render_event_time);
            if (have_previous_render_event) {
                stats.render_event_intervals.add(qpc_elapsed_ms(previous_render_event, render_event_time, frequency));
            }
            previous_render_event = render_event_time;
            have_previous_render_event = true;

            UINT32 padding = 0;
            hr = client->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                std::wcout << L"Render GetCurrentPadding failed: " << hresult_text(hr) << L"\n";
                client->Stop();
                receiver_stop = true;
                receiver.join();
                return 1;
            }
            stats.render_padding_min = std::min<std::uint64_t>(stats.render_padding_min, padding);
            stats.render_padding_max = std::max<std::uint64_t>(stats.render_padding_max, padding);

            const UINT32 available_frames = buffer_frames > padding ? buffer_frames - padding : 0;
            if (available_frames > 0) {
                BYTE* render_data = nullptr;
                hr = render_client->GetBuffer(available_frames, &render_data);
                if (FAILED(hr)) {
                    std::wcout << L"Render GetBuffer failed: " << hresult_text(hr) << L"\n";
                    client->Stop();
                    receiver_stop = true;
                    receiver.join();
                    return 1;
                }

                const bool duck_active =
                    self_ducking.enabled() &&
                    self_ducking_state != nullptr &&
                    self_ducking_state->active_until_qpc.load(std::memory_order_relaxed) >=
                        static_cast<std::uint64_t>(render_event_time.QuadPart);
                const double target_self_duck_gain =
                    duck_active ? self_ducking.duck_gain() : 1.0;
                const int smoothing_ms =
                    target_self_duck_gain < current_self_duck_gain
                        ? self_ducking.attack_ms
                        : self_ducking.release_ms;
                const double buffer_ms = frames_to_ms(available_frames, sample_rate);
                const double next_self_duck_gain = self_ducking.enabled() && self_ducking_state != nullptr
                    ? smooth_toward(current_self_duck_gain, target_self_duck_gain, buffer_ms, smoothing_ms)
                    : 1.0;
                const bool duck_applied =
                    duck_active || current_self_duck_gain < 0.999 || next_self_duck_gain < 0.999;
                const double output_gain_start = output_gain * current_self_duck_gain;
                const double output_gain_end = output_gain * next_self_duck_gain;
                if (duck_applied) {
                    ++stats.ducked_render_events;
                    stats.ducked_render_frames += available_frames;
                }

                if (!fill_render_buffer_from_mono_pcm16(
                        render_data,
                        available_frames,
                        *mix_format,
                        jitter_buffer,
                        output_gain_start,
                        output_gain_end,
                        render_mono_scratch)) {
                    render_client->ReleaseBuffer(available_frames, AUDCLNT_BUFFERFLAGS_SILENT);
                    client->Stop();
                    receiver_stop = true;
                    receiver.join();
                    std::wcout << L"Failed to fill render buffer from jitter buffer\n";
                    return 1;
                }
                current_self_duck_gain = next_self_duck_gain;

                hr = render_client->ReleaseBuffer(available_frames, 0);
                if (FAILED(hr)) {
                    std::wcout << L"Render ReleaseBuffer failed: " << hresult_text(hr) << L"\n";
                    client->Stop();
                    receiver_stop = true;
                    receiver.join();
                    return 1;
                }
            }
        } else if (wait_result == WAIT_FAILED) {
            ++stats.render_wait_failures;
            std::wcout << L"Render WaitForSingleObject failed: " << GetLastError() << L"\n";
            client->Stop();
            receiver_stop = true;
            receiver.join();
            return 1;
        }

        QueryPerformanceCounter(&now);
        const double after_wait_elapsed_ms = qpc_elapsed_ms(start_time, now, frequency);
        const int current_second = static_cast<int>(after_wait_elapsed_ms / 1000.0);
        if (should_print_progress_second(current_second, last_report_second, seconds)) {
            last_report_second = current_second;
            print_udp_play_progress(after_wait_elapsed_ms, jitter_buffer.snapshot(), sample_rate);
        }
    }

    hr = client->Stop();
    receiver_stop = true;
    receiver.join();

    if (FAILED(hr)) {
        std::wcout << L"Render Stop failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    print_udp_receive_summary(stats.receive);
    print_udp_play_summary(jitter_buffer.snapshot(), stats, sample_rate);
    return receiver_failed.load() ? 1 : 0;
}

int run_udp_send_test_in_thread_context(
    const std::wstring& host,
    std::uint16_t port,
    int seconds,
    double input_gain,
    ERole capture_role,
    const std::optional<std::wstring>& capture_device_selector,
    const SelfDuckingSettings& self_ducking,
    SelfDuckingState* self_ducking_state,
    const std::atomic_bool* stop_signal = nullptr) {
    const ComRuntime com;
    if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE) {
        std::wcout << L"Sender thread CoInitializeEx failed: " << hresult_text(com.hr) << L"\n";
        return 1;
    }

    ComPtr<IMMDeviceEnumerator> sender_enumerator;
    const HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&sender_enumerator));
    if (FAILED(hr)) {
        std::wcout << L"Sender thread MMDeviceEnumerator failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    return run_udp_send_test(
        *sender_enumerator.Get(),
        host,
        port,
        seconds,
        input_gain,
        capture_role,
        capture_device_selector,
        self_ducking,
        self_ducking_state,
        stop_signal);
}

int run_room_send_test(
    IMMDeviceEnumerator& enumerator,
    std::vector<std::unique_ptr<RoomPeerRuntime>>& peers,
    const std::wstring& bind_address,
    int seconds,
    double input_gain,
    ERole capture_role,
    const std::optional<std::wstring>& capture_device_selector,
    TelemetrySnapshot* telemetry_snapshot = nullptr,
    const std::atomic_bool* input_muted = nullptr,
    const std::atomic_bool* stop_signal = nullptr) {
    WinsockRuntime winsock;
    if (winsock.result != 0) {
        std::wcout << L"WSAStartup failed: " << winsock_error_text(winsock.result) << L"\n";
        return 1;
    }

    SocketHandle socket_handle = create_udp_socket();
    if (socket_handle.value == INVALID_SOCKET) {
        std::wcout << L"socket(AF_INET, SOCK_DGRAM) failed: " << winsock_error_text() << L"\n";
        return 1;
    }
    if (!bind_udp_socket(socket_handle, 0, false, bind_address)) {
        return 1;
    }

    std::wcout << L"== UDP room capture sender ==\n";
    std::wcout << L"Targets: " << peers.size() << L"\n";
    for (size_t index = 0; index < peers.size(); ++index) {
        const RoomPeerRuntime& peer = *peers[index];
        std::wcout << L"  [" << index << L"] " << peer.options.host << L":" << peer.options.port
                   << L", gain=" << peer.options.gain
                   << L", duck-db=" << peer.options.self_duck_db
                   << L", global-ptt=" << (peer.options.global_ptt_enabled ? L"on" : L"off") << L"\n";
    }
    std::wcout << L"Duration: " << duration_text(seconds) << L"\n";
    std::wcout << L"Capture role: " << role_name(capture_role) << L"\n";

    ComPtr<IMMDevice> device;
    if (!select_audio_endpoint(enumerator, eCapture, capture_role, capture_device_selector, L"capture", device)) {
        return 1;
    }

    const std::wstring capture_device_name = get_friendly_name(*device.Get());
    std::wcout << L"Capture device: " << capture_device_name << L"\n";

    ComPtr<IAudioClient> format_client;
    HRESULT hr = activate_audio_client(*device.Get(), format_client);
    if (FAILED(hr)) {
        std::wcout << L"Activate(IAudioClient) failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    WAVEFORMATEX* raw_mix_format = nullptr;
    hr = format_client->GetMixFormat(&raw_mix_format);
    if (FAILED(hr)) {
        std::wcout << L"GetMixFormat failed: " << hresult_text(hr) << L"\n";
        return 1;
    }
    WaveFormatPtr mix_format(raw_mix_format);
    print_wave_format(*mix_format, L"  ");

    ComPtr<IAudioClient> client;
    bool used_audio_client3 = false;
    UINT32 requested_period_frames = 0;
    hr = initialize_shared_capture_client(
        *device.Get(),
        *mix_format,
        client,
        used_audio_client3,
        requested_period_frames);
    if (FAILED(hr)) {
        std::wcout << L"Initialize shared capture failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (event.value == nullptr) {
        std::wcout << L"CreateEvent failed: " << GetLastError() << L"\n";
        return 1;
    }

    hr = client->SetEventHandle(event.value);
    if (FAILED(hr)) {
        std::wcout << L"SetEventHandle failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    UINT32 buffer_frames = 0;
    hr = client->GetBufferSize(&buffer_frames);
    if (FAILED(hr)) {
        std::wcout << L"GetBufferSize failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    REFERENCE_TIME stream_latency = 0;
    const HRESULT stream_latency_result = client->GetStreamLatency(&stream_latency);
    if (telemetry_snapshot != nullptr) {
        telemetry_snapshot->set_audio_endpoint_diagnostics(
            lanspeak::core::AudioEndpointKind::capture,
            lanspeak::core::AudioEndpointDiagnostics{
                true,
                lanspeak::common::wide_to_utf8(capture_device_name),
                mix_format->nSamplesPerSec,
                mix_format->nChannels,
                mix_format->wBitsPerSample,
                requested_period_frames != 0
                    ? frames_to_ms(requested_period_frames, mix_format->nSamplesPerSec)
                    : -1.0,
                frames_to_ms(buffer_frames, mix_format->nSamplesPerSec),
                SUCCEEDED(stream_latency_result) ? reference_time_to_ms(stream_latency) : -1.0,
                used_audio_client3});
    }

    std::wcout << L"Initialized: "
               << (used_audio_client3 ? L"IAudioClient3 min-period shared stream" : L"IAudioClient shared stream")
               << L"\n";
    if (requested_period_frames != 0) {
        std::wcout << L"Requested period: " << requested_period_frames << L" frames, "
                   << frames_to_ms(requested_period_frames, mix_format->nSamplesPerSec) << L" ms\n";
    }
    std::wcout << L"Actual buffer: " << buffer_frames << L" frames, "
               << frames_to_ms(buffer_frames, mix_format->nSamplesPerSec) << L" ms\n";
    std::wcout << L"UDP payload: mono PCM16, " << mix_format->nSamplesPerSec << L" Hz\n";
    std::wcout << L"Outgoing input gain: " << input_gain << L"\n";
    if (input_muted != nullptr && input_muted->load(std::memory_order_relaxed)) {
        std::wcout << L"Outgoing input starts muted\n";
    }

    ComPtr<IAudioCaptureClient> capture_client;
    hr = client->GetService(IID_PPV_ARGS(&capture_client));
    if (FAILED(hr)) {
        std::wcout << L"GetService(IAudioCaptureClient) failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    MmcssTask mmcss(L"Audio");
    if (mmcss.value == nullptr) {
        std::wcout << L"MMCSS Audio priority: unavailable, GetLastError=" << GetLastError() << L"\n";
    } else {
        std::wcout << L"MMCSS Audio priority: enabled\n";
    }

    LARGE_INTEGER frequency{};
    LARGE_INTEGER start_time{};
    LARGE_INTEGER previous_send_time{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start_time);
    bool have_previous_send = false;

    hr = client->Start();
    if (FAILED(hr)) {
        std::wcout << L"Start failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    UdpSendStats stats;
    int last_report_second = 0;
    const double duration_ms = static_cast<double>(seconds) * 1000.0;
    std::vector<BYTE> payload;
    std::vector<BYTE> datagram;
    payload.reserve(static_cast<size_t>(buffer_frames) * sizeof(std::int16_t));
    datagram.reserve(sizeof(UdpAudioPacketHeader) + payload.capacity());
    PeerRouter peer_router(peers.size());

    while (true) {
        if (stop_signal != nullptr && stop_signal->load(std::memory_order_relaxed)) {
            break;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed_ms = qpc_elapsed_ms(start_time, now, frequency);
        if (should_stop_for_duration(seconds, duration_ms, elapsed_ms)) {
            break;
        }

        const DWORD wait_ms = wait_ms_for_duration(seconds, duration_ms, elapsed_ms);
        const DWORD wait_result = WaitForSingleObject(event.value, wait_ms);
        if (wait_result == WAIT_TIMEOUT) {
            ++stats.capture.wait_timeouts;
        } else if (wait_result == WAIT_OBJECT_0) {
            ++stats.capture.wait_events;

            UINT32 next_packet_frames = 0;
            hr = capture_client->GetNextPacketSize(&next_packet_frames);
            if (FAILED(hr)) {
                std::wcout << L"GetNextPacketSize failed: " << hresult_text(hr) << L"\n";
                client->Stop();
                return 1;
            }

            while (next_packet_frames > 0) {
                BYTE* data = nullptr;
                UINT32 packet_frames = 0;
                DWORD flags = 0;
                UINT64 device_position = 0;
                UINT64 qpc_position = 0;
                hr = capture_client->GetBuffer(
                    &data,
                    &packet_frames,
                    &flags,
                    &device_position,
                    &qpc_position);
                if (FAILED(hr)) {
                    std::wcout << L"GetBuffer failed: " << hresult_text(hr) << L"\n";
                    client->Stop();
                    return 1;
                }

                stats.capture.add_packet(packet_frames);
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                double packet_peak = 0.0;
                if (silent) {
                    ++stats.capture.silent_packets;
                    stats.capture.silent_frames += packet_frames;
                } else {
                    ++stats.capture.data_packets;
                    stats.capture.data_frames += packet_frames;
                    packet_peak = packet_peak_abs_sample(data, packet_frames, *mix_format);
                    stats.capture.peak_abs_sample = std::max(stats.capture.peak_abs_sample, packet_peak);
                }
                if (telemetry_snapshot != nullptr) {
                    telemetry_snapshot->accumulate_local_peak(packet_peak);
                }
                if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                    ++stats.capture.discontinuity_packets;
                }
                if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0) {
                    ++stats.capture.timestamp_error_packets;
                }

                const bool muted_by_control =
                    input_muted != nullptr && input_muted->load(std::memory_order_relaxed);
                peer_router.begin(muted_by_control);
                bool local_talk_active = !muted_by_control;
                for (size_t peer_index = 0; peer_index < peers.size(); ++peer_index) {
                    const auto& peer = peers[peer_index];
                    const bool private_talk = peer->control.private_talk.load(std::memory_order_relaxed);
                    const lanspeak::core::PeerRouteState route{
                        peer->control.global_ptt_enabled.load(std::memory_order_relaxed),
                        private_talk,
                        peer->send_sequence};
                    peer_router.consider(peer_index, route);
                    local_talk_active = local_talk_active || private_talk;
                }

                if (local_talk_active && packet_peak > 0.0) {
                    LARGE_INTEGER activity_time{};
                    QueryPerformanceCounter(&activity_time);
                    for (const auto& peer : peers) {
                        const SelfDuckingSettings ducking = room_peer_runtime_ducking_settings(*peer);
                        if (!ducking.enabled() || peer->ducking_state == nullptr ||
                            packet_peak < ducking.threshold) {
                            continue;
                        }
                        const std::uint64_t duck_hold_ticks =
                            (static_cast<std::uint64_t>(frequency.QuadPart) *
                                 static_cast<std::uint64_t>(ducking.hold_ms) +
                             999) /
                            1000;
                        atomic_extend_to(
                            peer->ducking_state->active_until_qpc,
                            static_cast<std::uint64_t>(activity_time.QuadPart) + duck_hold_ticks);
                    }
                }
                const std::span<const size_t> voice_targets = peer_router.targets();
                if (voice_targets.empty()) {
                    stats.packets_suppressed += static_cast<std::uint64_t>(peers.size());
                    stats.frames_suppressed +=
                        static_cast<std::uint64_t>(packet_frames) * static_cast<std::uint64_t>(peers.size());

                    hr = capture_client->ReleaseBuffer(packet_frames);
                    if (FAILED(hr)) {
                        std::wcout << L"ReleaseBuffer failed: " << hresult_text(hr) << L"\n";
                        client->Stop();
                        return 1;
                    }

                    hr = capture_client->GetNextPacketSize(&next_packet_frames);
                    if (FAILED(hr)) {
                        std::wcout << L"GetNextPacketSize failed: " << hresult_text(hr) << L"\n";
                        client->Stop();
                        return 1;
                    }
                    continue;
                }

                if (!build_mono_pcm16_payload(
                        data,
                        packet_frames,
                        *mix_format,
                        silent,
                        input_gain,
                        payload)) {
                    std::wcout << L"Unsupported capture format for UDP PCM16 conversion\n";
                    capture_client->ReleaseBuffer(packet_frames);
                    client->Stop();
                    return 1;
                }
                UdpAudioPacketHeader header{};
                header.send_qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
                header.capture_device_position = device_position;
                header.capture_qpc_position = qpc_position;
                header.sample_rate = mix_format->nSamplesPerSec;
                header.channels = 1;
                header.bits_per_sample = 16;
                header.format_tag = WAVE_FORMAT_PCM;
                header.source_channels = mix_format->nChannels;
                header.source_flags = flags | kUdpSourceFlagTalkActive;
                header.frames = packet_frames;
                header.payload_bytes = static_cast<std::uint32_t>(payload.size());

                LARGE_INTEGER send_time{};
                QueryPerformanceCounter(&send_time);
                header.send_qpc = static_cast<std::uint64_t>(send_time.QuadPart);

                datagram.resize(sizeof(header) + payload.size());
                std::memcpy(datagram.data() + sizeof(header), payload.data(), payload.size());

                stats.packets_suppressed +=
                    static_cast<std::uint64_t>(peers.size() - voice_targets.size());
                stats.frames_suppressed +=
                    static_cast<std::uint64_t>(packet_frames) *
                    static_cast<std::uint64_t>(peers.size() - voice_targets.size());

                for (const size_t peer_index : voice_targets) {
                    RoomPeerRuntime& peer = *peers[peer_index];
                    UdpAudioPacketHeader peer_header = header;
                    peer_header.sequence = peer.send_sequence++;
                    std::memcpy(datagram.data(), &peer_header, sizeof(peer_header));

                    const int sent = sendto(
                        socket_handle.value,
                        reinterpret_cast<const char*>(datagram.data()),
                        static_cast<int>(datagram.size()),
                        0,
                        reinterpret_cast<const sockaddr*>(&peer.target),
                        peer.target_length);
                    if (sent == SOCKET_ERROR) {
                        ++stats.send_errors;
                    } else {
                        ++stats.packets_sent;
                        stats.bytes_sent += static_cast<std::uint64_t>(sent);
                        stats.payload_bytes_sent += payload.size();
                        stats.frames_sent += packet_frames;
                        if (have_previous_send) {
                            stats.send_intervals.add(qpc_elapsed_ms(previous_send_time, send_time, frequency));
                        }
                        previous_send_time = send_time;
                        have_previous_send = true;
                    }
                }

                hr = capture_client->ReleaseBuffer(packet_frames);
                if (FAILED(hr)) {
                    std::wcout << L"ReleaseBuffer failed: " << hresult_text(hr) << L"\n";
                    client->Stop();
                    return 1;
                }

                hr = capture_client->GetNextPacketSize(&next_packet_frames);
                if (FAILED(hr)) {
                    std::wcout << L"GetNextPacketSize failed: " << hresult_text(hr) << L"\n";
                    client->Stop();
                    return 1;
                }
            }
        } else if (wait_result == WAIT_FAILED) {
            std::wcout << L"WaitForSingleObject failed: " << GetLastError() << L"\n";
            client->Stop();
            return 1;
        }

        QueryPerformanceCounter(&now);
        const double after_wait_elapsed_ms = qpc_elapsed_ms(start_time, now, frequency);
        const int current_second = static_cast<int>(after_wait_elapsed_ms / 1000.0);
        if (should_print_progress_second(current_second, last_report_second, seconds)) {
            last_report_second = current_second;
            print_udp_send_progress(after_wait_elapsed_ms, stats);
        }
    }

    hr = client->Stop();
    if (FAILED(hr)) {
        std::wcout << L"Stop failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    print_udp_send_summary(stats);
    return stats.send_errors == 0 ? 0 : 1;
}

int run_room_send_test_in_thread_context(
    std::vector<std::unique_ptr<RoomPeerRuntime>>& peers,
    const std::wstring& bind_address,
    int seconds,
    double input_gain,
    ERole capture_role,
    const std::optional<std::wstring>& capture_device_selector,
    TelemetrySnapshot* telemetry_snapshot = nullptr,
    const std::atomic_bool* input_muted = nullptr,
    const std::atomic_bool* stop_signal = nullptr) {
    const ComRuntime com;
    if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE) {
        std::wcout << L"Room sender thread CoInitializeEx failed: " << hresult_text(com.hr) << L"\n";
        return 1;
    }

    ComPtr<IMMDeviceEnumerator> sender_enumerator;
    const HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&sender_enumerator));
    if (FAILED(hr)) {
        std::wcout << L"Room sender thread MMDeviceEnumerator failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    return run_room_send_test(
        *sender_enumerator.Get(),
        peers,
        bind_address,
        seconds,
        input_gain,
        capture_role,
        capture_device_selector,
        telemetry_snapshot,
        input_muted,
        stop_signal);
}

int run_udp_audio_loopback_test(
    IMMDeviceEnumerator& enumerator,
    int seconds,
    std::uint16_t port,
    double input_gain,
    double output_gain,
    ERole capture_role,
    ERole render_role,
    const std::optional<std::wstring>& capture_device_selector,
    const std::optional<std::wstring>& render_device_selector,
    const SelfDuckingSettings& self_ducking) {
    std::wcout << L"== UDP localhost audio loopback test ==\n";
    std::wcout << L"Port: " << port << L", duration: " << seconds << L" seconds\n\n";
    std::wcout << L"Input gain: " << input_gain << L", output gain: " << output_gain << L"\n\n";
    std::wcout << L"Capture role: " << role_name(capture_role)
               << L", render role: " << role_name(render_role) << L"\n\n";
    if (self_ducking.enabled()) {
        std::wcout << L"Self ducking: " << self_ducking.duck_db << L" dB, hold="
                   << self_ducking.hold_ms << L" ms, attack=" << self_ducking.attack_ms
                   << L" ms, release=" << self_ducking.release_ms
                   << L" ms, threshold=" << self_ducking.threshold << L"\n\n";
    }

    int send_result = 0;
    SelfDuckingState self_ducking_state;
    std::thread sender([&send_result, port, seconds, input_gain, capture_role, capture_device_selector,
                        &self_ducking, &self_ducking_state]() {
        Sleep(400);
        send_result = run_udp_send_test_in_thread_context(
            L"127.0.0.1",
            port,
            seconds,
            input_gain,
            capture_role,
            capture_device_selector,
            self_ducking,
            &self_ducking_state);
    });

    const int play_result = run_udp_play_test(
        enumerator,
        port,
        seconds + 1,
        true,
        output_gain,
        render_role,
        render_device_selector,
        self_ducking,
        &self_ducking_state);
    sender.join();
    return play_result != 0 ? play_result : send_result;
}

int run_duplex_test(
    IMMDeviceEnumerator& enumerator,
    std::uint16_t local_port,
    const std::wstring& peer_host,
    std::uint16_t peer_port,
    int seconds,
    double input_gain,
    double output_gain,
    ERole capture_role,
    ERole render_role,
    const std::optional<std::wstring>& capture_device_selector,
    const std::optional<std::wstring>& render_device_selector,
    const SelfDuckingSettings& self_ducking) {
    std::wcout << L"== UDP duplex voice prototype ==\n";
    std::wcout << L"Local listen: 0.0.0.0:" << local_port << L"\n";
    std::wcout << L"Peer target:  " << peer_host << L":" << peer_port << L"\n";
    std::wcout << L"Duration:     " << duration_text(seconds) << L"\n\n";
    std::wcout << L"Input gain:   " << input_gain << L"\n";
    std::wcout << L"Output gain:  " << output_gain << L"\n\n";
    std::wcout << L"Capture role: " << role_name(capture_role) << L"\n";
    std::wcout << L"Render role:  " << role_name(render_role) << L"\n\n";
    if (self_ducking.enabled()) {
        std::wcout << L"Self ducking: " << self_ducking.duck_db << L" dB, hold="
                   << self_ducking.hold_ms << L" ms, attack=" << self_ducking.attack_ms
                   << L" ms, release=" << self_ducking.release_ms
                   << L" ms, threshold=" << self_ducking.threshold << L"\n\n";
    }

    int send_result = 0;
    std::atomic_bool duplex_stop{false};
    SelfDuckingState self_ducking_state;
    std::thread sender([&send_result, peer_host, peer_port, seconds, input_gain, capture_role, capture_device_selector,
                        &self_ducking, &self_ducking_state, &duplex_stop]() {
        Sleep(250);
        send_result = run_udp_send_test_in_thread_context(
            peer_host,
            peer_port,
            seconds,
            input_gain,
            capture_role,
            capture_device_selector,
            self_ducking,
            &self_ducking_state,
            &duplex_stop);
        duplex_stop = true;
    });

    const int play_result = run_udp_play_test(
        enumerator,
        local_port,
        seconds,
        false,
        output_gain,
        render_role,
        render_device_selector,
        self_ducking,
        &self_ducking_state,
        &duplex_stop);
    duplex_stop = true;
    sender.join();
    return play_result != 0 ? play_result : send_result;
}

void print_room_progress(
    double elapsed_ms,
    const std::vector<std::unique_ptr<RoomPeerRuntime>>& peers,
    UINT32 sample_rate) {
    std::wcout << L"  room t=" << elapsed_ms / 1000.0 << L"s";
    for (size_t index = 0; index < peers.size(); ++index) {
        const JitterStats jitter = peers[index]->jitter->snapshot();
        const std::uint64_t datagrams = peers[index]->debug_datagrams.load(std::memory_order_relaxed);
        const std::uint64_t valid_packets = peers[index]->debug_valid_packets.load(std::memory_order_relaxed);
        const std::uint64_t invalid_packets = peers[index]->debug_invalid_packets.load(std::memory_order_relaxed);
        std::wcout << L", p" << index << L"="
                   << frames_to_ms(static_cast<UINT32>(std::min<size_t>(jitter.current_frames, UINT32_MAX)), sample_rate)
                   << L"ms/u" << jitter.after_start_underrun_events
                   << L"/rx" << valid_packets
                   << L"/dg" << datagrams;
        if (invalid_packets != 0) {
            std::wcout << L"/bad" << invalid_packets;
        }
    }
    std::wcout << L"\n";
}

int run_room_test(
    IMMDeviceEnumerator& enumerator,
    std::uint16_t local_port,
    const std::wstring& bind_address,
    const std::vector<RoomPeerOptions>& peer_options,
    int seconds,
    double input_gain,
    double output_gain,
    ERole capture_role,
    ERole render_role,
    const std::optional<std::wstring>& capture_device_selector,
    const std::optional<std::wstring>& render_device_selector,
    bool listen_only,
    bool start_input_muted,
    HANDLE telemetry_handle,
    HANDLE control_handle) {
    if (peer_options.empty()) {
        std::wcout << L"--room requires at least one --peer\n";
        return 1;
    }

    std::wcout << L"== UDP room voice prototype ==\n";
    std::wcout << L"Mode:         " << (listen_only ? L"listen-only" : L"duplex") << L"\n";
    std::wcout << L"Local listen: " << bind_address << L":" << local_port << L"\n";
    std::wcout << L"Contacts:     " << peer_options.size() << L"\n";
    std::wcout << L"Duration:     " << duration_text(seconds) << L"\n\n";
    if (!listen_only) {
        std::wcout << L"Outgoing input gain: " << input_gain << L"\n";
        std::wcout << L"Outgoing input muted: " << (start_input_muted ? L"yes" : L"no") << L"\n";
    }
    std::wcout << L"Output gain:         " << output_gain << L"\n\n";
    if (!listen_only) {
        std::wcout << L"Capture role: " << role_name(capture_role) << L"\n";
    }
    std::wcout << L"Render role:  " << role_name(render_role) << L"\n\n";
    TelemetryWriter telemetry{telemetry_handle};

    ComPtr<IMMDevice> device;
    if (!select_audio_endpoint(enumerator, eRender, render_role, render_device_selector, L"render", device)) {
        return 1;
    }

    const std::wstring render_device_name = get_friendly_name(*device.Get());
    std::wcout << L"Render device: " << render_device_name << L"\n";

    ComPtr<IAudioClient> format_client;
    HRESULT hr = activate_audio_client(*device.Get(), format_client);
    if (FAILED(hr)) {
        std::wcout << L"Activate render IAudioClient failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    WAVEFORMATEX* raw_mix_format = nullptr;
    hr = format_client->GetMixFormat(&raw_mix_format);
    if (FAILED(hr)) {
        std::wcout << L"Render GetMixFormat failed: " << hresult_text(hr) << L"\n";
        return 1;
    }
    WaveFormatPtr mix_format(raw_mix_format);
    print_wave_format(*mix_format, L"  ");

    if (sample_kind(*mix_format) == SampleKind::unknown) {
        std::wcout << L"Unsupported render mix format for room prototype\n";
        return 1;
    }

    ComPtr<IAudioClient> client;
    bool used_audio_client3 = false;
    UINT32 requested_period_frames = 0;
    hr = initialize_shared_capture_client(
        *device.Get(),
        *mix_format,
        client,
        used_audio_client3,
        requested_period_frames);
    if (FAILED(hr)) {
        std::wcout << L"Initialize shared render failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (event.value == nullptr) {
        std::wcout << L"CreateEvent failed: " << GetLastError() << L"\n";
        return 1;
    }

    hr = client->SetEventHandle(event.value);
    if (FAILED(hr)) {
        std::wcout << L"Render SetEventHandle failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    UINT32 buffer_frames = 0;
    hr = client->GetBufferSize(&buffer_frames);
    if (FAILED(hr)) {
        std::wcout << L"Render GetBufferSize failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    REFERENCE_TIME stream_latency = 0;
    const HRESULT stream_latency_result = client->GetStreamLatency(&stream_latency);

    ComPtr<IAudioRenderClient> render_client;
    hr = client->GetService(IID_PPV_ARGS(&render_client));
    if (FAILED(hr)) {
        std::wcout << L"GetService(IAudioRenderClient) failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    const UINT32 sample_rate = mix_format->nSamplesPerSec;
    const size_t max_jitter_frames = static_cast<size_t>(sample_rate / 2);
    std::vector<std::unique_ptr<RoomPeerRuntime>> peers;
    if (!prepare_room_peers(peer_options, sample_rate, max_jitter_frames, peers)) {
        return 1;
    }

    for (size_t index = 0; index < peers.size(); ++index) {
        const RoomPeerRuntime& peer = *peers[index];
        std::wcout << L"Contact [" << index << L"]: " << peer.options.host << L":" << peer.options.port
                   << L", ip=" << peer.ip_key
                   << L", gain=" << peer.options.gain
                   << L", duck-db=" << peer.options.self_duck_db
                   << L", threshold=" << peer.options.self_duck_threshold
                   << L", receive-buffer=" << peer.options.receive_buffer_ms << L" ms"
                   << L", global-ptt=" << (peer.options.global_ptt_enabled ? L"on" : L"off") << L"\n";
    }

    UdpPlayStats stats;
    RoomMixer room_mixer;
    room_mixer.reserve(buffer_frames);
    TelemetrySnapshot telemetry_snapshot(peers.size());
    telemetry_snapshot.set_audio_endpoint_diagnostics(
        lanspeak::core::AudioEndpointKind::render,
        lanspeak::core::AudioEndpointDiagnostics{
            true,
            lanspeak::common::wide_to_utf8(render_device_name),
            mix_format->nSamplesPerSec,
            mix_format->nChannels,
            mix_format->wBitsPerSample,
            requested_period_frames != 0
                ? frames_to_ms(requested_period_frames, mix_format->nSamplesPerSec)
                : -1.0,
            frames_to_ms(buffer_frames, mix_format->nSamplesPerSec),
            SUCCEEDED(stream_latency_result) ? reference_time_to_ms(stream_latency) : -1.0,
            used_audio_client3});
    std::atomic_bool stop{false};
    std::atomic_bool input_muted{start_input_muted};
    std::atomic_bool receiver_ready{false};
    std::atomic_bool receiver_failed{false};
    std::atomic_bool presence_enabled{false};
    int send_result = 0;
    std::thread sender;
    std::thread control_reader;
    std::thread telemetry_thread;

    std::thread receiver([&]() {
        receive_room_udp_audio(
            local_port,
            bind_address,
            sample_rate,
            peers,
            telemetry_snapshot,
            presence_enabled,
            stop,
            receiver_ready,
            receiver_failed);
    });
    if (control_handle != nullptr && control_handle != INVALID_HANDLE_VALUE) {
        control_reader = std::thread([&]() {
            read_room_control_commands(control_handle, peers, stop, &input_muted);
        });
    }
    if (telemetry.enabled()) {
        telemetry_thread = std::thread([&]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            while (!stop.load(std::memory_order_relaxed)) {
                write_room_telemetry(telemetry, telemetry_snapshot);
                Sleep(static_cast<DWORD>(kRoomTelemetryIntervalMs));
            }
        });
    }

    auto stop_and_join_room_threads = [&]() {
        stop = true;
        if (sender.joinable()) {
            sender.join();
        }
        if (receiver.joinable()) {
            receiver.join();
        }
        if (control_reader.joinable()) {
            control_reader.join();
        }
        if (telemetry_thread.joinable()) {
            telemetry_thread.join();
        }
    };

    for (int attempt = 0; attempt < 100 && !receiver_ready.load(); ++attempt) {
        Sleep(10);
    }

    if (!receiver_ready.load() || receiver_failed.load()) {
        stop_and_join_room_threads();
        std::wcout << L"Room UDP receiver failed to start\n";
        return 1;
    }

    if (!listen_only) {
        sender = std::thread([&]() {
            Sleep(250);
            send_result = run_room_send_test_in_thread_context(
                peers,
                bind_address,
                seconds,
                input_gain,
                capture_role,
                capture_device_selector,
                &telemetry_snapshot,
                &input_muted,
                &stop);
            stop = true;
        });
    }

    std::wcout << L"Initialized render: "
               << (used_audio_client3 ? L"IAudioClient3 min-period shared stream" : L"IAudioClient shared stream")
               << L"\n";
    if (requested_period_frames != 0) {
        std::wcout << L"Requested render period: " << requested_period_frames << L" frames, "
                   << frames_to_ms(requested_period_frames, sample_rate) << L" ms\n";
    }
    std::wcout << L"Render buffer: " << buffer_frames << L" frames, "
               << frames_to_ms(buffer_frames, sample_rate) << L" ms\n";
    std::wcout << L"Receive buffers: per contact\n";

    const UINT32 prefill_frames = std::min(
        buffer_frames,
        requested_period_frames != 0 ? requested_period_frames : std::max<UINT32>(1, sample_rate / 100));
    BYTE* prefill = nullptr;
    hr = render_client->GetBuffer(prefill_frames, &prefill);
    if (FAILED(hr)) {
        stop_and_join_room_threads();
        std::wcout << L"Render prefill GetBuffer failed: " << hresult_text(hr) << L"\n";
        return 1;
    }
    std::memset(prefill, 0, static_cast<size_t>(prefill_frames) * mix_format->nBlockAlign);
    hr = render_client->ReleaseBuffer(prefill_frames, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) {
        stop_and_join_room_threads();
        std::wcout << L"Render prefill ReleaseBuffer failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    MmcssTask mmcss(L"Audio");
    if (mmcss.value == nullptr) {
        std::wcout << L"MMCSS Audio priority: unavailable, GetLastError=" << GetLastError() << L"\n";
    } else {
        std::wcout << L"MMCSS Audio priority: enabled\n";
    }

    LARGE_INTEGER frequency{};
    LARGE_INTEGER start_time{};
    LARGE_INTEGER previous_render_event{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start_time);
    bool have_previous_render_event = false;
    int last_report_second = 0;
    const double duration_ms = static_cast<double>(seconds) * 1000.0;

    hr = client->Start();
    if (FAILED(hr)) {
        stop_and_join_room_threads();
        std::wcout << L"Render Start failed: " << hresult_text(hr) << L"\n";
        return 1;
    }
    presence_enabled.store(true, std::memory_order_relaxed);

    int play_result = 0;
    while (true) {
        if (stop.load(std::memory_order_relaxed)) {
            break;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed_ms = qpc_elapsed_ms(start_time, now, frequency);
        if (should_stop_for_duration(seconds, duration_ms, elapsed_ms)) {
            break;
        }

        const DWORD wait_ms = wait_ms_for_duration(seconds, duration_ms, elapsed_ms);
        const DWORD wait_result = WaitForSingleObject(event.value, wait_ms);
        if (wait_result == WAIT_TIMEOUT) {
            ++stats.render_wait_timeouts;
        } else if (wait_result == WAIT_OBJECT_0) {
            LARGE_INTEGER render_event_time{};
            QueryPerformanceCounter(&render_event_time);
            if (have_previous_render_event) {
                stats.render_event_intervals.add(qpc_elapsed_ms(previous_render_event, render_event_time, frequency));
            }
            previous_render_event = render_event_time;
            have_previous_render_event = true;

            UINT32 padding = 0;
            hr = client->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                std::wcout << L"Render GetCurrentPadding failed: " << hresult_text(hr) << L"\n";
                play_result = 1;
                break;
            }
            stats.render_padding_min = std::min<std::uint64_t>(stats.render_padding_min, padding);
            stats.render_padding_max = std::max<std::uint64_t>(stats.render_padding_max, padding);
            telemetry_snapshot.update_render_padding_ms(frames_to_ms(padding, sample_rate));

            const UINT32 available_frames = buffer_frames > padding ? buffer_frames - padding : 0;
            if (available_frames > 0) {
                BYTE* render_data = nullptr;
                hr = render_client->GetBuffer(available_frames, &render_data);
                if (FAILED(hr)) {
                    std::wcout << L"Render GetBuffer failed: " << hresult_text(hr) << L"\n";
                    play_result = 1;
                    break;
                }

                const double buffer_ms = frames_to_ms(available_frames, sample_rate);
                if (!fill_render_buffer_from_room_peers(
                        render_data,
                        available_frames,
                        *mix_format,
                        peers,
                        output_gain,
                        render_event_time,
                        buffer_ms,
                        stats,
                        room_mixer,
                        telemetry_snapshot)) {
                    render_client->ReleaseBuffer(available_frames, AUDCLNT_BUFFERFLAGS_SILENT);
                    std::wcout << L"Failed to fill room render buffer\n";
                    play_result = 1;
                    break;
                }

                hr = render_client->ReleaseBuffer(available_frames, 0);
                if (FAILED(hr)) {
                    std::wcout << L"Render ReleaseBuffer failed: " << hresult_text(hr) << L"\n";
                    play_result = 1;
                    break;
                }
            }
        } else if (wait_result == WAIT_FAILED) {
            ++stats.render_wait_failures;
            std::wcout << L"Render WaitForSingleObject failed: " << GetLastError() << L"\n";
            play_result = 1;
            break;
        }

        QueryPerformanceCounter(&now);
        const double after_wait_elapsed_ms = qpc_elapsed_ms(start_time, now, frequency);
        const int current_second = static_cast<int>(after_wait_elapsed_ms / 1000.0);
        if (!listen_only && should_print_progress_second(current_second, last_report_second, seconds)) {
            last_report_second = current_second;
            print_room_progress(after_wait_elapsed_ms, peers, sample_rate);
        }
    }

    hr = client->Stop();
    stop_and_join_room_threads();

    if (FAILED(hr)) {
        std::wcout << L"Render Stop failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    std::wcout << L"\nRoom summary:\n";
    for (size_t index = 0; index < peers.size(); ++index) {
        std::wcout << L"\nContact [" << index << L"] " << peers[index]->options.host << L":"
                   << peers[index]->options.port << L"\n";
        print_udp_receive_summary(peers[index]->receive);
        print_udp_play_summary(peers[index]->jitter->snapshot(), stats, sample_rate);
    }

    return play_result != 0 ? play_result : send_result;
}

void probe_device(IMMDevice& device, const std::vector<std::wstring>& markers, const ProbeOptions& options) {
    DWORD state = 0;
    HRESULT hr = device.GetState(&state);
    if (FAILED(hr)) {
        std::wcout << L"  state: GetState failed: " << hresult_text(hr) << L"\n";
    }

    const std::optional<EDataFlow> flow = get_device_flow(device);
    std::wcout << L"  flow: " << (flow ? flow_name(*flow) : L"unknown") << L"\n";
    std::wcout << L"  state: " << state_name(state) << L" (0x" << std::hex << std::uppercase << state
               << std::dec << L")\n";

    if (!markers.empty()) {
        std::wcout << L"  default roles:";
        for (const std::wstring& marker : markers) {
            std::wcout << L" " << marker;
        }
        std::wcout << L"\n";
    }

    std::wcout << L"  id: " << get_device_id(device) << L"\n";

    ComPtr<IAudioClient> client;
    hr = activate_audio_client(device, client);
    if (FAILED(hr)) {
        std::wcout << L"  Activate(IAudioClient): failed: " << hresult_text(hr) << L"\n";
        return;
    }

    print_basic_periods(*client.Get());

    WAVEFORMATEX* raw_mix_format = nullptr;
    hr = client->GetMixFormat(&raw_mix_format);
    if (FAILED(hr)) {
        std::wcout << L"  mix format: failed: " << hresult_text(hr) << L"\n";
        return;
    }
    WaveFormatPtr mix_format(raw_mix_format);
    print_wave_format(*mix_format, L"  ");

    probe_shared_event_init(device, *mix_format);
    print_iaudioclient3_periods(device, *mix_format);
    if (options.include_exclusive_checks) {
        probe_exclusive_event_init(device, *mix_format);
    } else {
        std::wcout << L"  exclusive checks: skipped (use --exclusive to enable)\n";
    }
}

void collect_default_marker(
    IMMDeviceEnumerator& enumerator,
    std::map<std::wstring, std::vector<std::wstring>>& markers,
    EDataFlow flow,
    ERole role,
    bool print = true) {
    ComPtr<IMMDevice> device;
    const HRESULT hr = enumerator.GetDefaultAudioEndpoint(flow, role, &device);
    if (FAILED(hr)) {
        if (print) {
            std::wcout << L"Default " << flow_name(flow) << L" " << role_name(role)
                       << L": unavailable: " << hresult_text(hr) << L"\n";
        }
        return;
    }

    const std::wstring id = get_device_id(*device.Get());
    markers[id].push_back(flow_name(flow) + L":" + role_name(role));
    if (print) {
        std::wcout << L"Default " << flow_name(flow) << L" " << role_name(role)
                   << L": " << get_friendly_name(*device.Get()) << L"\n";
    }
}

void print_default_devices(IMMDeviceEnumerator& enumerator, std::map<std::wstring, std::vector<std::wstring>>& markers) {
    std::wcout << L"== Default endpoints ==\n";
    collect_default_marker(enumerator, markers, eRender, eConsole);
    collect_default_marker(enumerator, markers, eRender, eMultimedia);
    collect_default_marker(enumerator, markers, eRender, eCommunications);
    collect_default_marker(enumerator, markers, eCapture, eConsole);
    collect_default_marker(enumerator, markers, eCapture, eMultimedia);
    collect_default_marker(enumerator, markers, eCapture, eCommunications);
    std::wcout << L"\n";
}

std::wstring escape_device_list_field(const std::wstring& value) {
    std::wstring escaped;
    escaped.reserve(value.size());
    for (const wchar_t ch : value) {
        switch (ch) {
        case L'\\':
            escaped += L"\\\\";
            break;
        case L'\t':
            escaped += L"\\t";
            break;
        case L'\r':
            escaped += L"\\r";
            break;
        case L'\n':
            escaped += L"\\n";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::wstring join_markers(const std::vector<std::wstring>& markers) {
    std::wstring joined;
    for (const std::wstring& marker : markers) {
        if (!joined.empty()) {
            joined += L", ";
        }
        joined += marker;
    }
    return joined;
}

void collect_default_markers_quiet(
    IMMDeviceEnumerator& enumerator,
    std::map<std::wstring, std::vector<std::wstring>>& markers) {
    collect_default_marker(enumerator, markers, eRender, eConsole, false);
    collect_default_marker(enumerator, markers, eRender, eMultimedia, false);
    collect_default_marker(enumerator, markers, eRender, eCommunications, false);
    collect_default_marker(enumerator, markers, eCapture, eConsole, false);
    collect_default_marker(enumerator, markers, eCapture, eMultimedia, false);
    collect_default_marker(enumerator, markers, eCapture, eCommunications, false);
}

int print_device_list(IMMDeviceEnumerator& enumerator) {
    std::map<std::wstring, std::vector<std::wstring>> default_markers;
    collect_default_markers_quiet(enumerator, default_markers);

    std::vector<EndpointCandidate> endpoints;
    if (!collect_active_endpoints(enumerator, endpoints)) {
        return 1;
    }

    std::wcout << L"LANSPK_DEVICE_LIST\t1\n";
    for (const EndpointCandidate& endpoint : endpoints) {
        const auto marker_it = default_markers.find(endpoint.id);
        const std::wstring markers =
            marker_it == default_markers.end() ? L"" : join_markers(marker_it->second);
        std::wcout << L"DEVICE\t"
                   << flow_name(endpoint.flow) << L"\t"
                   << endpoint.index << L"\t"
                   << escape_device_list_field(endpoint.name) << L"\t"
                   << escape_device_list_field(endpoint.id) << L"\t"
                   << escape_device_list_field(markers) << L"\n";
    }
    std::wcout << L"END_DEVICE_LIST\n";
    return 0;
}

void enumerate_active_endpoints(IMMDeviceEnumerator& enumerator, const ProbeOptions& options) {
    std::map<std::wstring, std::vector<std::wstring>> default_markers;
    print_default_devices(enumerator, default_markers);

    ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = enumerator.EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        std::wcout << L"EnumAudioEndpoints failed: " << hresult_text(hr) << L"\n";
        return;
    }

    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        std::wcout << L"IMMDeviceCollection::GetCount failed: " << hresult_text(hr) << L"\n";
        return;
    }

    std::wcout << L"== Active endpoints ==\n";
    std::wcout << L"Count: " << count << L"\n\n";

    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        hr = collection->Item(index, &device);
        if (FAILED(hr)) {
            std::wcout << L"[" << index << L"] Item failed: " << hresult_text(hr) << L"\n\n";
            continue;
        }

        const std::wstring id = get_device_id(*device.Get());
        const auto marker_it = default_markers.find(id);
        const std::vector<std::wstring> markers =
            marker_it == default_markers.end() ? std::vector<std::wstring>{} : marker_it->second;

        std::wcout << L"[" << index << L"] " << get_friendly_name(*device.Get()) << L"\n";
        probe_device(*device.Get(), markers, options);
        std::wcout << L"\n";
    }
}

} // namespace

int lanspeak::core::run_application(int argc, wchar_t* argv[]) {
    try {
        std::wcout.imbue(std::locale(std::locale(""), new NoGroupingNumpunct()));
    } catch (...) {
        // Keep diagnostics running even if the user's console locale is unusual.
    }

    SetConsoleOutputCP(CP_UTF8);
    std::wcout << std::unitbuf;

    const ProbeOptions options = parse_options(argc, argv);
    if (options.show_help) {
        print_usage();
        return 0;
    }

    if (options.mode != ProbeOptions::Mode::list_devices) {
        std::wcout << L"LanSpeak WASAPI probe\n";
        std::wcout << L"Process id: " << GetCurrentProcessId() << L"\n\n";
        std::wcout << L"Mode: shared diagnostics";
        if (options.include_exclusive_checks) {
            std::wcout << L" + exclusive diagnostics";
        } else {
            std::wcout << L" (exclusive checks disabled)";
        }
        std::wcout << L"\n\n";
    }

    const ComRuntime com;
    if (FAILED(com.hr)) {
        if (com.hr == RPC_E_CHANGED_MODE) {
            std::wcout << L"COM already initialized in another apartment mode; continuing.\n";
        } else {
            std::wcout << L"CoInitializeEx failed: " << hresult_text(com.hr) << L"\n";
            return 1;
        }
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    const HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));

    if (FAILED(hr)) {
        std::wcout << L"CoCreateInstance(MMDeviceEnumerator) failed: " << hresult_text(hr) << L"\n";
        return 1;
    }

    const SelfDuckingSettings self_ducking{
        options.self_duck_db,
        options.self_duck_hold_ms,
        options.self_duck_attack_ms,
        options.self_duck_release_ms,
        options.self_duck_threshold};

    if (options.mode == ProbeOptions::Mode::list_devices) {
        return print_device_list(*enumerator.Get());
    }

    if (options.mode == ProbeOptions::Mode::capture_test) {
        return run_capture_timing_test(
            *enumerator.Get(),
            options.capture_test_seconds,
            options.capture_role,
            options.capture_device_selector);
    }

    if (options.mode == ProbeOptions::Mode::tone_test) {
        return run_tone_render_test(
            *enumerator.Get(),
            options.udp_seconds,
            options.output_gain,
            options.tone_frequency,
            options.render_role,
            options.render_device_selector);
    }

    if (options.mode == ProbeOptions::Mode::udp_recv) {
        return run_udp_receive_test(options.udp_port, options.udp_seconds, false, false);
    }

    if (options.mode == ProbeOptions::Mode::udp_send) {
        return run_udp_send_test(
            *enumerator.Get(),
            options.udp_host,
            options.udp_port,
            options.udp_seconds,
            options.input_gain,
            options.capture_role,
            options.capture_device_selector,
            self_ducking,
            nullptr);
    }

    if (options.mode == ProbeOptions::Mode::udp_loopback) {
        return run_udp_loopback_test(
            *enumerator.Get(),
            options.udp_seconds,
            options.udp_port,
            options.input_gain,
            options.capture_role,
            options.capture_device_selector,
            self_ducking);
    }

    if (options.mode == ProbeOptions::Mode::udp_play) {
        return run_udp_play_test(
            *enumerator.Get(),
            options.udp_port,
            options.udp_seconds,
            false,
            options.output_gain,
            options.render_role,
            options.render_device_selector,
            self_ducking,
            nullptr);
    }

    if (options.mode == ProbeOptions::Mode::udp_audio_loopback) {
        return run_udp_audio_loopback_test(
            *enumerator.Get(),
            options.udp_seconds,
            options.udp_port,
            options.input_gain,
            options.output_gain,
            options.capture_role,
            options.render_role,
            options.capture_device_selector,
            options.render_device_selector,
            self_ducking);
    }

    if (options.mode == ProbeOptions::Mode::duplex) {
        return run_duplex_test(
            *enumerator.Get(),
            options.local_port,
            options.peer_host,
            options.peer_port,
            options.udp_seconds,
            options.input_gain,
            options.output_gain,
            options.capture_role,
            options.render_role,
            options.capture_device_selector,
            options.render_device_selector,
            self_ducking);
    }

    if (options.mode == ProbeOptions::Mode::room) {
        return run_room_test(
            *enumerator.Get(),
            options.local_port,
            options.bind_address,
            options.room_peers,
            options.udp_seconds,
            options.input_gain,
            options.output_gain,
            options.capture_role,
            options.render_role,
            options.capture_device_selector,
            options.render_device_selector,
            options.room_listen_only,
            options.input_muted,
            reinterpret_cast<HANDLE>(options.telemetry_handle_value),
            reinterpret_cast<HANDLE>(options.control_handle_value));
    }

    enumerate_active_endpoints(*enumerator.Get(), options);
    return 0;
}
