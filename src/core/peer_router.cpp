#include "core/peer_router.h"

namespace lanspeak::core {

bool should_send_voice_to_peer(bool input_muted, const PeerRouteState& peer) {
    return peer.private_ptt_active || (!input_muted && peer.global_ptt_enabled);
}

std::size_t count_voice_targets(bool input_muted, std::span<const PeerRouteState> peers) {
    std::size_t count = 0;
    for (const PeerRouteState& peer : peers) {
        count += should_send_voice_to_peer(input_muted, peer) ? 1u : 0u;
    }
    return count;
}

PeerRouter::PeerRouter(std::size_t peer_capacity) {
    targets_.reserve(peer_capacity);
}

void PeerRouter::begin(bool input_muted) {
    input_muted_ = input_muted;
    targets_.clear();
}

void PeerRouter::consider(std::size_t peer_index, const PeerRouteState& peer) {
    if (should_send_voice_to_peer(input_muted_, peer)) {
        targets_.push_back(peer_index);
    }
}

std::span<const std::size_t> PeerRouter::targets() const {
    return targets_;
}

} // namespace lanspeak::core
