#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lanspeak::core {

struct PeerRouteState {
    bool global_ptt_enabled = true;
    bool private_ptt_active = false;
    std::uint64_t next_sequence = 0;
};

bool should_send_voice_to_peer(bool input_muted, const PeerRouteState& peer);
std::size_t count_voice_targets(bool input_muted, std::span<const PeerRouteState> peers);

class PeerRouter {
public:
    explicit PeerRouter(std::size_t peer_capacity = 0);

    void begin(bool input_muted);
    void consider(std::size_t peer_index, const PeerRouteState& peer);
    [[nodiscard]] std::span<const std::size_t> targets() const;

private:
    bool input_muted_ = true;
    std::vector<std::size_t> targets_;
};

} // namespace lanspeak::core
