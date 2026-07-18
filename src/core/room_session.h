#pragma once

#include "common/line_buffer.h"
#include "core/options.h"

#include <atomic>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace lanspeak::core {

struct RoomPeerControl {
    std::atomic<double> gain{1.0};
    std::atomic<double> duck_db{0.0};
    std::atomic<double> duck_threshold{0.02};
    std::atomic<int> duck_attack_ms{8};
    std::atomic<int> duck_hold_ms{80};
    std::atomic<int> duck_release_ms{120};
    std::atomic_bool global_ptt_enabled{true};
    std::atomic_bool private_talk{false};
};

struct RoomPeerControlSnapshot {
    double gain = 1.0;
    double duck_db = 0.0;
    double duck_threshold = 0.02;
    int duck_attack_ms = 8;
    int duck_hold_ms = 80;
    int duck_release_ms = 120;
    bool global_ptt_enabled = true;
    bool private_talk = false;
};

void initialize_room_peer_control(RoomPeerControl& control, const RoomPeerOptions& options);
RoomPeerControlSnapshot snapshot_room_peer_control(const RoomPeerControl& control);
bool valid_room_peer_settings(const RoomPeerOptions& options);

bool apply_room_control_line(
    std::string_view line,
    std::span<RoomPeerControl* const> peers,
    std::atomic_bool* input_muted,
    std::atomic_bool* stop,
    std::wostream* diagnostics = nullptr,
    std::atomic<std::uint64_t>* discovery_request_id = nullptr);

void consume_room_control_bytes(
    std::string_view bytes,
    common::LineBuffer& lines,
    std::span<RoomPeerControl* const> peers,
    std::atomic_bool* input_muted,
    std::atomic_bool* stop,
    std::wostream* diagnostics = nullptr,
    std::atomic<std::uint64_t>* discovery_request_id = nullptr);

} // namespace lanspeak::core
