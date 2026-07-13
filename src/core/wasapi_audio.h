#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <audioclient.h>
#include <mmreg.h>

#include "core/jitter_buffer.h"

#include <cstdint>
#include <span>
#include <vector>

namespace lanspeak::core {

enum class SampleKind {
    unknown,
    float32,
    pcm16
};

double reference_time_to_ms(REFERENCE_TIME value);
double frames_to_ms(UINT32 frames, UINT32 sample_rate);
REFERENCE_TIME frames_to_reference_time(UINT32 frames, UINT32 sample_rate);
bool is_wave_subformat(const GUID& guid, WORD tag);
SampleKind sample_kind(const WAVEFORMATEX& format);
double packet_peak_abs_sample(const BYTE* data, UINT32 frames, const WAVEFORMATEX& format);

bool build_mono_pcm16_payload(
    const BYTE* data,
    UINT32 frames,
    const WAVEFORMATEX& format,
    bool silent,
    double input_gain,
    std::vector<BYTE>& payload);

bool fill_render_buffer_from_mono_pcm16(
    BYTE* destination,
    UINT32 frames,
    const WAVEFORMATEX& render_format,
    JitterBuffer& jitter_buffer,
    double output_gain_start,
    double output_gain_end,
    std::vector<std::int16_t>& mono_frames);

bool fill_render_buffer_with_tone(
    BYTE* destination,
    UINT32 frames,
    const WAVEFORMATEX& render_format,
    double frequency_hz,
    double output_gain,
    double& phase,
    double& peak_abs_sample);

bool write_room_mix_to_render_buffer(
    BYTE* destination,
    UINT32 frames,
    const WAVEFORMATEX& render_format,
    std::span<const double> mixed);

} // namespace lanspeak::core
