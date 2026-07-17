#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace lanspeak::core {

enum class AudioEndpointKind {
    capture,
    render
};

struct AudioEndpointDiagnostics {
    bool valid = false;
    std::string name_utf8;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    double engine_period_ms = -1.0;
    double buffer_ms = -1.0;
    double stream_latency_ms = -1.0;
    bool low_latency_shared = false;
};

class TelemetrySnapshot {
public:
    explicit TelemetrySnapshot(std::size_t peer_count);
    TelemetrySnapshot(const TelemetrySnapshot&) = delete;
    TelemetrySnapshot& operator=(const TelemetrySnapshot&) = delete;

    void accumulate_local_peak(double peak);
    void update_peer_meter(std::size_t peer_index, double level_db, bool voice_active);
    void mark_peer_stream(std::size_t peer_index, std::uint64_t now_ms);
    void set_audio_endpoint_diagnostics(
        AudioEndpointKind kind,
        AudioEndpointDiagnostics diagnostics);
    void update_render_padding_ms(double padding_ms);

    std::string serialize(
        std::uint64_t now_ms,
        std::uint64_t stream_hold_ms = 200);

private:
    struct PeerMeter {
        std::atomic<double> level_db{-90.0};
        std::atomic_bool voice_active{false};
        std::atomic<std::uint64_t> last_stream_packet_ms{0};
    };

    std::atomic<double> local_pending_peak_{0.0};
    std::unique_ptr<PeerMeter[]> peers_;
    std::size_t peer_count_ = 0;
    std::mutex audio_diagnostics_mutex_;
    AudioEndpointDiagnostics capture_diagnostics_;
    AudioEndpointDiagnostics render_diagnostics_;
    std::atomic<double> render_padding_ms_{-1.0};
};

} // namespace lanspeak::core
