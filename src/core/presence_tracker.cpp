#include "core/presence_tracker.h"

#include <algorithm>

namespace lanspeak::core {
namespace {

constexpr std::uint64_t kOnlineProbeMinimumMs = 25'000;
constexpr std::uint64_t kOnlineProbeMaximumMs = 35'000;
constexpr std::uint64_t kOfflineProbeMinimumMs = 60'000;
constexpr std::uint64_t kOfflineProbeMaximumMs = 120'000;

} // namespace

PresenceTracker::PresenceTracker(
    std::size_t peer_count,
    std::uint64_t session_id,
    std::uint64_t random_seed)
    : peers_(peer_count),
      session_id_(session_id == 0 ? 1 : session_id),
      random_state_(random_seed == 0 ? session_id_ : random_seed) {
    if (random_state_ == 0) random_state_ = 1;
    actions_.reserve(peer_count);
}

std::uint64_t PresenceTracker::session_id() const {
    return session_id_;
}

void PresenceTracker::start(std::uint64_t now_ms) {
    started_ = true;
    for (PeerState& peer : peers_) {
        peer.next_probe_at_ms = now_ms;
    }
}

std::span<const PresenceAction> PresenceTracker::tick(std::uint64_t now_ms) {
    actions_.clear();
    if (!started_) return actions_;

    for (std::size_t index = 0; index < peers_.size(); ++index) {
        PeerState& peer = peers_[index];
        if (peer.awaiting_pong) {
            if (now_ms < peer.retry_at_ms) continue;
            if (peer.attempts >= 3) {
                peer.awaiting_pong = false;
                if (peer.visible.state != PeerPresenceState::unknown) {
                    peer.visible.state = PeerPresenceState::offline;
                }
                peer.visible.rtt_ms = -1.0;
                schedule_offline_probe(peer, now_ms);
                continue;
            }
            actions_.push_back(PresenceAction{index, UdpPresenceType::ping, peer.pending_nonce});
            ++peer.attempts;
            peer.retry_at_ms = now_ms + 2'000;
            continue;
        }

        if (now_ms >= peer.next_probe_at_ms) {
            emit_ping(index, peer, now_ms);
        }
    }
    return actions_;
}

std::optional<PresenceAction> PresenceTracker::on_packet(
    std::size_t peer_index,
    const UdpPresencePacket& packet,
    std::uint64_t now_ms) {
    if (peer_index >= peers_.size()) return std::nullopt;
    PeerState& peer = peers_[peer_index];

    if (packet.type == UdpPresenceType::ping) {
        if (peer.visible.remote_session_id != packet.session_id) {
            peer.visible.rtt_ms = -1.0;
        }
        peer.visible.remote_session_id = packet.session_id;
        mark_online(peer, now_ms, true);
        if (!peer.awaiting_pong && peer.visible.rtt_ms < 0.0) {
            peer.next_probe_at_ms = now_ms;
        }
        return PresenceAction{peer_index, UdpPresenceType::pong, packet.nonce};
    }

    if (packet.type == UdpPresenceType::pong) {
        if (!peer.awaiting_pong || packet.nonce != peer.pending_nonce) {
            return std::nullopt;
        }
        peer.visible.remote_session_id = packet.session_id;
        peer.visible.rtt_ms = now_ms >= peer.ping_started_at_ms
            ? static_cast<double>(now_ms - peer.ping_started_at_ms)
            : -1.0;
        mark_online(peer, now_ms);
        return std::nullopt;
    }

    if (peer.visible.remote_session_id != 0 &&
        peer.visible.remote_session_id == packet.session_id) {
        peer.awaiting_pong = false;
        peer.visible.state = PeerPresenceState::offline;
        peer.visible.rtt_ms = -1.0;
        schedule_offline_probe(peer, now_ms);
    }
    return std::nullopt;
}

void PresenceTracker::on_audio_packet(std::size_t peer_index, std::uint64_t now_ms) {
    if (peer_index < peers_.size()) {
        mark_online(peers_[peer_index], now_ms, true);
    }
}

std::span<const PresenceAction> PresenceTracker::shutdown() {
    actions_.clear();
    if (!started_) return actions_;
    for (std::size_t index = 0; index < peers_.size(); ++index) {
        actions_.push_back(PresenceAction{index, UdpPresenceType::goodbye, 0});
    }
    started_ = false;
    return actions_;
}

PeerPresenceSnapshot PresenceTracker::snapshot(std::size_t peer_index) const {
    return peer_index < peers_.size() ? peers_[peer_index].visible : PeerPresenceSnapshot{};
}

std::uint64_t PresenceTracker::next_random() {
    std::uint64_t value = random_state_;
    value ^= value << 13u;
    value ^= value >> 7u;
    value ^= value << 17u;
    random_state_ = value == 0 ? 1 : value;
    return random_state_;
}

std::uint64_t PresenceTracker::random_interval(
    std::uint64_t minimum_ms,
    std::uint64_t maximum_ms) {
    return minimum_ms + next_random() % (maximum_ms - minimum_ms + 1);
}

std::uint64_t PresenceTracker::next_nonce() {
    const std::uint64_t nonce = next_random();
    return nonce == 0 ? 1 : nonce;
}

void PresenceTracker::mark_online(
    PeerState& peer,
    std::uint64_t now_ms,
    bool preserve_pending_probe) {
    peer.visible.state = PeerPresenceState::online;
    peer.visible.last_seen_ms = now_ms;
    if (preserve_pending_probe && peer.awaiting_pong) {
        return;
    }
    peer.awaiting_pong = false;
    peer.attempts = 0;
    peer.pending_nonce = 0;
    peer.next_probe_at_ms = now_ms + random_interval(kOnlineProbeMinimumMs, kOnlineProbeMaximumMs);
}

void PresenceTracker::schedule_offline_probe(PeerState& peer, std::uint64_t now_ms) {
    peer.next_probe_at_ms = now_ms + random_interval(kOfflineProbeMinimumMs, kOfflineProbeMaximumMs);
}

void PresenceTracker::emit_ping(
    std::size_t peer_index,
    PeerState& peer,
    std::uint64_t now_ms) {
    peer.awaiting_pong = true;
    peer.pending_nonce = next_nonce();
    peer.ping_started_at_ms = now_ms;
    peer.attempts = 1;
    peer.retry_at_ms = now_ms + 1'000;
    actions_.push_back(PresenceAction{peer_index, UdpPresenceType::ping, peer.pending_nonce});
}

} // namespace lanspeak::core
