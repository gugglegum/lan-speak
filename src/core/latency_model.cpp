#include "core/latency_model.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lanspeak::core {
namespace {

constexpr std::uint32_t kWarmupSamples = 32;
constexpr double kEwmaAlpha = 0.1;
constexpr double kMaximumMeasuredCaptureUs = 500'000.0;
constexpr std::uint64_t kFallbackDelayMs = 2'000;

std::uint32_t rounded_us(double value) {
    if (!std::isfinite(value) || value <= 0.0) return 0;
    return static_cast<std::uint32_t>(std::clamp(
        std::llround(value),
        1LL,
        static_cast<long long>(std::numeric_limits<std::uint32_t>::max())));
}

} // namespace

std::optional<double> capture_midpoint_age_us(
    std::uint64_t capture_qpc_position_100ns,
    std::uint32_t packet_frames,
    std::uint32_t sample_rate,
    std::uint64_t send_qpc,
    std::uint64_t qpc_frequency,
    bool timestamp_error) {
    if (timestamp_error || capture_qpc_position_100ns == 0 || packet_frames == 0 ||
        sample_rate == 0 || qpc_frequency == 0) {
        return std::nullopt;
    }

    const long double send_100ns =
        static_cast<long double>(send_qpc) * 10'000'000.0L /
        static_cast<long double>(qpc_frequency);
    const long double packet_100ns =
        static_cast<long double>(packet_frames) * 10'000'000.0L /
        static_cast<long double>(sample_rate);
    const long double age_us =
        (send_100ns - static_cast<long double>(capture_qpc_position_100ns) -
         packet_100ns / 2.0L) /
        10.0L;
    if (age_us <= 0.0L || age_us > kMaximumMeasuredCaptureUs) return std::nullopt;
    return static_cast<double>(age_us);
}

CaptureLatencyEstimator::CaptureLatencyEstimator(
    std::uint64_t start_qpc,
    std::uint64_t qpc_frequency,
    std::uint32_t fallback_capture_us)
    : start_qpc_(start_qpc),
      qpc_frequency_(qpc_frequency),
      fallback_capture_us_(fallback_capture_us) {}

void CaptureLatencyEstimator::add_packet(
    std::uint64_t capture_qpc_position_100ns,
    std::uint32_t packet_frames,
    std::uint32_t sample_rate,
    std::uint64_t send_qpc,
    bool timestamp_error) {
    if (packet_frames != 0 && sample_rate != 0) {
        const double duration_us =
            static_cast<double>(packet_frames) * 1'000'000.0 / sample_rate;
        packet_duration_ewma_us_ = packet_duration_ewma_us_ == 0.0
            ? duration_us
            : packet_duration_ewma_us_ + kEwmaAlpha * (duration_us - packet_duration_ewma_us_);
    }

    const std::optional<double> measurement = capture_midpoint_age_us(
        capture_qpc_position_100ns,
        packet_frames,
        sample_rate,
        send_qpc,
        qpc_frequency_,
        timestamp_error);
    if (!measurement) return;
    capture_ewma_us_ = valid_samples_ == 0
        ? *measurement
        : capture_ewma_us_ + kEwmaAlpha * (*measurement - capture_ewma_us_);
    ++valid_samples_;
}

bool CaptureLatencyEstimator::fallback_ready(std::uint64_t now_qpc) const {
    if (fallback_capture_us_ == 0 || qpc_frequency_ == 0 || now_qpc < start_qpc_) return false;
    const std::uint64_t elapsed_ms =
        (now_qpc - start_qpc_) * 1'000 / qpc_frequency_;
    return elapsed_ms >= kFallbackDelayMs;
}

bool CaptureLatencyEstimator::ready(std::uint64_t now_qpc) const {
    return valid_samples_ >= kWarmupSamples || fallback_ready(now_qpc);
}

std::uint32_t CaptureLatencyEstimator::capture_to_send_us(std::uint64_t now_qpc) const {
    if (valid_samples_ >= kWarmupSamples) return rounded_us(capture_ewma_us_);
    return fallback_ready(now_qpc) ? fallback_capture_us_ : 0;
}

std::uint32_t CaptureLatencyEstimator::packet_duration_us() const {
    return rounded_us(packet_duration_ewma_us_);
}

std::uint32_t CaptureLatencyEstimator::valid_sample_count() const {
    return valid_samples_;
}

DirectionalLatencyEstimate estimate_directional_latency(
    const PeerLatencyProfile* local_profile,
    const PeerLatencyProfile* remote_profile,
    double rtt_ms) {
    DirectionalLatencyEstimate result;
    if (!std::isfinite(rtt_ms) || rtt_ms < 0.0) return result;
    const double one_way_network_ms = rtt_ms / 2.0;

    if (local_profile != nullptr && remote_profile != nullptr &&
        remote_profile->capture_to_send_us != 0 && local_profile->receive_buffer_us != 0 &&
        local_profile->render_latency_us != 0) {
        result.incoming_ms =
            (remote_profile->capture_to_send_us + local_profile->receive_buffer_us +
             local_profile->render_latency_us) /
                1000.0 +
            one_way_network_ms;
    }
    if (local_profile != nullptr && remote_profile != nullptr &&
        local_profile->capture_to_send_us != 0 && remote_profile->receive_buffer_us != 0 &&
        remote_profile->render_latency_us != 0) {
        result.outgoing_ms =
            (local_profile->capture_to_send_us + remote_profile->receive_buffer_us +
             remote_profile->render_latency_us) /
                1000.0 +
            one_way_network_ms;
    }
    return result;
}

} // namespace lanspeak::core
