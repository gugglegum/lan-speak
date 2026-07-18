#include "core/peer_info_tracker.h"

#include <algorithm>

namespace lanspeak::core {
namespace {

bool same_profile(const PeerLatencyProfile& left, const PeerLatencyProfile& right) {
    return left.capture_to_send_us == right.capture_to_send_us &&
        left.render_latency_us == right.render_latency_us &&
        left.packet_duration_us == right.packet_duration_us &&
        left.receive_buffer_us == right.receive_buffer_us;
}

UdpPeerInfoPacket info_packet(
    std::uint64_t session_id,
    std::uint32_t revision,
    const PeerLatencyProfile& profile) {
    UdpPeerInfoPacket packet{};
    packet.type = UdpPeerInfoType::info;
    packet.session_id = session_id;
    packet.revision = revision;
    if (profile.capture_to_send_us != 0) {
        packet.flags |= kPeerInfoCaptureToSendValid;
        packet.capture_to_send_us = profile.capture_to_send_us;
    }
    if (profile.render_latency_us != 0) {
        packet.flags |= kPeerInfoRenderLatencyValid;
        packet.render_latency_us = profile.render_latency_us;
    }
    if (profile.packet_duration_us != 0) {
        packet.flags |= kPeerInfoPacketDurationValid;
        packet.packet_duration_us = profile.packet_duration_us;
    }
    if (profile.receive_buffer_us != 0) {
        packet.flags |= kPeerInfoReceiveBufferValid;
        packet.receive_buffer_us = profile.receive_buffer_us;
    }
    return packet;
}

PeerLatencyProfile profile_from_packet(const UdpPeerInfoPacket& packet) {
    return PeerLatencyProfile{
        (packet.flags & kPeerInfoCaptureToSendValid) != 0 ? packet.capture_to_send_us : 0,
        (packet.flags & kPeerInfoRenderLatencyValid) != 0 ? packet.render_latency_us : 0,
        (packet.flags & kPeerInfoPacketDurationValid) != 0 ? packet.packet_duration_us : 0,
        (packet.flags & kPeerInfoReceiveBufferValid) != 0 ? packet.receive_buffer_us : 0};
}

} // namespace

PeerInfoTracker::PeerInfoTracker(std::size_t peer_count, std::uint64_t local_session_id)
    : peers_(peer_count),
      local_session_id_(local_session_id == 0 ? 1 : local_session_id) {
    actions_.reserve(peer_count * 2);
}

void PeerInfoTracker::set_local_profile(
    std::size_t peer_index,
    const PeerLatencyProfile& profile,
    std::uint64_t now_ms) {
    if (peer_index >= peers_.size()) return;
    PeerState& peer = peers_[peer_index];
    if (peer.local_valid && same_profile(peer.local, profile)) return;

    peer.local = profile;
    peer.local_valid = true;
    peer.local_revision = peer.local_revision == UINT32_MAX ? 1 : peer.local_revision + 1;
    peer.awaiting_ack = false;
    peer.info_attempts = 0;
    peer.info_acked = false;
    peer.info_exhausted = false;
    if (peer.online) peer.info_retry_at_ms = now_ms;
}

void PeerInfoTracker::update_presence(
    std::size_t peer_index,
    PeerPresenceState state,
    std::uint64_t remote_session_id,
    std::uint64_t now_ms) {
    if (peer_index >= peers_.size()) return;
    PeerState& peer = peers_[peer_index];
    if (state != PeerPresenceState::online) {
        if (peer.online || peer.remote.valid) clear_remote(peer);
        peer.online = false;
        peer.remote_session_id = 0;
        peer.awaiting_ack = false;
        peer.info_acked = false;
        peer.info_exhausted = false;
        return;
    }

    const bool session_changed = remote_session_id != 0 &&
        remote_session_id != peer.remote_session_id;
    const bool became_online = !peer.online;
    if (session_changed) {
        clear_remote(peer);
        peer.awaiting_ack = false;
        peer.info_acked = false;
        peer.info_exhausted = false;
    }
    peer.online = true;
    if (remote_session_id != 0) peer.remote_session_id = remote_session_id;

    if (became_online || session_changed) {
        peer.request_attempts = 0;
        peer.request_at_ms = peer.remote_session_id != 0 ? now_ms + 1'000 : 0;
        if (peer.local_valid) {
            peer.awaiting_ack = false;
            peer.info_attempts = 0;
            peer.info_retry_at_ms = now_ms;
        }
    }
}

std::span<const PeerInfoAction> PeerInfoTracker::tick(std::uint64_t now_ms) {
    actions_.clear();
    for (std::size_t index = 0; index < peers_.size(); ++index) {
        PeerState& peer = peers_[index];
        if (!peer.online) continue;

        if (peer.local_valid && !peer.info_acked && !peer.info_exhausted &&
            (!peer.awaiting_ack || now_ms >= peer.info_retry_at_ms)) {
            if (!peer.awaiting_ack || peer.info_attempts < 3) {
                queue_info(index, peer, now_ms);
            } else {
                peer.awaiting_ack = false;
                peer.info_exhausted = true;
            }
        }

        if (!peer.remote.valid && peer.remote_session_id != 0 &&
            peer.request_at_ms != 0 && now_ms >= peer.request_at_ms &&
            peer.request_attempts < 3) {
            queue_request(index, peer, now_ms);
        }
    }
    return actions_;
}

std::span<const PeerInfoAction> PeerInfoTracker::on_packet(
    std::size_t peer_index,
    const UdpPeerInfoPacket& packet,
    std::uint64_t now_ms) {
    actions_.clear();
    if (peer_index >= peers_.size()) return actions_;
    PeerState& peer = peers_[peer_index];
    if (!peer.online) return actions_;

    if (packet.type == UdpPeerInfoType::info) {
        if (peer.remote_session_id != 0 && packet.session_id != peer.remote_session_id) {
            return actions_;
        }
        if (peer.remote_session_id == 0) peer.remote_session_id = packet.session_id;
        if (!peer.remote.valid || packet.revision > peer.remote.remote_revision) {
            peer.remote.valid = true;
            peer.remote.remote_session_id = packet.session_id;
            peer.remote.remote_revision = packet.revision;
            peer.remote.remote_profile = profile_from_packet(packet);
            peer.request_attempts = 0;
            peer.request_at_ms = 0;
        } else if (packet.revision < peer.remote.remote_revision) {
            return actions_;
        }
        queue_ack(peer_index, packet);
        return actions_;
    }

    if (packet.type == UdpPeerInfoType::ack) {
        if (peer.local_valid && packet.session_id == local_session_id_ &&
            packet.revision == peer.local_revision) {
            peer.awaiting_ack = false;
            peer.info_attempts = 0;
            peer.info_acked = true;
            peer.info_exhausted = false;
        }
        return actions_;
    }

    if (packet.session_id == peer.remote_session_id && peer.local_valid) {
        peer.info_acked = false;
        peer.info_exhausted = false;
        peer.info_attempts = 0;
        queue_info(peer_index, peer, now_ms);
    }
    return actions_;
}

PeerInfoSnapshot PeerInfoTracker::snapshot(std::size_t peer_index) const {
    return peer_index < peers_.size() ? peers_[peer_index].remote : PeerInfoSnapshot{};
}

const PeerLatencyProfile* PeerInfoTracker::local_profile(std::size_t peer_index) const {
    if (peer_index >= peers_.size() || !peers_[peer_index].local_valid) return nullptr;
    return &peers_[peer_index].local;
}

void PeerInfoTracker::clear_remote(PeerState& peer) {
    peer.remote = {};
    peer.request_attempts = 0;
    peer.request_at_ms = 0;
}

void PeerInfoTracker::queue_info(
    std::size_t peer_index,
    PeerState& peer,
    std::uint64_t now_ms) {
    actions_.push_back(PeerInfoAction{
        peer_index,
        info_packet(local_session_id_, peer.local_revision, peer.local)});
    peer.awaiting_ack = true;
    ++peer.info_attempts;
    if (peer.info_attempts == 1) {
        peer.info_retry_at_ms = now_ms + 1'000;
    } else if (peer.info_attempts == 2) {
        peer.info_retry_at_ms = now_ms + 2'000;
    } else {
        peer.info_retry_at_ms = 0;
    }
}

void PeerInfoTracker::queue_request(
    std::size_t peer_index,
    PeerState& peer,
    std::uint64_t now_ms) {
    UdpPeerInfoPacket packet{};
    packet.type = UdpPeerInfoType::request;
    packet.session_id = local_session_id_;
    packet.revision = 1;
    actions_.push_back(PeerInfoAction{peer_index, packet});
    ++peer.request_attempts;
    if (peer.request_attempts == 1) {
        peer.request_at_ms = now_ms + 2'000;
    } else if (peer.request_attempts == 2) {
        peer.request_at_ms = now_ms + 5'000;
    } else {
        peer.request_at_ms = 0;
    }
}

void PeerInfoTracker::queue_ack(
    std::size_t peer_index,
    const UdpPeerInfoPacket& info) {
    UdpPeerInfoPacket packet{};
    packet.type = UdpPeerInfoType::ack;
    packet.session_id = info.session_id;
    packet.revision = info.revision;
    actions_.push_back(PeerInfoAction{peer_index, packet});
}

} // namespace lanspeak::core
