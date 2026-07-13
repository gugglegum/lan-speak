#include "core/pcm_audio.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lanspeak::core {

std::int16_t float_to_pcm16(double value) {
    const double clamped = std::clamp(value, -1.0, 1.0);
    const long rounded = std::lround(clamped * (clamped < 0.0 ? 32768.0 : 32767.0));
    return static_cast<std::int16_t>(std::clamp<long>(rounded, -32768, 32767));
}

std::int16_t read_int16_le(const std::byte* data) {
    const std::uint16_t low = std::to_integer<std::uint8_t>(data[0]);
    const std::uint16_t high = std::to_integer<std::uint8_t>(data[1]);
    return static_cast<std::int16_t>(low | (high << 8u));
}

void write_int16_le(std::byte* destination, std::int16_t value) {
    const auto bits = static_cast<std::uint16_t>(value);
    destination[0] = static_cast<std::byte>(bits & 0xffu);
    destination[1] = static_cast<std::byte>((bits >> 8u) & 0xffu);
}

bool decode_mono_pcm16_le(
    std::span<const std::byte> payload,
    std::vector<std::int16_t>& output) {
    if (payload.size() % sizeof(std::int16_t) != 0) {
        output.clear();
        return false;
    }

    const std::size_t frame_count = payload.size() / sizeof(std::int16_t);
    output.resize(frame_count);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        output[frame] = read_int16_le(payload.data() + frame * sizeof(std::int16_t));
    }
    return true;
}

bool resample_mono_pcm16_linear(
    std::span<const std::int16_t> input,
    std::uint32_t input_sample_rate,
    std::uint32_t output_sample_rate,
    std::vector<std::int16_t>& output) {
    output.clear();
    if (input.empty() || input_sample_rate == 0 || output_sample_rate == 0) {
        return false;
    }
    if (input_sample_rate == output_sample_rate) {
        output.assign(input.begin(), input.end());
        return true;
    }

    const long double exact_frames =
        static_cast<long double>(input.size()) * output_sample_rate / input_sample_rate;
    if (exact_frames > static_cast<long double>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    const std::size_t output_frames =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(exact_frames)));
    output.resize(output_frames);

    const long double source_step =
        static_cast<long double>(input_sample_rate) / output_sample_rate;
    for (std::size_t frame = 0; frame < output_frames; ++frame) {
        const long double source_position = static_cast<long double>(frame) * source_step;
        const std::size_t left = std::min<std::size_t>(
            static_cast<std::size_t>(source_position), input.size() - 1);
        const std::size_t right = std::min(left + 1, input.size() - 1);
        const long double fraction = source_position - static_cast<long double>(left);
        const long double sample =
            static_cast<long double>(input[left]) * (1.0L - fraction) +
            static_cast<long double>(input[right]) * fraction;
        output[frame] = static_cast<std::int16_t>(std::clamp<long>(
            std::lround(sample), -32768, 32767));
    }
    return true;
}

} // namespace lanspeak::core
