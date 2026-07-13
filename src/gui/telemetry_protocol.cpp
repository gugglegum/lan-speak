#include "gui/telemetry_protocol.h"

#include <sstream>
#include <string>

namespace lanspeak::gui {

bool TelemetryParser::append(std::string_view bytes) {
    lines_.append(bytes);
    bool changed = false;
    std::string line;
    while (lines_.next(line)) {
        changed = parse_line(line) || changed;
    }
    if (changed) {
        ++snapshot_.revision;
    }
    return changed;
}

const TelemetrySnapshot& TelemetryParser::snapshot() const {
    return snapshot_;
}

void TelemetryParser::clear() {
    lines_.clear();
    snapshot_ = {};
}

bool TelemetryParser::parse_line(const std::string& line) {
    std::istringstream stream(line);
    stream.imbue(std::locale::classic());
    std::string kind;
    stream >> kind;
    if (kind == "local_level") {
        double level = -90.0;
        int active = 0;
        if (!(stream >> level >> active)) {
            return false;
        }
        snapshot_.local = MeterTelemetry{true, level, active != 0, active != 0};
        return true;
    }
    if (kind != "peer_level") {
        return false;
    }

    std::size_t index = 0;
    double level = -90.0;
    int active = 0;
    if (!(stream >> index >> level >> active)) {
        return false;
    }
    int stream_active = level > -90.0 || active != 0 ? 1 : 0;
    if (!(stream >> stream_active)) {
        stream.clear();
    }
    if (snapshot_.peers.size() <= index) {
        snapshot_.peers.resize(index + 1);
    }
    snapshot_.peers[index] = MeterTelemetry{
        true, level, active != 0, stream_active != 0};
    return true;
}

} // namespace lanspeak::gui
