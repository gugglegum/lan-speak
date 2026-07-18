#include "gui/telemetry_protocol.h"

#include <algorithm>
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
    if (line.rfind("discovery_peer\t", 0) == 0) {
        const std::vector<std::string> fields = split_tab_fields(line);
        DiscoveryPeerTelemetry peer;
        unsigned int port = 0;
        int already_contact = 0;
        if (fields.size() != 7 ||
            !parse_number(fields[1], peer.request_id) || peer.request_id == 0 ||
            !parse_number(fields[2], peer.session_id) || peer.session_id == 0 ||
            !parse_number(fields[4], port) || port == 0 || port > UINT16_MAX ||
            !parse_number(fields[5], already_contact) ||
            (already_contact != 0 && already_contact != 1)) {
            return false;
        }
        peer.ip_utf8 = unescape_telemetry_field(fields[3]);
        peer.voice_port = static_cast<std::uint16_t>(port);
        peer.already_contact = already_contact != 0;
        peer.computer_name_utf8 = unescape_telemetry_field(fields[6]);
        const auto existing = std::find_if(
            snapshot_.discovery_peers.begin(),
            snapshot_.discovery_peers.end(),
            [&](const DiscoveryPeerTelemetry& item) {
                return item.request_id == peer.request_id && item.session_id == peer.session_id;
            });
        if (existing == snapshot_.discovery_peers.end()) {
            snapshot_.discovery_peers.push_back(std::move(peer));
        } else {
            *existing = std::move(peer);
        }
        return true;
    }
    if (line.rfind("discovery_error\t", 0) == 0) {
        const std::vector<std::string> fields = split_tab_fields(line);
        DiscoveryErrorTelemetry error;
        if (fields.size() != 3 ||
            !parse_number(fields[1], error.request_id) || error.request_id == 0 ||
            !parse_number(fields[2], error.error_code) || error.error_code == 0) {
            return false;
        }
        error.valid = true;
        snapshot_.discovery_error = error;
        return true;
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
    if (kind == "peer_presence") {
        std::size_t index = 0;
        int state = 0;
        double rtt_ms = -1.0;
        if (!(stream >> index >> state >> rtt_ms) ||
            state < static_cast<int>(PeerPresenceState::unknown) ||
            state > static_cast<int>(PeerPresenceState::offline)) {
            return false;
        }
        if (snapshot_.peer_presence.size() <= index) {
            snapshot_.peer_presence.resize(index + 1);
        }
        snapshot_.peer_presence[index] = PresenceTelemetry{
            true,
            static_cast<PeerPresenceState>(state),
            rtt_ms};
        return true;
    }
    if (kind == "peer_latency") {
        std::size_t index = 0;
        double incoming_ms = -1.0;
        double outgoing_ms = -1.0;
        if (!(stream >> index >> incoming_ms >> outgoing_ms)) return false;
        if (snapshot_.peer_latency.size() <= index) {
            snapshot_.peer_latency.resize(index + 1);
        }
        snapshot_.peer_latency[index] = LatencyTelemetry{true, incoming_ms, outgoing_ms};
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
