#pragma once

#include "core/latency_model.h"
#include "core/presence_tracker.h"
#include "core/udp_peer_info_packet.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lanspeak::core {

struct PeerInfoAction {
    std::size_t peer_index = 0;
    UdpPeerInfoPacket packet;
};

struct PeerInfoSnapshot {
    bool valid = false;
    std::uint64_t remote_session_id = 0;
    std::uint32_t remote_revision = 0;
    PeerLatencyProfile remote_profile;
};

class PeerInfoTracker {
public:
    PeerInfoTracker(std::size_t peer_count, std::uint64_t local_session_id);

    void set_local_profile(
        std::size_t peer_index,
        const PeerLatencyProfile& profile,
        std::uint64_t now_ms);
    void update_presence(
        std::size_t peer_index,
        PeerPresenceState state,
        std::uint64_t remote_session_id,
        std::uint64_t now_ms);
    std::span<const PeerInfoAction> tick(std::uint64_t now_ms);
    std::span<const PeerInfoAction> on_packet(
        std::size_t peer_index,
        const UdpPeerInfoPacket& packet,
        std::uint64_t now_ms);

    [[nodiscard]] PeerInfoSnapshot snapshot(std::size_t peer_index) const;
    [[nodiscard]] const PeerLatencyProfile* local_profile(std::size_t peer_index) const;

private:
    struct PeerState {
        bool online = false;
        std::uint64_t remote_session_id = 0;
        PeerInfoSnapshot remote;
        bool local_valid = false;
        PeerLatencyProfile local;
        std::uint32_t local_revision = 0;
        bool info_acked = false;
        bool info_exhausted = false;
        bool awaiting_ack = false;
        unsigned int info_attempts = 0;
        std::uint64_t info_retry_at_ms = 0;
        unsigned int request_attempts = 0;
        std::uint64_t request_at_ms = 0;
    };

    void clear_remote(PeerState& peer);
    void queue_info(std::size_t peer_index, PeerState& peer, std::uint64_t now_ms);
    void queue_request(std::size_t peer_index, PeerState& peer, std::uint64_t now_ms);
    void queue_ack(std::size_t peer_index, const UdpPeerInfoPacket& info);

    std::vector<PeerState> peers_;
    std::vector<PeerInfoAction> actions_;
    std::uint64_t local_session_id_ = 1;
};

} // namespace lanspeak::core
