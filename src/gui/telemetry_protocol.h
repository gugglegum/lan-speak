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

struct TelemetrySnapshot {
    MeterTelemetry local;
    std::vector<MeterTelemetry> peers;
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
