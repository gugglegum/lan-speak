#include "common/line_buffer.h"
#include "core/audio_math.h"
#include "core/jitter_buffer.h"
#include "core/latency_model.h"
#include "core/network_discovery.h"
#include "core/options.h"
#include "core/pcm_audio.h"
#include "core/peer_info_tracker.h"
#include "core/peer_router.h"
#include "core/presence_tracker.h"
#include "core/room_mixer.h"
#include "core/room_session.h"
#include "core/telemetry_snapshot.h"
#include "core/udp_audio_packet.h"
#include "core/udp_discovery_packet.h"
#include "core/udp_peer_info_packet.h"
#include "core/udp_presence_packet.h"
#include "core/wasapi_audio.h"
#include "gui/contact_list_model.h"
#include "gui/vu_math.h"
#include "gui/gdi_object_cache.h"
#include "gui/hotkey_utils.h"
#include "gui/localization.h"
#include "gui/network_adapters.h"
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
#include <optional>
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

void test_udp_presence_packet_validation() {
    using namespace lanspeak::core;
    UdpPresencePacket source{};
    source.type = UdpPresenceType::ping;
    source.session_id = 123;
    source.nonce = 456;

    UdpPresencePacket parsed{};
    const UdpPresenceDatagram datagram = serialize_udp_presence_packet(source);
    CHECK(validate_udp_presence_packet(datagram, parsed));
    CHECK(parsed.type == UdpPresenceType::ping);
    CHECK(parsed.session_id == 123);
    CHECK(parsed.nonce == 456);
    CHECK(!validate_udp_presence_packet(std::span(datagram).first(datagram.size() - 1), parsed));
    std::array<std::byte, sizeof(UdpPresencePacket) + 1> oversized{};
    std::copy(datagram.begin(), datagram.end(), oversized.begin());
    CHECK(!validate_udp_presence_packet(oversized, parsed));

    source.type = UdpPresenceType::pong;
    CHECK(validate_udp_presence_packet(serialize_udp_presence_packet(source), parsed));
    CHECK(parsed.type == UdpPresenceType::pong);
    CHECK(parsed.nonce == source.nonce);
    source.type = UdpPresenceType::ping;

    auto invalid_packet = source;
    invalid_packet.magic = 0;
    CHECK(!validate_udp_presence_packet(serialize_udp_presence_packet(invalid_packet), parsed));
    invalid_packet = source;
    invalid_packet.version = 2;
    CHECK(!validate_udp_presence_packet(serialize_udp_presence_packet(invalid_packet), parsed));
    invalid_packet = source;
    invalid_packet.header_size = 0;
    CHECK(!validate_udp_presence_packet(serialize_udp_presence_packet(invalid_packet), parsed));
    invalid_packet = source;
    invalid_packet.reserved = 1;
    CHECK(!validate_udp_presence_packet(serialize_udp_presence_packet(invalid_packet), parsed));
    invalid_packet = source;
    invalid_packet.type = static_cast<UdpPresenceType>(99);
    CHECK(!validate_udp_presence_packet(serialize_udp_presence_packet(invalid_packet), parsed));
    invalid_packet = source;
    invalid_packet.session_id = 0;
    CHECK(!validate_udp_presence_packet(serialize_udp_presence_packet(invalid_packet), parsed));
    invalid_packet = source;
    invalid_packet.nonce = 0;
    CHECK(!validate_udp_presence_packet(serialize_udp_presence_packet(invalid_packet), parsed));

    source.type = UdpPresenceType::goodbye;
    source.nonce = 0;
    CHECK(validate_udp_presence_packet(serialize_udp_presence_packet(source), parsed));
    source.nonce = 1;
    CHECK(!validate_udp_presence_packet(serialize_udp_presence_packet(source), parsed));
}

