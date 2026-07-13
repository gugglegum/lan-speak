#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lanspeak::core {

class RoomMixer {
public:
    void reserve(std::size_t frames);
    void begin(std::size_t frames);
    [[nodiscard]] std::span<std::int16_t> mono_scratch();
    [[nodiscard]] std::span<double> mixed_frames();
    [[nodiscard]] std::span<const double> mixed_frames() const;

private:
    std::vector<std::int16_t> mono_scratch_;
    std::vector<double> mixed_frames_;
};

} // namespace lanspeak::core
