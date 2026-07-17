#pragma once

#include "common/line_buffer.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lanspeak::gui {

struct MeterTelemetry {
    bool valid = false;
    double level_db = -90.0;
    bool voice_active = false;
    bool stream_active = false;
};

struct AudioEndpointTelemetry {
    bool valid = false;
    std::string name_utf8;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    double engine_period_ms = -1.0;
    double buffer_ms = -1.0;
    double stream_latency_ms = -1.0;
    bool low_latency_shared = false;
    double current_padding_ms = -1.0;
};

struct TelemetrySnapshot {
    MeterTelemetry local;
    std::vector<MeterTelemetry> peers;
    AudioEndpointTelemetry capture;
    AudioEndpointTelemetry render;
    std::uint64_t revision = 0;
};

class TelemetryParser {
public:
    bool append(std::string_view bytes);
    [[nodiscard]] const TelemetrySnapshot& snapshot() const;
    void clear();

private:
    bool parse_line(const std::string& line);

    common::LineBuffer lines_;
    TelemetrySnapshot snapshot_;
};

} // namespace lanspeak::gui