void test_udp_discovery_packet_validation() {
    using namespace lanspeak::core;
    UdpDiscoveryPacket query{};
    query.header.type = UdpDiscoveryType::query;
    query.header.request_id = 123;
    query.header.session_id = 456;
    query.header.voice_port = 49740;
    UdpDiscoveryPacket parsed{};
    const auto query_datagram = serialize_udp_discovery_packet(query);
    CHECK(query_datagram.size() == sizeof(UdpDiscoveryHeader));
    CHECK(validate_udp_discovery_packet(query_datagram, parsed));
    CHECK(parsed.header.type == UdpDiscoveryType::query);
    CHECK(parsed.computer_name_utf8.empty());
    CHECK(!validate_udp_discovery_packet(
        std::span(query_datagram).first(query_datagram.size() - 1), parsed));

    UdpDiscoveryPacket response = query;
    response.header.type = UdpDiscoveryType::response;
    response.computer_name_utf8 = "PC-\xD0\x9C\xD0\xB0\xD0\xBA\xD1\x81";
    const auto response_datagram = serialize_udp_discovery_packet(response);
    CHECK(validate_udp_discovery_packet(response_datagram, parsed));
    CHECK(parsed.computer_name_utf8 == response.computer_name_utf8);

    auto invalid = query_datagram;
    UdpDiscoveryHeader header{};
    std::memcpy(&header, invalid.data(), sizeof(header));
    header.magic = 0;
    std::memcpy(invalid.data(), &header, sizeof(header));
    CHECK(!validate_udp_discovery_packet(invalid, parsed));
    header = query.header;
    header.version = 2;
    std::memcpy(invalid.data(), &header, sizeof(header));
    CHECK(!validate_udp_discovery_packet(invalid, parsed));
    header = query.header;
    header.request_id = 0;
    std::memcpy(invalid.data(), &header, sizeof(header));
    CHECK(!validate_udp_discovery_packet(invalid, parsed));
    header = query.header;
    header.session_id = 0;
    std::memcpy(invalid.data(), &header, sizeof(header));
    CHECK(!validate_udp_discovery_packet(invalid, parsed));

    response.computer_name_utf8.assign(kMaximumDiscoveryNameBytes + 1, 'x');
    CHECK(serialize_udp_discovery_packet(response).empty());
    response.computer_name_utf8 = std::string("\xC3\x28", 2);
    CHECK(!validate_udp_discovery_packet(serialize_udp_discovery_packet(response), parsed));
}

