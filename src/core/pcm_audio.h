#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lanspeak::core {

std::int16_t float_to_pcm16(double value);
std::int16_t read_int16_le(const std::byte* data);
void write_int16_le(std::byte* destination, std::int16_t value);

bool decode_mono_pcm16_le(
    std::span<const std::byte> payload,
    std::vector<std::int16_t>& output);

bool resample_mono_pcm16_linear(
    std::span<const std::int16_t> input,
    std::uint32_t input_sample_rate,
    std::uint32_t output_sample_rate,
    std::vector<std::int16_t>& output);

} // namespace lanspeak::core
