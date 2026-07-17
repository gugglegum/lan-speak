#include "gui/telemetry_protocol.h"

#include <sstream>
#include <string>
#include <vector>

namespace lanspeak::gui {
namespace {

std::vector<std::string> split_tab_fields(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        const std::size_t separator = line.find('\t', start);
        if (separator == std::string::npos) {
            fields.emplace_back(line.substr(start));
            return fields;
        }
        fields.emplace_back(line.substr(start, separator - start));
        start = separator + 1;
    }
}

std::string unescape_telemetry_field(const std::string& value) {
    std::string unescaped;
    unescaped.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (ch != '\\' || index + 1 >= value.size()) {
            unescaped.push_back(ch);
            continue;
        }
        const char next = value[++index];
        switch (next) {
        case 't': unescaped.push_back('\t'); break;
        case 'r': unescaped.push_back('\r'); break;
        case 'n': unescaped.push_back('\n'); break;
        case '\\': unescaped.push_back('\\'); break;
        default: unescaped.push_back(next); break;
        }
    }
    return unescaped;
}

template <typename Value>
bool parse_number(const std::string& field, Value& value) {
    std::istringstream stream(field);
    stream.imbue(std::locale::classic());
    stream >> value;
    return stream && stream.peek() == std::char_traits<char>::eof();
}

bool parse_audio_endpoint_line(
    const std::string& line,
    AudioEndpointTelemetry& endpoint) {
    const std::vector<std::string> fields = split_tab_fields(line);
    if (fields.size() != 10) {
        return false;
    }

    unsigned int channels = 0;
    unsigned int bits_per_sample = 0;
    int low_latency_shared = 0;
    AudioEndpointTelemetry parsed;
    parsed.name_utf8 = unescape_telemetry_field(fields[1]);
    if (!parse_number(fields[2], parsed.sample_rate) ||
        !parse_number(fields[3], channels) ||
        !parse_number(fields[4], bits_per_sample) ||
        !parse_number(fields[5], parsed.engine_period_ms) ||
        !parse_number(fields[6], parsed.buffer_ms) ||
        !parse_number(fields[7], parsed.stream_latency_ms) ||
        !parse_number(fields[8], low_latency_shared) ||
        !parse_number(fields[9], parsed.current_padding_ms) ||
        channels > UINT16_MAX || bits_per_sample > UINT16_MAX) {
        return false;
    }
    parsed.channels = static_cast<std::uint16_t>(channels);
    parsed.bits_per_sample = static_cast<std::uint16_t>(bits_per_sample);
    parsed.low_latency_shared = low_latency_shared != 0;
    parsed.valid = true;
    endpoint = std::move(parsed);
    return true;
}

} // namespace

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
    if (line.rfind("audio_input\t", 0) == 0) {
        return parse_audio_endpoint_line(line, snapshot_.capture);
    }
    if (line.rfind("audio_output\t", 0) == 0) {
        return parse_audio_endpoint_line(line, snapshot_.render);
    }

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
