#include "core/wasapi_audio.h"

#include "core/pcm_audio.h"

#include <algorithm>
#include <cmath>

namespace lanspeak::core {
namespace {

constexpr REFERENCE_TIME kReferenceTimePerSecond = 10'000'000;

void append_int16_le(std::vector<BYTE>& output, std::int16_t value) {
    output.push_back(static_cast<BYTE>(value & 0xff));
    output.push_back(static_cast<BYTE>((static_cast<std::uint16_t>(value) >> 8u) & 0xffu));
}

} // namespace

double reference_time_to_ms(REFERENCE_TIME value) {
    return static_cast<double>(value) / 10'000.0;
}

double frames_to_ms(UINT32 frames, UINT32 sample_rate) {
    return sample_rate == 0
        ? 0.0
        : static_cast<double>(frames) * 1000.0 / static_cast<double>(sample_rate);
}

REFERENCE_TIME frames_to_reference_time(UINT32 frames, UINT32 sample_rate) {
    if (sample_rate == 0) {
        return 0;
    }
    return static_cast<REFERENCE_TIME>(
        (static_cast<std::uint64_t>(frames) * kReferenceTimePerSecond + sample_rate / 2) /
        sample_rate);
}

bool is_wave_subformat(const GUID& guid, WORD tag) {
    constexpr BYTE tail[] = {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    return guid.Data1 == tag && guid.Data2 == 0x0000 && guid.Data3 == 0x0010 &&
        std::equal(std::begin(tail), std::end(tail), std::begin(guid.Data4));
}

SampleKind sample_kind(const WAVEFORMATEX& format) {
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32) {
        return SampleKind::float32;
    }
    if (format.wFormatTag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16) {
        return SampleKind::pcm16;
    }
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        if (is_wave_subformat(extensible.SubFormat, WAVE_FORMAT_IEEE_FLOAT) &&
            format.wBitsPerSample == 32) {
            return SampleKind::float32;
        }
        if (is_wave_subformat(extensible.SubFormat, WAVE_FORMAT_PCM) &&
            format.wBitsPerSample == 16) {
            return SampleKind::pcm16;
        }
    }
    return SampleKind::unknown;
}

double packet_peak_abs_sample(const BYTE* data, UINT32 frames, const WAVEFORMATEX& format) {
    if (data == nullptr || frames == 0 || format.nChannels == 0) {
        return 0.0;
    }
    const std::uint64_t sample_count = static_cast<std::uint64_t>(frames) * format.nChannels;
    double peak = 0.0;
    switch (sample_kind(format)) {
    case SampleKind::float32: {
        const auto* samples = reinterpret_cast<const float*>(data);
        for (std::uint64_t index = 0; index < sample_count; ++index) {
            const double value = std::abs(static_cast<double>(samples[index]));
            if (std::isfinite(value)) {
                peak = std::max(peak, value);
            }
        }
        break;
    }
    case SampleKind::pcm16: {
        const auto* samples = reinterpret_cast<const std::int16_t*>(data);
        for (std::uint64_t index = 0; index < sample_count; ++index) {
            peak = std::max(peak, std::abs(static_cast<double>(samples[index]) / 32768.0));
        }
        break;
    }
    case SampleKind::unknown:
        break;
    }
    return peak;
}

bool build_mono_pcm16_payload(
    const BYTE* data,
    UINT32 frames,
    const WAVEFORMATEX& format,
    bool silent,
    double input_gain,
    std::vector<BYTE>& payload) {
    payload.clear();
    payload.reserve(static_cast<std::size_t>(frames) * sizeof(std::int16_t));
    if (silent || data == nullptr) {
        payload.resize(static_cast<std::size_t>(frames) * sizeof(std::int16_t), 0);
        return true;
    }
    const WORD channels = format.nChannels;
    if (channels == 0) {
        return false;
    }
    switch (sample_kind(format)) {
    case SampleKind::float32: {
        const auto* samples = reinterpret_cast<const float*>(data);
        for (UINT32 frame = 0; frame < frames; ++frame) {
            double mixed = 0.0;
            for (WORD channel = 0; channel < channels; ++channel) {
                mixed += samples[static_cast<std::size_t>(frame) * channels + channel];
            }
            append_int16_le(payload, float_to_pcm16(mixed / channels * input_gain));
        }
        return true;
    }
    case SampleKind::pcm16: {
        const auto* samples = reinterpret_cast<const std::int16_t*>(data);
        for (UINT32 frame = 0; frame < frames; ++frame) {
            int mixed = 0;
            for (WORD channel = 0; channel < channels; ++channel) {
                mixed += samples[static_cast<std::size_t>(frame) * channels + channel];
            }
            const double scaled = static_cast<double>(mixed) / channels * input_gain;
            append_int16_le(payload, static_cast<std::int16_t>(std::clamp(
                static_cast<int>(std::lrint(scaled)), -32768, 32767)));
        }
        return true;
    }
    case SampleKind::unknown:
        return false;
    }
    return false;
}

