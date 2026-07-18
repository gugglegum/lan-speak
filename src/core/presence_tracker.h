#pragma once

#include "core/udp_presence_packet.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lanspeak::core {

enum class PeerPresenceState : std::uint8_t {
    unknown = 0,
    online = 1,
    offline = 2
};

struct PeerPresenceSnapshot {
    PeerPresenceState state = PeerPresenceState::unknown;
    std::uint64_t remote_session_id = 0;
    std::uint64_t last_seen_ms = 0;
    double rtt_ms = -1.0;
};

struct PresenceAction {
    std::size_t peer_index = 0;
    UdpPresenceType type = UdpPresenceType::ping;
    std::uint64_t nonce = 0;
};

class PresenceTracker {
public:
    PresenceTracker(std::size_t peer_count, std::uint64_t session_id, std::uint64_t random_seed);

    [[nodiscard]] std::uint64_t session_id() const;
    void start(std::uint64_t now_ms);
    std::span<const PresenceAction> tick(std::uint64_t now_ms);
    std::optional<PresenceAction> on_packet(
        std::size_t peer_index,
        const UdpPresencePacket& packet,
        std::uint64_t now_ms);
    void on_audio_packet(std::size_t peer_index, std::uint64_t now_ms);
    std::span<const PresenceAction> shutdown();
    [[nodiscard]] PeerPresenceSnapshot snapshot(std::size_t peer_index) const;

private:
    struct PeerState {
        PeerPresenceSnapshot visible;
        std::uint64_t next_probe_at_ms = 0;
        std::uint64_t ping_started_at_ms = 0;
        std::uint64_t retry_at_ms = 0;
        std::uint64_t pending_nonce = 0;
        unsigned int attempts = 0;
        bool awaiting_pong = false;
    };

    std::uint64_t next_random();
    std::uint64_t random_interval(std::uint64_t minimum_ms, std::uint64_t maximum_ms);
    std::uint64_t next_nonce();
    void mark_online(PeerState& peer, std::uint64_t now_ms);
    void schedule_offline_probe(PeerState& peer, std::uint64_t now_ms);
    void emit_ping(std::size_t peer_index, PeerState& peer, std::uint64_t now_ms);

    std::vector<PeerState> peers_;
    std::vector<PresenceAction> actions_;
    std::uint64_t session_id_ = 1;
    std::uint64_t random_state_ = 1;
    bool started_ = false;
};

} // namespace lanspeak::core
