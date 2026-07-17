#include "core/telemetry_snapshot.h"

#include "core/audio_math.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string_view>

namespace lanspeak::core {
namespace {

std::string escape_telemetry_field(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
        case '\\': escaped += "\\\\"; break;
        case '\t': escaped += "\\t"; break;
        case '\r': escaped += "\\r"; break;
        case '\n': escaped += "\\n"; break;
        default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

void serialize_audio_endpoint(
    std::ostringstream& stream,
    std::string_view kind,
    const AudioEndpointDiagnostics& diagnostics,
    double padding_ms) {
    if (!diagnostics.valid) {
        return;
    }
    stream << std::setprecision(3)
           << kind << "\t"
           << escape_telemetry_field(diagnostics.name_utf8) << "\t"
           << diagnostics.sample_rate << "\t"
           << diagnostics.channels << "\t"
           << diagnostics.bits_per_sample << "\t"
           << diagnostics.engine_period_ms << "\t"
           << diagnostics.buffer_ms << "\t"
           << diagnostics.stream_latency_ms << "\t"
           << (diagnostics.low_latency_shared ? 1 : 0) << "\t"
           << padding_ms << "\n";
}

} // namespace

TelemetrySnapshot::TelemetrySnapshot(std::size_t peer_count)
    : peers_(peer_count == 0 ? nullptr : std::make_unique<PeerMeter[]>(peer_count)),
      peer_count_(peer_count) {}

void TelemetrySnapshot::accumulate_local_peak(double peak) {
    if (!std::isfinite(peak) || peak <= 0.0) {
        return;
    }
    double current = local_pending_peak_.load(std::memory_order_relaxed);
    while (current < peak &&
           !local_pending_peak_.compare_exchange_weak(
               current,
               peak,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

void TelemetrySnapshot::update_peer_meter(
    std::size_t peer_index,
    double level_db,
    bool voice_active) {
    if (peer_index >= peer_count_) {
        return;
    }
    peers_[peer_index].level_db.store(level_db, std::memory_order_relaxed);
    peers_[peer_index].voice_active.store(voice_active, std::memory_order_relaxed);
}

void TelemetrySnapshot::mark_peer_stream(std::size_t peer_index, std::uint64_t now_ms) {
    if (peer_index < peer_count_) {
        peers_[peer_index].last_stream_packet_ms.store(now_ms, std::memory_order_relaxed);
    }
}

void TelemetrySnapshot::set_audio_endpoint_diagnostics(
    AudioEndpointKind kind,
    AudioEndpointDiagnostics diagnostics) {
    std::lock_guard lock(audio_diagnostics_mutex_);
    if (kind == AudioEndpointKind::capture) {
        capture_diagnostics_ = std::move(diagnostics);
    } else {
        render_diagnostics_ = std::move(diagnostics);
    }
}

void TelemetrySnapshot::update_render_padding_ms(double padding_ms) {
    render_padding_ms_.store(padding_ms, std::memory_order_relaxed);
}

std::string TelemetrySnapshot::serialize(
    std::uint64_t now_ms,
    std::uint64_t stream_hold_ms) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(1);

    const double local_peak = local_pending_peak_.exchange(0.0, std::memory_order_relaxed);
    stream << "local_level\t" << audio_level_dbfs(local_peak) << "\t"
           << (local_peak >= 0.015 ? 1 : 0) << "\n";
    for (std::size_t index = 0; index < peer_count_; ++index) {
        const std::uint64_t last_packet =
            peers_[index].last_stream_packet_ms.load(std::memory_order_relaxed);
        const bool stream_active = last_packet != 0 && now_ms >= last_packet &&
            now_ms - last_packet <= stream_hold_ms;
        stream << "peer_level\t" << index << "\t"
               << peers_[index].level_db.load(std::memory_order_relaxed) << "\t"
               << (peers_[index].voice_active.load(std::memory_order_relaxed) ? 1 : 0) << "\t"
               << (stream_active ? 1 : 0) << "\n";
    }
    {
        std::lock_guard lock(audio_diagnostics_mutex_);
        serialize_audio_endpoint(stream, "audio_input", capture_diagnostics_, -1.0);
        serialize_audio_endpoint(
            stream,
            "audio_output",
            render_diagnostics_,
            render_padding_ms_.load(std::memory_order_relaxed));
    }
    return stream.str();
}

} // namespace lanspeak::core