bool fill_render_buffer_from_mono_pcm16(
    BYTE* destination,
    UINT32 frames,
    const WAVEFORMATEX& render_format,
    JitterBuffer& jitter_buffer,
    double output_gain_start,
    double output_gain_end,
    std::vector<std::int16_t>& mono_frames) {
    if (destination == nullptr || render_format.nChannels == 0) {
        return false;
    }
    jitter_buffer.pop_into(mono_frames, frames);
    const WORD channels = render_format.nChannels;
    switch (sample_kind(render_format)) {
    case SampleKind::float32: {
        auto* samples = reinterpret_cast<float*>(destination);
        for (UINT32 frame = 0; frame < frames; ++frame) {
            const double ramp = frames > 1
                ? static_cast<double>(frame) / static_cast<double>(frames - 1)
                : 1.0;
            const double gain = output_gain_start + (output_gain_end - output_gain_start) * ramp;
            const float value = static_cast<float>(std::clamp(
                static_cast<double>(mono_frames[frame]) / 32768.0 * gain, -1.0, 1.0));
            for (WORD channel = 0; channel < channels; ++channel) {
                samples[static_cast<std::size_t>(frame) * channels + channel] = value;
            }
        }
        return true;
    }
    case SampleKind::pcm16: {
        auto* samples = reinterpret_cast<std::int16_t*>(destination);
        for (UINT32 frame = 0; frame < frames; ++frame) {
            const double ramp = frames > 1
                ? static_cast<double>(frame) / static_cast<double>(frames - 1)
                : 1.0;
            const double gain = output_gain_start + (output_gain_end - output_gain_start) * ramp;
            const auto value = static_cast<std::int16_t>(std::clamp(
                static_cast<int>(std::lrint(static_cast<double>(mono_frames[frame]) * gain)),
                -32768,
                32767));
            for (WORD channel = 0; channel < channels; ++channel) {
                samples[static_cast<std::size_t>(frame) * channels + channel] = value;
            }
        }
        return true;
    }
    case SampleKind::unknown:
        return false;
    }
    return false;
}

bool fill_render_buffer_with_tone(
    BYTE* destination,
    UINT32 frames,
    const WAVEFORMATEX& render_format,
    double frequency_hz,
    double output_gain,
    double& phase,
    double& peak_abs_sample) {
    if (destination == nullptr || render_format.nChannels == 0 || render_format.nSamplesPerSec == 0) {
        return false;
    }
    constexpr double two_pi = 6.283185307179586476925286766559;
    const WORD channels = render_format.nChannels;
    const double amplitude = std::clamp(0.25 * output_gain, 0.0, 1.0);
    const double phase_step = two_pi * frequency_hz / render_format.nSamplesPerSec;
    auto next_sample = [&]() {
        const double value = std::sin(phase) * amplitude;
        peak_abs_sample = std::max(peak_abs_sample, std::fabs(value));
        phase += phase_step;
        if (phase >= two_pi) {
            phase = std::fmod(phase, two_pi);
        }
        return value;
    };
    switch (sample_kind(render_format)) {
    case SampleKind::float32: {
        auto* samples = reinterpret_cast<float*>(destination);
        for (UINT32 frame = 0; frame < frames; ++frame) {
            const float value = static_cast<float>(next_sample());
            for (WORD channel = 0; channel < channels; ++channel) {
                samples[static_cast<std::size_t>(frame) * channels + channel] = value;
            }
        }
        return true;
    }
    case SampleKind::pcm16: {
        auto* samples = reinterpret_cast<std::int16_t*>(destination);
        for (UINT32 frame = 0; frame < frames; ++frame) {
            const std::int16_t value = float_to_pcm16(next_sample());
            for (WORD channel = 0; channel < channels; ++channel) {
                samples[static_cast<std::size_t>(frame) * channels + channel] = value;
            }
        }
        return true;
    }
    case SampleKind::unknown:
        return false;
    }
    return false;
}

bool write_room_mix_to_render_buffer(
    BYTE* destination,
    UINT32 frames,
    const WAVEFORMATEX& render_format,
    std::span<const double> mixed) {
    if (destination == nullptr || mixed.size() < frames || render_format.nChannels == 0) {
        return false;
    }
    const WORD channels = render_format.nChannels;
    switch (sample_kind(render_format)) {
    case SampleKind::float32: {
        auto* samples = reinterpret_cast<float*>(destination);
        for (UINT32 frame = 0; frame < frames; ++frame) {
            const float value = static_cast<float>(std::clamp(mixed[frame], -1.0, 1.0));
            for (WORD channel = 0; channel < channels; ++channel) {
                samples[static_cast<std::size_t>(frame) * channels + channel] = value;
            }
        }
        return true;
    }
    case SampleKind::pcm16: {
        auto* samples = reinterpret_cast<std::int16_t*>(destination);
        for (UINT32 frame = 0; frame < frames; ++frame) {
            const auto value = static_cast<std::int16_t>(std::clamp(
                static_cast<int>(std::lrint(mixed[frame] * 32767.0)), -32768, 32767));
            for (WORD channel = 0; channel < channels; ++channel) {
                samples[static_cast<std::size_t>(frame) * channels + channel] = value;
            }
        }
        return true;
    }
    case SampleKind::unknown:
        return false;
    }
    return false;
}

} // namespace lanspeak::core
