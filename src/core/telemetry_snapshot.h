#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lanspeak::core {

class TelemetrySnapshot {
public:
    explicit TelemetrySnapshot(std::size_t peer_count);
    TelemetrySnapshot(const TelemetrySnapshot&) = delete;
    TelemetrySnapshot& operator=(const TelemetrySnapshot&) = delete;

    void accumulate_local_peak(double peak);
    void update_peer_meter(std::size_t peer_index, double level_db, bool voice_active);
    void mark_peer_stream(std::size_t peer_index, std::uint64_t now_ms);

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
};

} // namespace lanspeak::core
