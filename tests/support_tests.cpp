#include "common/line_buffer.h"
#include "core/audio_math.h"
#include "core/jitter_buffer.h"
#include "core/pcm_audio.h"
#include "core/peer_router.h"
#include "core/room_mixer.h"
#include "core/room_session.h"
#include "core/telemetry_snapshot.h"
#include "core/udp_audio_packet.h"
#include "core/wasapi_audio.h"
#include "gui/contact_list_model.h"
#include "gui/vu_math.h"
#include "gui/gdi_object_cache.h"
#include "gui/hotkey_utils.h"
#include "gui/localization.h"
#include "gui/settings_store.h"
#include "gui/telemetry_protocol.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <malloc.h>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::atomic<std::size_t> allocation_count{0};
thread_local bool track_allocations = false;

void record_allocation() {
    if (track_allocations) {
        allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

void* operator new(std::size_t size) {
    record_allocation();
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    ::operator delete(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    ::operator delete(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    ::operator delete(memory);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    record_allocation();
    if (void* memory = _aligned_malloc(size == 0 ? 1 : size, static_cast<std::size_t>(alignment))) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void* memory, std::align_val_t) noexcept {
    _aligned_free(memory);
}

void operator delete[](void* memory, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}

void operator delete(void* memory, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}

void operator delete[](void* memory, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}

namespace {

class AllocationScope {
public:
    AllocationScope() : start_(allocation_count.load(std::memory_order_relaxed)) {
        track_allocations = true;
    }

    ~AllocationScope() {
        track_allocations = false;
    }

    [[nodiscard]] std::size_t count() const {
        return allocation_count.load(std::memory_order_relaxed) - start_;
    }

private:
    std::size_t start_ = 0;
};

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error(
            std::string("check failed at line ") + std::to_string(line) + ": " + expression);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

bool close_to(double left, double right, double tolerance = 1e-6) {
    return std::abs(left - right) <= tolerance;
}

void test_udp_packet_validation() {
    using namespace lanspeak::core;
    UdpAudioPacketHeader source{};
    source.sample_rate = 48000;
    source.frames = 4;
    source.payload_bytes = 8;
    std::array<std::byte, sizeof(source) + 8> datagram{};
    std::memcpy(datagram.data(), &source, sizeof(source));

    UdpAudioPacketHeader parsed{};
    CHECK(validate_udp_audio_packet(datagram.data(), datagram.size(), 48000, parsed));
    CHECK(parsed.frames == 4);

    auto invalid = datagram;
    invalid[0] = std::byte{0};
    CHECK(!validate_udp_audio_packet(invalid.data(), invalid.size(), 48000, parsed));

    source.payload_bytes = 7;
    std::memcpy(invalid.data(), &source, sizeof(source));
    CHECK(!validate_udp_audio_packet(invalid.data(), invalid.size(), 48000, parsed));
}

void test_pcm_conversion_and_resampling() {
    using namespace lanspeak::core;
    CHECK(float_to_pcm16(2.0) == 32767);
    CHECK(float_to_pcm16(-2.0) == -32768);
    CHECK(float_to_pcm16(0.0) == 0);

    std::array<std::byte, 6> bytes{};
    write_int16_le(bytes.data(), -32768);
    write_int16_le(bytes.data() + 2, 0);
    write_int16_le(bytes.data() + 4, 32767);
    std::vector<std::int16_t> decoded;
    CHECK(decode_mono_pcm16_le(bytes, decoded));
    CHECK((decoded == std::vector<std::int16_t>{-32768, 0, 32767}));

    std::vector<std::int16_t> source(441);
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<std::int16_t>(index * 20 - 4000);
    }
    std::vector<std::int16_t> resampled;
    CHECK(resample_mono_pcm16_linear(source, 44100, 48000, resampled));
    CHECK(resampled.size() == 480);
    CHECK(resampled.front() == source.front());
}

void test_wasapi_pcm_payload_and_render_conversion() {
    using namespace lanspeak::core;
    WAVEFORMATEX float_stereo{};
    float_stereo.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    float_stereo.nChannels = 2;
    float_stereo.nSamplesPerSec = 48000;
    float_stereo.wBitsPerSample = 32;
    float_stereo.nBlockAlign = 8;
    float_stereo.nAvgBytesPerSec = 384000;
    CHECK(sample_kind(float_stereo) == SampleKind::float32);

    const std::array<float, 6> stereo{
        2.0f, 2.0f,
        -2.0f, -2.0f,
        0.75f, -0.25f};
    std::vector<BYTE> payload;
    payload.reserve(6);
    CHECK(build_mono_pcm16_payload(
        reinterpret_cast<const BYTE*>(stereo.data()),
        3,
        float_stereo,
        false,
        1.0,
        payload));
    CHECK(payload.size() == 6);
    CHECK(read_int16_le(reinterpret_cast<const std::byte*>(payload.data())) == 32767);
    CHECK(read_int16_le(reinterpret_cast<const std::byte*>(payload.data() + 2)) == -32768);
    CHECK(read_int16_le(reinterpret_cast<const std::byte*>(payload.data() + 4)) == 8192);

    const std::array<double, 2> mixed{2.0, -2.0};
    std::array<float, 4> rendered{};
    CHECK(write_room_mix_to_render_buffer(
        reinterpret_cast<BYTE*>(rendered.data()),
        2,
        float_stereo,
        mixed));
    CHECK(rendered[0] == 1.0f && rendered[1] == 1.0f);
    CHECK(rendered[2] == -1.0f && rendered[3] == -1.0f);
}

void test_jitter_in_order_and_reordering() {
    using lanspeak::core::JitterBuffer;
    JitterBuffer jitter(2, 16);
    const std::array<std::int16_t, 2> first{1, 2};
    const std::array<std::int16_t, 2> second{3, 4};
    const std::array<std::int16_t, 2> third{5, 6};
    jitter.push(10, first);
    jitter.push(12, third);
    jitter.push(11, second);

    std::array<std::int16_t, 6> output{};
    jitter.pop_into(output);
    CHECK((output == std::array<std::int16_t, 6>{1, 2, 3, 4, 5, 6}));
    CHECK(jitter.snapshot().sequence_gaps == 0);

    jitter.push(11, second);
    CHECK(jitter.snapshot().late_or_duplicate_packets == 1);

    JitterBuffer pcm_jitter(2, 16);
    const std::array<std::byte, 4> pcm{
        std::byte{0x34}, std::byte{0x12}, std::byte{0xcc}, std::byte{0xff}};
    CHECK(pcm_jitter.push_pcm16_le(0, pcm));
    std::array<std::int16_t, 2> pcm_output{};
    pcm_jitter.pop_into(pcm_output);
    CHECK(pcm_output[0] == 0x1234);
    CHECK(pcm_output[1] == -52);
}

void test_jitter_gap_overflow_and_underrun() {
    using lanspeak::core::JitterBuffer;
    JitterBuffer jitter(1, 4);
    const std::array<std::int16_t, 3> first{1, 2, 3};
    const std::array<std::int16_t, 3> second{4, 5, 6};
    jitter.push(0, first);
    jitter.push(1, second);

    std::array<std::int16_t, 4> output{};
    jitter.pop_into(output);
    CHECK((output == std::array<std::int16_t, 4>{3, 4, 5, 6}));
    CHECK(jitter.snapshot().overflow_dropped_frames == 2);

    std::array<std::int16_t, 2> silence{7, 7};
    jitter.pop_into(silence);
    CHECK((silence == std::array<std::int16_t, 2>{0, 0}));
    CHECK(jitter.snapshot().after_start_underrun_frames == 2);

    jitter.push(500, first);
    CHECK(jitter.snapshot().sequence_restarts == 1);
}

void test_peer_routing() {
    using namespace lanspeak::core;
    std::array<PeerRouteState, 3> peers{{
        {true, false, 0},
        {false, true, 0},
        {false, false, 0},
    }};
    CHECK(count_voice_targets(false, peers) == 2);
    CHECK(count_voice_targets(true, peers) == 1);
    CHECK(should_send_voice_to_peer(true, peers[1]));
    CHECK(!should_send_voice_to_peer(true, peers[0]));

    PeerRouter router(peers.size());
    router.begin(true);
    for (std::size_t index = 0; index < peers.size(); ++index) {
        router.consider(index, peers[index]);
    }
    CHECK(router.targets().size() == 1);
    CHECK(router.targets()[0] == 1);

    router.begin(false);
    for (std::size_t index = 0; index < peers.size(); ++index) {
        router.consider(index, peers[index]);
    }
    CHECK(router.targets().size() == 2);
    CHECK(router.targets()[0] == 0);
    CHECK(router.targets()[1] == 1);
}

void test_room_mixer_reuses_storage() {
    lanspeak::core::RoomMixer mixer;
    mixer.begin(480);
    auto mono = mixer.mono_scratch();
    auto mixed = mixer.mixed_frames();
    CHECK(mono.size() == 480);
    CHECK(mixed.size() == 480);
    mixed[0] = 1.0;
    mixer.begin(480);
    CHECK(mixer.mixed_frames()[0] == 0.0);
}

void test_hot_paths_do_not_allocate_after_warmup() {
    using namespace lanspeak::core;

    JitterBuffer jitter(1, 2048);
    std::array<std::byte, 960> packet{};
    std::array<std::int16_t, 480> output{};
    CHECK(jitter.push_pcm16_le(0, packet));
    jitter.pop_into(output);

    RoomMixer mixer;
    mixer.reserve(480);
    mixer.begin(480);

    std::vector<std::int16_t> decoded;
    std::vector<std::int16_t> resampled;
    decoded.reserve(480);
    resampled.reserve(523);
    CHECK(decode_mono_pcm16_le(packet, decoded));
    CHECK(resample_mono_pcm16_linear(decoded, 44100, 48000, resampled));

    std::array<PeerRouteState, 2> peers{{{true, false, 0}, {false, true, 0}}};
    PeerRouter router(peers.size());
    router.begin(false);
    router.consider(0, peers[0]);
    router.consider(1, peers[1]);

    TelemetrySnapshot telemetry(2);

    std::size_t allocations = 0;
    {
        AllocationScope scope;
        CHECK(jitter.push_pcm16_le(1, packet));
        jitter.pop_into(output);
        mixer.begin(480);
        CHECK(decode_mono_pcm16_le(packet, decoded));
        CHECK(resample_mono_pcm16_linear(decoded, 44100, 48000, resampled));
        router.begin(true);
        router.consider(0, peers[0]);
        router.consider(1, peers[1]);
        telemetry.accumulate_local_peak(0.25);
        telemetry.update_peer_meter(0, -18.0, true);
        telemetry.mark_peer_stream(0, 1000);
        allocations = scope.count();
    }
    CHECK(allocations == 0);
}

void test_core_telemetry_snapshot() {
    lanspeak::core::TelemetrySnapshot source(2);
    source.accumulate_local_peak(0.1);
    source.update_peer_meter(0, -12.5, true);
    source.mark_peer_stream(0, 1000);
    source.update_peer_meter(1, -60.0, false);

    lanspeak::gui::TelemetryParser parser;
    CHECK(parser.append(source.serialize(1050, 200)));
    const auto first = parser.snapshot();
    CHECK(first.local.valid);
    CHECK(first.local.voice_active);
    CHECK(first.peers.size() == 2);
    CHECK(close_to(first.peers[0].level_db, -12.5));
    CHECK(first.peers[0].voice_active);
    CHECK(first.peers[0].stream_active);
    CHECK(!first.peers[1].stream_active);

    CHECK(parser.append(source.serialize(1300, 200)));
    const auto second = parser.snapshot();
    CHECK(!second.local.voice_active);
    CHECK(!second.peers[0].stream_active);
}

void test_line_buffer_fragmentation() {
    lanspeak::common::LineBuffer lines;
    std::string line;
    lines.append("local_level\t-20");
    CHECK(!lines.next(line));
    lines.append("\t1\r\npeer_");
    CHECK(lines.next(line));
    CHECK(line == "local_level\t-20\t1");
    CHECK(!lines.next(line));
    lines.append("level\t0\t-90\t0\n");
    CHECK(lines.next(line));
    CHECK(line == "peer_level\t0\t-90\t0");
    CHECK(lines.pending_bytes() == 0);
}

void test_fragmented_room_control_commands() {
    using namespace lanspeak::core;
    RoomPeerOptions options;
    options.gain = 1.0;
    options.global_ptt_enabled = true;
    RoomPeerControl first;
    RoomPeerControl second;
    initialize_room_peer_control(first, options);
    initialize_room_peer_control(second, options);
    std::array<RoomPeerControl*, 2> peers{&first, &second};
    std::atomic_bool input_muted{true};
    std::atomic_bool stop{false};
    lanspeak::common::LineBuffer lines;

    consume_room_control_bytes(
        "peer_settings 1 1.5 18 0.04 7",
        lines,
        peers,
        &input_muted,
        &stop);
    consume_room_control_bytes(
        " 90 160 0\npeer_talk 1 1\ninput_muted 0\nshut",
        lines,
        peers,
        &input_muted,
        &stop);
    CHECK(!stop.load());
    consume_room_control_bytes("down\n", lines, peers, &input_muted, &stop);

    const RoomPeerControlSnapshot updated = snapshot_room_peer_control(second);
    CHECK(close_to(updated.gain, 1.5));
    CHECK(close_to(updated.duck_db, 18.0));
    CHECK(close_to(updated.duck_threshold, 0.04));
    CHECK(updated.duck_attack_ms == 7);
    CHECK(updated.duck_hold_ms == 90);
    CHECK(updated.duck_release_ms == 160);
    CHECK(!updated.global_ptt_enabled);
    CHECK(updated.private_talk);
    CHECK(!input_muted.load());
    CHECK(stop.load());
}

void test_ducking_and_vu_math() {
    using namespace lanspeak::core;
    const double attacked = smooth_toward(1.0, 0.25, 8.0, 8);
    CHECK(attacked < 1.0 && attacked > 0.25);
    const double released = smooth_toward(attacked, 1.0, 120.0, 120);
    CHECK(released > attacked && released < 1.0);
    CHECK(close_to(decibels_to_gain(-12.0), std::pow(10.0, -12.0 / 20.0)));
    CHECK(audio_level_dbfs(1.0) == 0.0);

    CHECK(lanspeak::gui::vu_fill_ratio(-60.0) == 0.0);
    CHECK(lanspeak::gui::vu_fill_ratio(-30.0) == 0.5);
    CHECK(lanspeak::gui::vu_fill_ratio(0.0) == 1.0);
    std::uint64_t timestamp = 1000;
    const double attack = lanspeak::gui::smooth_vu_level(-40.0, -10.0, 1033, timestamp, 24.0);
    CHECK(attack == -10.0);
    const double release = lanspeak::gui::smooth_vu_level(attack, -60.0, 2033, timestamp, 24.0);
    CHECK(close_to(release, -34.0));
}

void test_contact_meter_bank() {
    lanspeak::gui::ContactMeterBank meters;
    meters.sync(2);
    CHECK(meters.size() == 2);
    CHECK(meters.update(0, -18.0, true, true, 1000, 24.0, 0.1));
    const auto* first = meters.get(0);
    CHECK(first != nullptr);
    CHECK(close_to(first->level_db, -18.0));
    CHECK(first->voice_active);
    CHECK(first->stream_active);
    CHECK(first->osd_last_active_ms == 1000);

    CHECK(meters.update(0, -60.0, false, false, 2000, 24.0, 0.1));
    first = meters.get(0);
    CHECK(first != nullptr);
    CHECK(close_to(first->level_db, -42.0));
    CHECK(first->osd_last_active_ms == 1000);

    meters.append();
    CHECK(meters.size() == 3);
    meters.erase(1);
    CHECK(meters.size() == 2);
    meters.reset();
    CHECK(close_to(meters.get(0)->level_db, -90.0));
}

void test_settings_round_trip_and_legacy_contact() {
    using namespace lanspeak::gui;
    AppSettings source;
    source.window_width = 777;
    source.window_height = 555;
    source.language = LanguageSetting::russian;
    source.talk_mode = TalkMode::hold;
    source.ptt_all_hotkey = Hotkey{4, 'S'};
    source.debug_console_visible = true;
    source.capture_device_selector = L"{capture}\\device";
    source.render_device_selector = L"{render}";
    Contact contact;
    contact.name = L"Test\tUser";
    contact.host = L"192.168.0.25";
    contact.port = 50000;
    contact.gain = 1.4;
    contact.muted = true;
    contact.global_ptt_enabled = false;
    contact.ptt_hotkey = Hotkey{2, '1'};
    source.contacts.push_back(contact);

    const AppSettings parsed = parse_settings(serialize_settings(source));
    CHECK(parsed.window_width == 777);
    CHECK(parsed.window_height == 555);
    CHECK(parsed.language == LanguageSetting::russian);
    CHECK(parsed.talk_mode == TalkMode::hold);
    CHECK(parsed.contacts.size() == 1);
    CHECK(parsed.contacts[0].name == contact.name);
    CHECK(parsed.contacts[0].port == 50000);
    CHECK(close_to(parsed.contacts[0].gain, 1.4));
    CHECK(parsed.contacts[0].muted);
    CHECK(!parsed.contacts[0].global_ptt_enabled);
    CHECK(parsed.contacts[0].ptt_hotkey == contact.ptt_hotkey);

    const std::string legacy =
        "version=1\n"
        "contact=10.0.0.2\t49740\t1.5\t1\t10\t0.03\t7\t90\t150\n";
    const AppSettings old = parse_settings(legacy);
    CHECK(old.contacts.size() == 1);
    CHECK(old.contacts[0].name == L"10.0.0.2");
    CHECK(old.contacts[0].port == 49740);
    CHECK(close_to(old.contacts[0].gain, 1.5));
}

void test_fragmented_telemetry_snapshot() {
    lanspeak::gui::TelemetryParser parser;
    CHECK(!parser.append("local_level\t-18"));
    CHECK(parser.append(".5\t1\npeer_level\t0\t-90\t0\t0\npeer_"));
    CHECK(parser.append("level\t1\t-24\t1\t1\n"));
    const auto& snapshot = parser.snapshot();
    CHECK(snapshot.local.valid);
    CHECK(close_to(snapshot.local.level_db, -18.5));
    CHECK(snapshot.local.voice_active);
    CHECK(snapshot.peers.size() == 2);
    CHECK(!snapshot.peers[0].voice_active);
    CHECK(snapshot.peers[1].stream_active);
}

void test_gdi_cache_handle_count_is_stable() {
    {
        lanspeak::gui::GdiObjectCache warmup;
        CHECK(warmup.brush(RGB(0, 0, 0)) != nullptr);
        CHECK(warmup.pen(RGB(0, 0, 0)) != nullptr);
    }
    const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    lanspeak::gui::GdiObjectCache cache;
    constexpr std::array<COLORREF, 4> colors{
        RGB(42, 168, 91),
        RGB(52, 105, 184),
        RGB(176, 185, 198),
        RGB(248, 249, 251)};

    for (const COLORREF color : colors) {
        CHECK(cache.brush(color) != nullptr);
        CHECK(cache.pen(color, 1) != nullptr);
        CHECK(cache.pen(color, 2) != nullptr);
    }
    const DWORD warmed = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int iteration = 0; iteration < 10000; ++iteration) {
        const COLORREF color = colors[static_cast<std::size_t>(iteration) % colors.size()];
        CHECK(cache.brush(color) != nullptr);
        CHECK(cache.pen(color, iteration % 2 + 1) != nullptr);
    }
    CHECK(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == warmed);
    cache.reset();
    const DWORD after_reset = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    CHECK(after_reset < warmed);
    CHECK(after_reset >= before);
}

void test_hotkey_formatting_and_identity() {
    using namespace lanspeak::gui;
    const Hotkey hotkey{kHotkeyCtrl | kHotkeyAlt, 'S'};
    CHECK(format_hotkey(hotkey, L"not set") == L"Ctrl+Alt+S");
    CHECK(format_hotkey({}, L"not set") == L"not set");
    CHECK(same_hotkey(hotkey, Hotkey{kHotkeyCtrl | kHotkeyAlt, 'S'}));
    CHECK(!same_hotkey(hotkey, Hotkey{kHotkeyCtrl, 'S'}));
    CHECK(is_modifier_key(VK_LSHIFT));
    CHECK(modifier_mask_for_vk(VK_RCONTROL) == kHotkeyCtrl);
}

void test_about_localization() {
    using namespace lanspeak::gui;
    CHECK(std::wstring(localized_text(TextId::help, LanguageSetting::english)) == L"Help");
    CHECK(std::wstring(localized_text(TextId::help, LanguageSetting::russian)) == L"Помощь");
    CHECK(std::wstring(localized_text(TextId::about, LanguageSetting::english)) == L"About");
    CHECK(std::wstring(localized_text(TextId::about, LanguageSetting::russian)) == L"О программе");
    CHECK(std::wstring(localized_text(
              TextId::about_product_version,
              LanguageSetting::english)) == L"LAN Speak version 1.0.0");
    CHECK(std::wstring(localized_text(
              TextId::about_product_version,
              LanguageSetting::russian)) == L"LAN Speak версия 1.0.0");
}

} // namespace

int main() {
    try {
        test_udp_packet_validation();
        test_pcm_conversion_and_resampling();
        test_wasapi_pcm_payload_and_render_conversion();
        test_jitter_in_order_and_reordering();
        test_jitter_gap_overflow_and_underrun();
        test_peer_routing();
        test_room_mixer_reuses_storage();
        test_hot_paths_do_not_allocate_after_warmup();
        test_core_telemetry_snapshot();
        test_line_buffer_fragmentation();
        test_fragmented_room_control_commands();
        test_ducking_and_vu_math();
        test_contact_meter_bank();
        test_settings_round_trip_and_legacy_contact();
        test_fragmented_telemetry_snapshot();
        test_gdi_cache_handle_count_is_stable();
        test_hotkey_formatting_and_identity();
        test_about_localization();
        std::cout << "LanSpeak support tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
