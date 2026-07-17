#include "core/room_session.h"

#include <cmath>
#include <iostream>
#include <locale>
#include <sstream>

namespace lanspeak::core {
namespace {

void log(std::wostream* output, std::wstring_view message) {
    if (output != nullptr) {
        *output << message << L'\n';
    }
}

} // namespace

void initialize_room_peer_control(RoomPeerControl& control, const RoomPeerOptions& options) {
    control.gain.store(options.gain, std::memory_order_relaxed);
    control.duck_db.store(options.self_duck_db, std::memory_order_relaxed);
    control.duck_threshold.store(options.self_duck_threshold, std::memory_order_relaxed);
    control.duck_attack_ms.store(options.self_duck_attack_ms, std::memory_order_relaxed);
    control.duck_hold_ms.store(options.self_duck_hold_ms, std::memory_order_relaxed);
    control.duck_release_ms.store(options.self_duck_release_ms, std::memory_order_relaxed);
    control.global_ptt_enabled.store(options.global_ptt_enabled, std::memory_order_relaxed);
}

RoomPeerControlSnapshot snapshot_room_peer_control(const RoomPeerControl& control) {
    return RoomPeerControlSnapshot{
        control.gain.load(std::memory_order_relaxed),
        control.duck_db.load(std::memory_order_relaxed),
        control.duck_threshold.load(std::memory_order_relaxed),
        control.duck_attack_ms.load(std::memory_order_relaxed),
        control.duck_hold_ms.load(std::memory_order_relaxed),
        control.duck_release_ms.load(std::memory_order_relaxed),
        control.global_ptt_enabled.load(std::memory_order_relaxed),
        control.private_talk.load(std::memory_order_relaxed)};
}

bool valid_room_peer_settings(const RoomPeerOptions& options) {
    return std::isfinite(options.gain) && options.gain >= 0.0 && options.gain <= 8.0 &&
        std::isfinite(options.self_duck_db) && options.self_duck_db >= 0.0 &&
        options.self_duck_db <= 60.0 &&
        std::isfinite(options.self_duck_threshold) && options.self_duck_threshold >= 0.0 &&
        options.self_duck_threshold <= 1.0 &&
        options.self_duck_attack_ms >= 0 && options.self_duck_attack_ms <= 1000 &&
        options.self_duck_hold_ms >= 0 && options.self_duck_hold_ms <= 1000 &&
        options.self_duck_release_ms >= 0 && options.self_duck_release_ms <= 5000 &&
        options.receive_buffer_ms >= 1 && options.receive_buffer_ms <= 500;
}

bool apply_room_control_line(
    std::string_view line,
    std::span<RoomPeerControl* const> peers,
    std::atomic_bool* input_muted,
    std::atomic_bool* stop,
    std::wostream* diagnostics) {
    if (line.empty()) return false;
    std::istringstream stream{std::string(line)};
    stream.imbue(std::locale::classic());
    std::string command;
    stream >> command;
    if (command == "shutdown") {
        if (stop != nullptr) stop->store(true, std::memory_order_relaxed);
        log(diagnostics, L"Control: graceful shutdown requested");
        return true;
    }
    if (command == "input_muted") {
        int muted = 1;
        stream >> muted;
        if (!stream || (muted != 0 && muted != 1)) {
            log(diagnostics, L"Control: invalid input_muted ignored");
            return false;
        }
        if (input_muted != nullptr) input_muted->store(muted != 0, std::memory_order_relaxed);
        log(diagnostics, muted != 0 ? L"Control: input muted" : L"Control: input open");
        return true;
    }
    if (command == "peer_talk") {
        std::size_t index = 0;
        int active = 0;
        stream >> index >> active;
        if (!stream || index >= peers.size() || peers[index] == nullptr ||
            (active != 0 && active != 1)) {
            log(diagnostics, L"Control: invalid peer_talk ignored");
            return false;
        }
        peers[index]->private_talk.store(active != 0, std::memory_order_relaxed);
        if (diagnostics != nullptr) {
            *diagnostics << L"Control: peer [" << index << L"] private talk "
                         << (active != 0 ? L"on" : L"off") << L'\n';
        }
        return true;
    }
    if (command != "peer_settings") {
        log(diagnostics, L"Control: unknown command ignored");
        return false;
    }

    std::size_t index = 0;
    RoomPeerOptions update;
    stream >> index >> update.gain >> update.self_duck_db >> update.self_duck_threshold
           >> update.self_duck_attack_ms >> update.self_duck_hold_ms
           >> update.self_duck_release_ms;
    if (!stream || index >= peers.size() || peers[index] == nullptr ||
        !valid_room_peer_settings(update)) {
        log(diagnostics, L"Control: invalid peer_settings ignored");
        return false;
    }

    int global_ptt_enabled = -1;
    if (stream >> global_ptt_enabled) {
        if (global_ptt_enabled != 0 && global_ptt_enabled != 1) {
            log(diagnostics, L"Control: invalid peer_settings global PTT value ignored");
            return false;
        }
        update.global_ptt_enabled = global_ptt_enabled != 0;
    } else {
        stream.clear();
        update.global_ptt_enabled =
            peers[index]->global_ptt_enabled.load(std::memory_order_relaxed);
    }
    initialize_room_peer_control(*peers[index], update);
    if (diagnostics != nullptr) {
        *diagnostics << L"Control: peer [" << index << L"] live settings updated"
                     << L", gain=" << update.gain
                     << L", duck-db=" << update.self_duck_db
                     << L", threshold=" << update.self_duck_threshold
                     << L", global-ptt=" << (update.global_ptt_enabled ? L"on" : L"off")
                     << L'\n';
    }
    return true;
}

void consume_room_control_bytes(
    std::string_view bytes,
    common::LineBuffer& lines,
    std::span<RoomPeerControl* const> peers,
    std::atomic_bool* input_muted,
    std::atomic_bool* stop,
    std::wostream* diagnostics) {
    lines.append(bytes);
    std::string line;
    while (lines.next(line)) {
        apply_room_control_line(line, peers, input_muted, stop, diagnostics);
    }
}

} // namespace lanspeak::core
