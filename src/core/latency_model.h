#pragma once

#include <cstdint>
#include <optional>

namespace lanspeak::core {

struct PeerLatencyProfile {
    std::uint32_t capture_to_send_us = 0;
    std::uint32_t render_latency_us = 0;
    std::uint32_t packet_duration_us = 0;
    std::uint32_t receive_buffer_us = 0;
};

struct DirectionalLatencyEstimate {
    double incoming_ms = -1.0;
    double outgoing_ms = -1.0;
};

std::optional<double> capture_midpoint_age_us(
    std::uint64_t capture_qpc_position_100ns,
    std::uint32_t packet_frames,
    std::uint32_t sample_rate,
    std::uint64_t send_qpc,
    std::uint64_t qpc_frequency,
    bool timestamp_error);

class CaptureLatencyEstimator {
public:
    CaptureLatencyEstimator(
        std::uint64_t start_qpc,
        std::uint64_t qpc_frequency,
        std::uint32_t fallback_capture_us = 0);

    void add_packet(
        std::uint64_t capture_qpc_position_100ns,
        std::uint32_t packet_frames,
        std::uint32_t sample_rate,
        std::uint64_t send_qpc,
        bool timestamp_error);

    [[nodiscard]] bool ready(std::uint64_t now_qpc) const;
    [[nodiscard]] std::uint32_t capture_to_send_us(std::uint64_t now_qpc) const;
    [[nodiscard]] std::uint32_t packet_duration_us() const;
    [[nodiscard]] std::uint32_t valid_sample_count() const;

private:
    [[nodiscard]] bool fallback_ready(std::uint64_t now_qpc) const;

    std::uint64_t start_qpc_ = 0;
    std::uint64_t qpc_frequency_ = 0;
    std::uint32_t fallback_capture_us_ = 0;
    std::uint32_t valid_samples_ = 0;
    double capture_ewma_us_ = 0.0;
    double packet_duration_ewma_us_ = 0.0;
};

DirectionalLatencyEstimate estimate_directional_latency(
    const PeerLatencyProfile* local_profile,
    const PeerLatencyProfile* remote_profile,
    double rtt_ms);

} // namespace lanspeak::core