void test_discovery_network_helpers() {
    using namespace lanspeak::core;
    CHECK(ipv4_broadcast_host_order(0xc0a8012au, 24) == 0xc0a801ffu);
    CHECK(ipv4_broadcast_host_order(0x0a142132u, 16) == 0x0a14ffffu);
    CHECK(ipv4_broadcast_host_order(0x7f000001u, 32) == 0x7f000001u);

    DiscoveryReplyCache cache;
    CHECK(cache.should_reply(0x01020304u, 77, 1000));
    CHECK(!cache.should_reply(0x01020304u, 77, 1001));
    CHECK(cache.should_reply(0x01020305u, 77, 1001));
    CHECK(cache.should_reply(0x01020304u, 78, 1001));
    CHECK(cache.should_reply(0x01020304u, 77, 11'000));
}

void test_udp_peer_info_packet_validation() {
    using namespace lanspeak::core;
    UdpPeerInfoPacket source{};
    source.type = UdpPeerInfoType::info;
    source.session_id = 123;
    source.revision = 7;
    source.flags = kPeerInfoCaptureToSendValid | kPeerInfoRenderLatencyValid |
        kPeerInfoPacketDurationValid | kPeerInfoReceiveBufferValid;
    source.capture_to_send_us = 8'000;
    source.render_latency_us = 6'000;
    source.packet_duration_us = 10'000;
    source.receive_buffer_us = 20'000;

    UdpPeerInfoPacket parsed{};
    const auto datagram = serialize_udp_peer_info_packet(source);
    CHECK(validate_udp_peer_info_packet(datagram, parsed));
    CHECK(parsed.type == UdpPeerInfoType::info);
    CHECK(parsed.revision == 7);
    CHECK(parsed.receive_buffer_us == 20'000);
    CHECK(!validate_udp_peer_info_packet(std::span(datagram).first(datagram.size() - 1), parsed));

    auto invalid = source;
    invalid.magic = 0;
    CHECK(!validate_udp_peer_info_packet(serialize_udp_peer_info_packet(invalid), parsed));
    invalid = source;
    invalid.version = 2;
    CHECK(!validate_udp_peer_info_packet(serialize_udp_peer_info_packet(invalid), parsed));
    invalid = source;
    invalid.packet_size = 0;
    CHECK(!validate_udp_peer_info_packet(serialize_udp_peer_info_packet(invalid), parsed));
    invalid = source;
    invalid.session_id = 0;
    CHECK(!validate_udp_peer_info_packet(serialize_udp_peer_info_packet(invalid), parsed));
    invalid = source;
    invalid.revision = 0;
    CHECK(!validate_udp_peer_info_packet(serialize_udp_peer_info_packet(invalid), parsed));
    invalid = source;
    invalid.flags |= 0x8000;
    CHECK(!validate_udp_peer_info_packet(serialize_udp_peer_info_packet(invalid), parsed));
    invalid = source;
    invalid.capture_to_send_us = 0;
    CHECK(!validate_udp_peer_info_packet(serialize_udp_peer_info_packet(invalid), parsed));
    invalid = source;
    invalid.render_latency_us = 5'000'001;
    CHECK(!validate_udp_peer_info_packet(serialize_udp_peer_info_packet(invalid), parsed));

    for (const UdpPeerInfoType type : {UdpPeerInfoType::request, UdpPeerInfoType::ack}) {
        UdpPeerInfoPacket control{};
        control.type = type;
        control.session_id = 123;
        control.revision = 7;
        CHECK(validate_udp_peer_info_packet(serialize_udp_peer_info_packet(control), parsed));
        control.receive_buffer_us = 1;
        CHECK(!validate_udp_peer_info_packet(serialize_udp_peer_info_packet(control), parsed));
    }
}

void test_latency_model() {
    using namespace lanspeak::core;
    constexpr std::uint64_t frequency = 10'000'000;
    constexpr std::uint64_t capture_position = 1'000'000;
    constexpr std::uint64_t send_position = 1'120'000;
    const auto age = capture_midpoint_age_us(
        capture_position, 480, 48'000, send_position, frequency, false);
    CHECK(age.has_value());
    CHECK(close_to(*age, 7'000.0));
    CHECK(!capture_midpoint_age_us(
        capture_position, 480, 48'000, send_position, frequency, true));
    CHECK(!capture_midpoint_age_us(
        capture_position, 480, 48'000, capture_position, frequency, false));

    CaptureLatencyEstimator measured(0, frequency, 20'000);
    for (int index = 0; index < 31; ++index) {
        measured.add_packet(capture_position, 480, 48'000, send_position, false);
    }
    CHECK(!measured.ready(send_position));
    measured.add_packet(capture_position, 480, 48'000, send_position, false);
    CHECK(measured.ready(send_position));
    CHECK(measured.capture_to_send_us(send_position) == 7'000);
    CHECK(measured.packet_duration_us() == 10'000);
    CHECK(measured.valid_sample_count() == 32);

    CaptureLatencyEstimator fallback(100, 1'000, 12'000);
    fallback.add_packet(0, 480, 48'000, 1'000, true);
    CHECK(!fallback.ready(2'099));
    CHECK(fallback.ready(2'100));
    CHECK(fallback.capture_to_send_us(2'100) == 12'000);

    const PeerLatencyProfile local{8'000, 6'000, 10'000, 5'000};
    const PeerLatencyProfile remote{12'000, 9'000, 10'000, 20'000};
    const DirectionalLatencyEstimate estimate =
        estimate_directional_latency(&local, &remote, 40.0);
    CHECK(close_to(estimate.incoming_ms, 43.0));
    CHECK(close_to(estimate.outgoing_ms, 57.0));
    CHECK(estimate_directional_latency(&local, &remote, -1.0).incoming_ms < 0.0);
    PeerLatencyProfile incomplete = remote;
    incomplete.render_latency_us = 0;
    CHECK(estimate_directional_latency(&local, &incomplete, 4.0).outgoing_ms < 0.0);
}

void test_peer_info_tracker() {
    using namespace lanspeak::core;
    const PeerLatencyProfile local{8'000, 6'000, 10'000, 20'000};
    PeerInfoTracker tracker(1, 100);
    tracker.set_local_profile(0, local, 0);
    tracker.update_presence(0, PeerPresenceState::online, 200, 0);
    auto actions = tracker.tick(0);
    CHECK(actions.size() == 1);
    CHECK(actions[0].packet.type == UdpPeerInfoType::info);
    CHECK(actions[0].packet.session_id == 100);
    const std::uint32_t local_revision = actions[0].packet.revision;
    CHECK(tracker.tick(999).empty());
    CHECK(tracker.tick(1'000).size() == 2); // INFO retry and first REQUEST.

    UdpPeerInfoPacket ack{};
    ack.type = UdpPeerInfoType::ack;
    ack.session_id = 100;
    ack.revision = local_revision;
    CHECK(tracker.on_packet(0, ack, 1'001).empty());

    UdpPeerInfoPacket remote_info{};
    remote_info.type = UdpPeerInfoType::info;
    remote_info.session_id = 200;
    remote_info.revision = 3;
    remote_info.flags = kPeerInfoCaptureToSendValid | kPeerInfoRenderLatencyValid |
        kPeerInfoPacketDurationValid | kPeerInfoReceiveBufferValid;
    remote_info.capture_to_send_us = 11'000;
    remote_info.render_latency_us = 9'000;
    remote_info.packet_duration_us = 10'000;
    remote_info.receive_buffer_us = 5'000;
    actions = tracker.on_packet(0, remote_info, 1'002);
    CHECK(actions.size() == 1);
    CHECK(actions[0].packet.type == UdpPeerInfoType::ack);
    CHECK(actions[0].packet.session_id == 200);
    CHECK(tracker.snapshot(0).valid);
    CHECK(tracker.snapshot(0).remote_revision == 3);
    CHECK(tracker.tick(100'000).empty());

    actions = tracker.on_packet(0, remote_info, 100'001);
    CHECK(actions.size() == 1); // Duplicate INFO is ACKed without being reapplied.
    remote_info.revision = 2;
    CHECK(tracker.on_packet(0, remote_info, 100'002).empty());

    tracker.update_presence(0, PeerPresenceState::online, 201, 100'003);
    CHECK(!tracker.snapshot(0).valid);
    remote_info.session_id = 200;
    remote_info.revision = 4;
    CHECK(tracker.on_packet(0, remote_info, 100'004).empty());
    CHECK(tracker.tick(101'003).size() == 2); // New INFO and REQUEST for the new session.
    UdpPeerInfoPacket request{};
    request.type = UdpPeerInfoType::request;
    request.session_id = 200;
    request.revision = 1;
    CHECK(tracker.on_packet(0, request, 100'005).empty());
    request.session_id = 201;
    actions = tracker.on_packet(0, request, 100'006);
    CHECK(actions.size() == 1);
    CHECK(actions[0].packet.type == UdpPeerInfoType::info);

    PeerInfoTracker retries(1, 300);
    retries.set_local_profile(0, local, 0);
    retries.update_presence(0, PeerPresenceState::online, 400, 0);
    CHECK(retries.tick(0).size() == 1);
    CHECK(retries.tick(1'000).size() == 2);
    CHECK(retries.tick(3'000).size() == 2);
    CHECK(retries.tick(3'001).empty());
    CHECK(retries.tick(8'000).size() == 1); // Final REQUEST only; INFO retries are exhausted.
    CHECK(retries.tick(100'000).empty());

    retries.update_presence(0, PeerPresenceState::offline, 400, 100'001);
    CHECK(!retries.snapshot(0).valid);
    CHECK(retries.tick(100'002).empty());
}

void test_presence_tracker_lifecycle() {
    using namespace lanspeak::core;

    PresenceTracker retries(2, 10, 20);
    retries.start(100);
    auto actions = retries.tick(100);
    CHECK(actions.size() == 2);
    CHECK(actions[0].type == UdpPresenceType::ping);
    CHECK(actions[0].nonce != 0);
    CHECK(retries.tick(1'099).empty());
    CHECK(retries.tick(1'100).size() == 2);
    CHECK(retries.tick(3'099).empty());
    CHECK(retries.tick(3'100).size() == 2);
    CHECK(retries.tick(5'099).empty());
    CHECK(retries.tick(5'100).empty());
    CHECK(retries.snapshot(0).state == PeerPresenceState::unknown);
    CHECK(retries.snapshot(1).state == PeerPresenceState::unknown);
    CHECK(retries.tick(65'099).empty());
    CHECK(retries.tick(125'101).size() == 2);

    PresenceTracker tracker(1, 100, 200);
    tracker.start(1'000);
    actions = tracker.tick(1'000);
    CHECK(actions.size() == 1);
    const std::uint64_t nonce = actions[0].nonce;

    UdpPresencePacket pong{};
    pong.type = UdpPresenceType::pong;
    pong.session_id = 900;
    pong.nonce = nonce + 1;
    tracker.on_packet(0, pong, 1'020);
    CHECK(tracker.snapshot(0).state == PeerPresenceState::unknown);
    pong.nonce = nonce;
    tracker.on_packet(0, pong, 1'042);
    CHECK(tracker.snapshot(0).state == PeerPresenceState::online);
    CHECK(tracker.snapshot(0).remote_session_id == 900);
    CHECK(close_to(tracker.snapshot(0).rtt_ms, 42.0));
    CHECK(tracker.tick(26'041).empty());
    const auto periodic_actions = tracker.tick(36'043);
    CHECK(periodic_actions.size() == 1);
    const PresenceAction periodic_ping = periodic_actions[0];

    UdpPresencePacket ping{};
    ping.type = UdpPresenceType::ping;
    ping.session_id = 901;
    ping.nonce = 777;
    const auto response = tracker.on_packet(0, ping, 36'044);
    CHECK(response.has_value());
    CHECK(response->type == UdpPresenceType::pong);
    CHECK(response->nonce == ping.nonce);
    CHECK(tracker.snapshot(0).remote_session_id == 901);
    CHECK(tracker.snapshot(0).rtt_ms < 0.0);
    pong.session_id = 901;
    pong.nonce = periodic_ping.nonce;
    tracker.on_packet(0, pong, 36'050);
    CHECK(close_to(tracker.snapshot(0).rtt_ms, 7.0));
    CHECK(tracker.tick(61'043).empty());

    UdpPresencePacket goodbye{};
    goodbye.type = UdpPresenceType::goodbye;
    goodbye.session_id = 900;
    goodbye.nonce = 0;
    tracker.on_packet(0, goodbye, 61'044);
    CHECK(tracker.snapshot(0).state == PeerPresenceState::online);
    goodbye.session_id = 901;
    tracker.on_packet(0, goodbye, 61'045);
    CHECK(tracker.snapshot(0).state == PeerPresenceState::offline);

    tracker.on_audio_packet(0, 61'046);
    CHECK(tracker.snapshot(0).state == PeerPresenceState::online);
    CHECK(tracker.snapshot(0).remote_session_id == 901);
    CHECK(tracker.tick(86'045).empty());

    PresenceTracker lost_after_online(1, 300, 400);
    lost_after_online.start(0);
    const auto initial_ping = lost_after_online.tick(0);
    CHECK(initial_ping.size() == 1);
    pong.session_id = 902;
    pong.nonce = initial_ping[0].nonce;
    lost_after_online.on_packet(0, pong, 5);
    CHECK(lost_after_online.tick(35'006).size() == 1);
    CHECK(lost_after_online.tick(36'006).size() == 1);
    CHECK(lost_after_online.tick(38'006).size() == 1);
    CHECK(lost_after_online.tick(40'006).empty());
    CHECK(lost_after_online.snapshot(0).state == PeerPresenceState::offline);

    const auto shutdown = tracker.shutdown();
    CHECK(shutdown.size() == 1);
    CHECK(shutdown[0].type == UdpPresenceType::goodbye);
    CHECK(shutdown[0].nonce == 0);
}

void test_presence_tracker_bidirectional_rtt() {
    using namespace lanspeak::core;

    PresenceTracker first(1, 100, 300);
    PresenceTracker second(1, 200, 400);
    first.start(1'000);
    second.start(1'000);
    const PresenceAction first_ping = first.tick(1'000)[0];
    const PresenceAction second_ping = second.tick(1'000)[0];

    UdpPresencePacket packet{};
    packet.type = UdpPresenceType::ping;
    packet.session_id = 100;
    packet.nonce = first_ping.nonce;
    const PresenceAction second_pong = *second.on_packet(0, packet, 1'002);
    packet.session_id = 200;
    packet.nonce = second_ping.nonce;
    const PresenceAction first_pong = *first.on_packet(0, packet, 1'003);

    packet.type = UdpPresenceType::pong;
    packet.session_id = 200;
    packet.nonce = second_pong.nonce;
    first.on_packet(0, packet, 1'006);
    packet.session_id = 100;
    packet.nonce = first_pong.nonce;
    second.on_packet(0, packet, 1'007);
    CHECK(close_to(first.snapshot(0).rtt_ms, 6.0));
    CHECK(close_to(second.snapshot(0).rtt_ms, 7.0));

    PresenceTracker early(1, 500, 600);
    PresenceTracker late(1, 700, 800);
    early.start(0);
    CHECK(early.tick(0).size() == 1);
    CHECK(early.tick(1'000).size() == 1);
    CHECK(early.tick(3'000).size() == 1);
    CHECK(early.tick(5'000).empty());
    late.start(6'000);
    const PresenceAction late_ping = late.tick(6'000)[0];
    packet.type = UdpPresenceType::ping;
    packet.session_id = 700;
    packet.nonce = late_ping.nonce;
    const PresenceAction early_pong = *early.on_packet(0, packet, 6'001);
    packet.type = UdpPresenceType::pong;
    packet.session_id = 500;
    packet.nonce = early_pong.nonce;
    late.on_packet(0, packet, 6'003);
    CHECK(late.snapshot(0).rtt_ms >= 0.0);

    const auto immediate = early.tick(6'001);
    CHECK(immediate.size() == 1);
    CHECK(immediate[0].type == UdpPresenceType::ping);
    packet.type = UdpPresenceType::ping;
    packet.session_id = 500;
    packet.nonce = immediate[0].nonce;
    const PresenceAction late_pong = *late.on_packet(0, packet, 6'002);
    packet.type = UdpPresenceType::pong;
    packet.session_id = 700;
    packet.nonce = late_pong.nonce;
    early.on_packet(0, packet, 6'004);
    CHECK(early.snapshot(0).rtt_ms >= 0.0);
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
        telemetry.update_peer_presence(0, PeerPresenceState::online, 1.0);
        telemetry.update_peer_latency(0, 15.0, 16.0);
        allocations = scope.count();
    }
    CHECK(allocations == 0);
}

void test_core_telemetry_snapshot() {
    lanspeak::core::TelemetrySnapshot source(2);
    source.accumulate_local_peak(0.1);
    source.update_peer_meter(0, -12.5, true);
    source.mark_peer_stream(0, 1000);
    source.update_peer_presence(0, lanspeak::core::PeerPresenceState::online, 2.5);
    source.update_peer_latency(0, 31.5, 42.5);
    source.update_peer_meter(1, -60.0, false);
    source.update_peer_presence(1, lanspeak::core::PeerPresenceState::offline, -1.0);
    source.set_audio_endpoint_diagnostics(
        lanspeak::core::AudioEndpointKind::capture,
        lanspeak::core::AudioEndpointDiagnostics{
            true, "Microphone\\Input", 48000, 2, 32, 2.67, 5.83, 8.0, true});
    source.set_audio_endpoint_diagnostics(
        lanspeak::core::AudioEndpointKind::render,
        lanspeak::core::AudioEndpointDiagnostics{
            true, "Headphones", 44100, 2, 32, 10.0, 22.0, 25.0, false});
    source.update_render_padding_ms(3.25);

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
    CHECK(first.peer_presence.size() == 2);
    CHECK(first.peer_presence[0].state == lanspeak::gui::PeerPresenceState::online);
    CHECK(close_to(first.peer_presence[0].rtt_ms, 2.5));
    CHECK(first.peer_latency.size() == 2);
    CHECK(close_to(first.peer_latency[0].incoming_ms, 31.5));
    CHECK(close_to(first.peer_latency[0].outgoing_ms, 42.5));
    CHECK(first.peer_presence[1].state == lanspeak::gui::PeerPresenceState::offline);
    CHECK(first.capture.valid);
    CHECK(first.capture.name_utf8 == "Microphone\\Input");
    CHECK(first.capture.sample_rate == 48000);
    CHECK(first.capture.low_latency_shared);
    CHECK(close_to(first.capture.buffer_ms, 5.83, 0.01));
    CHECK(first.render.valid);
    CHECK(first.render.name_utf8 == "Headphones");
    CHECK(first.render.sample_rate == 44100);
    CHECK(!first.render.low_latency_shared);
    CHECK(close_to(first.render.current_padding_ms, 3.25, 0.01));

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
    std::atomic<std::uint64_t> discovery_request_id{0};
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
    consume_room_control_bytes(
        "down\ndisc",
        lines,
        peers,
        &input_muted,
        &stop,
        nullptr,
        &discovery_request_id);
    consume_room_control_bytes(
        "over 998877\n",
        lines,
        peers,
        &input_muted,
        &stop,
        nullptr,
        &discovery_request_id);

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
    CHECK(discovery_request_id.load() == 998877);
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
    source.ptt_all_hotkey = Hotkey{0, VK_XBUTTON1};
    source.debug_console_visible = true;
    source.local_port = 51234;
    source.network_adapter_id = L"{stable-adapter-id}";
    source.capture_device_selector = L"{capture}\\device";
    source.render_device_selector = L"{render}";
    Contact contact;
    contact.name = L"Test\tUser";
    contact.host = L"192.168.0.25";
    contact.port = 50000;
    contact.gain = 1.4;
    contact.muted = true;
    contact.global_ptt_enabled = false;
    contact.receive_buffer_ms = 45;
    contact.ptt_hotkey = Hotkey{2, '1'};
    source.contacts.push_back(contact);

    const AppSettings parsed = parse_settings(serialize_settings(source));
    CHECK(parsed.window_width == 777);
    CHECK(parsed.window_height == 555);
    CHECK(parsed.language == LanguageSetting::russian);
    CHECK(parsed.local_port == 51234);
    CHECK(parsed.network_adapter_id == source.network_adapter_id);
    CHECK(serialize_settings(source).find("talk_mode=") == std::string::npos);
    CHECK(parsed.contacts.size() == 1);
    CHECK(parsed.contacts[0].name == contact.name);
    CHECK(parsed.contacts[0].port == 50000);
    CHECK(close_to(parsed.contacts[0].gain, 1.4));
    CHECK(parsed.contacts[0].muted);
    CHECK(!parsed.contacts[0].global_ptt_enabled);
    CHECK(parsed.contacts[0].receive_buffer_ms == 45);
    CHECK(parsed.contacts[0].ptt_hotkey == contact.ptt_hotkey);

    const std::string legacy =
        "version=1\n"
        "talk_mode=toggle\n"
        "contact=10.0.0.2\t49740\t1.5\t1\t10\t0.03\t7\t90\t150\n";
    const AppSettings old = parse_settings(legacy);
    CHECK(old.contacts.size() == 1);
    CHECK(old.contacts[0].name == L"10.0.0.2");
    CHECK(old.contacts[0].port == 49740);
    CHECK(close_to(old.contacts[0].gain, 1.5));
    CHECK(old.contacts[0].receive_buffer_ms == 20);
}

void test_room_peer_receive_buffer_options() {
    using namespace lanspeak::core;
    std::vector<std::wstring> arguments{
        L"LanSpeakCore.exe",
        L"--room",
        L"49740",
        L"--bind-address",
        L"192.168.0.10",
        L"--peer",
        L"192.168.0.2",
        L"49740",
        L"1.0",
        L"12",
        L"0.02",
        L"8",
        L"80",
        L"120",
        L"0",
        L"45",
        L"--peer",
        L"192.168.0.3",
        L"49740",
        L"1.0",
        L"0",
        L"0.02",
        L"8",
        L"80",
        L"120"};
    std::vector<wchar_t*> argv;
    argv.reserve(arguments.size());
    for (std::wstring& argument : arguments) {
        argv.push_back(argument.data());
    }

    const ProbeOptions options = parse_options(static_cast<int>(argv.size()), argv.data());
    CHECK(!options.show_help);
    CHECK(options.capture_role == eConsole);
    CHECK(options.render_role == eConsole);
    CHECK(options.bind_address == L"192.168.0.10");
    CHECK(options.room_peers.size() == 2);
    CHECK(options.room_peers[0].receive_buffer_ms == 45);
    CHECK(!options.room_peers[0].global_ptt_enabled);
    CHECK(options.room_peers[1].receive_buffer_ms == kDefaultReceiveBufferMs);

    std::vector<std::wstring> empty_room_arguments{
        L"LanSpeakCore.exe", L"--room", L"49740"};
    argv.clear();
    for (std::wstring& argument : empty_room_arguments) argv.push_back(argument.data());
    const ProbeOptions empty_room = parse_options(static_cast<int>(argv.size()), argv.data());
    CHECK(!empty_room.show_help);
    CHECK(empty_room.mode == ProbeOptions::Mode::room);
    CHECK(empty_room.room_peers.empty());
}

void test_network_adapter_resolution() {
    using namespace lanspeak::gui;
    const std::vector<NetworkAdapterInfo> adapters{
        {L"adapter-a", L"Ethernet", L"192.168.0.10"},
        {L"adapter-b", L"Wi-Fi", L"192.168.1.20"}};
    CHECK(resolve_network_bind_address(adapters, L"") == std::optional<std::wstring>(L"0.0.0.0"));
    CHECK(resolve_network_bind_address(adapters, L"adapter-b") ==
          std::optional<std::wstring>(L"192.168.1.20"));
    CHECK(!resolve_network_bind_address(adapters, L"missing"));
}

void test_fragmented_telemetry_snapshot() {
    lanspeak::gui::TelemetryParser parser;
    CHECK(!parser.append("local_level\t-18"));
    CHECK(parser.append(".5\t1\npeer_level\t0\t-90\t0\t0\npeer_"));
    CHECK(parser.append(
        "level\t1\t-24\t1\t1\n"
        "peer_presence\t0\t1\t3.5\n"
        "peer_latency\t0\t14.5\t18.5\n"
        "audio_input\tUSB\\tMic\t48000\t2\t32\t2.7\t5.8\t8.1\t1\t-1\n"));
    const auto& snapshot = parser.snapshot();
    CHECK(snapshot.local.valid);
    CHECK(close_to(snapshot.local.level_db, -18.5));
    CHECK(snapshot.local.voice_active);
    CHECK(snapshot.peers.size() == 2);
    CHECK(!snapshot.peers[0].voice_active);
    CHECK(snapshot.peers[1].stream_active);
    CHECK(snapshot.peer_presence.size() == 1);
    CHECK(snapshot.peer_presence[0].state == lanspeak::gui::PeerPresenceState::online);
    CHECK(close_to(snapshot.peer_presence[0].rtt_ms, 3.5));
    CHECK(snapshot.peer_latency.size() == 1);
    CHECK(close_to(snapshot.peer_latency[0].incoming_ms, 14.5));
    CHECK(close_to(snapshot.peer_latency[0].outgoing_ms, 18.5));
    CHECK(snapshot.capture.valid);
    CHECK(snapshot.capture.name_utf8 == "USB\tMic");
    CHECK(snapshot.capture.sample_rate == 48000);
    CHECK(snapshot.capture.low_latency_shared);
    CHECK(parser.append(
        "discovery_peer\t44\t55\t192.168.0.20\t49740\t0\tDESKTOP\\tONE\n"
        "discovery_error\t45\t10049\n"));
    CHECK(snapshot.discovery_peers.size() == 1);
    CHECK(snapshot.discovery_peers[0].request_id == 44);
    CHECK(snapshot.discovery_peers[0].session_id == 55);
    CHECK(snapshot.discovery_peers[0].voice_port == 49740);
    CHECK(snapshot.discovery_peers[0].computer_name_utf8 == "DESKTOP\tONE");
    CHECK(snapshot.discovery_error.valid);
    CHECK(snapshot.discovery_error.request_id == 45);
    CHECK(snapshot.discovery_error.error_code == 10049);

    CHECK(parser.append(
        "discovery_peer\t44\t55\t192.168.0.20\t49740\t1\tDESKTOP\\tONE\n"));
    CHECK(snapshot.discovery_peers.size() == 1);
    CHECK(snapshot.discovery_peers[0].already_contact);

    parser.clear();
    CHECK(parser.snapshot().peer_presence.empty());
    CHECK(parser.snapshot().peer_latency.empty());
    CHECK(parser.append("peer_level\t0\t-30\t1\t1\n"));
    CHECK(parser.snapshot().peers.size() == 1);
    CHECK(parser.snapshot().peer_presence.empty());
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
    CHECK(is_supported_mouse_hotkey(VK_RBUTTON));
    CHECK(is_supported_mouse_hotkey(VK_MBUTTON));
    CHECK(is_supported_mouse_hotkey(VK_XBUTTON1));
    CHECK(is_supported_mouse_hotkey(VK_XBUTTON2));
    CHECK(!is_supported_mouse_hotkey(VK_LBUTTON));
    CHECK(format_hotkey(Hotkey{0, VK_RBUTTON}, L"not set") == L"Mouse 2");
    CHECK(format_hotkey(Hotkey{0, VK_XBUTTON2}, L"not set") == L"Mouse 5");
}

void test_about_localization() {
    using namespace lanspeak::gui;
    CHECK(std::wstring(localized_text(TextId::help, LanguageSetting::english)) == L"Help");
    CHECK(std::wstring(localized_text(TextId::help, LanguageSetting::russian)) == L"Помощь");
    CHECK(std::wstring(localized_text(TextId::about, LanguageSetting::english)) == L"About");
    CHECK(std::wstring(localized_text(TextId::about, LanguageSetting::russian)) == L"О программе");
    CHECK(std::wstring(localized_text(
              TextId::about_product_version,
              LanguageSetting::english)) == L"LAN Speak version ");
    CHECK(std::wstring(localized_text(
              TextId::about_product_version,
              LanguageSetting::russian)) == L"LAN Speak версия ");
    CHECK(std::wstring(localized_text(
              TextId::audio_latency_diagnostics,
              LanguageSetting::english)) == L"Audio latency diagnostics");
    CHECK(std::wstring(localized_text(
              TextId::audio_latency_diagnostics,
              LanguageSetting::russian)) == L"Диагностика задержек звука");
    CHECK(std::wstring(localized_text(
              TextId::presence_online,
              LanguageSetting::english)) == L"Online");
    CHECK(std::wstring(localized_text(
              TextId::presence_offline,
              LanguageSetting::russian)) == L"Офлайн");
    CHECK(std::wstring(localized_text(
              TextId::presence_unknown,
              LanguageSetting::russian)) == L"Статус неизвестен");
}

} // namespace

int main() {
    try {
        test_udp_packet_validation();
        test_udp_presence_packet_validation();
        test_udp_discovery_packet_validation();
        test_discovery_network_helpers();
        test_udp_peer_info_packet_validation();
        test_latency_model();
        test_peer_info_tracker();
        test_presence_tracker_lifecycle();
        test_presence_tracker_bidirectional_rtt();
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
        test_room_peer_receive_buffer_options();
        test_network_adapter_resolution();
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
