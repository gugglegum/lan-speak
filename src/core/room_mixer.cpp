#include "core/room_mixer.h"

#include <algorithm>

namespace lanspeak::core {

void RoomMixer::reserve(std::size_t frames) {
    mono_scratch_.reserve(frames);
    mixed_frames_.reserve(frames);
}

void RoomMixer::begin(std::size_t frames) {
    mono_scratch_.resize(frames);
    mixed_frames_.resize(frames);
    std::fill(mixed_frames_.begin(), mixed_frames_.end(), 0.0);
}

std::span<std::int16_t> RoomMixer::mono_scratch() {
    return mono_scratch_;
}

std::span<double> RoomMixer::mixed_frames() {
    return mixed_frames_;
}

std::span<const double> RoomMixer::mixed_frames() const {
    return mixed_frames_;
}

} // namespace lanspeak::core
