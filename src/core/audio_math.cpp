#include "core/audio_math.h"

#include <algorithm>
#include <cmath>

namespace lanspeak::core {

double smooth_toward(double current, double target, double elapsed_ms, int time_ms) {
    if (time_ms <= 0 || elapsed_ms <= 0.0) {
        return target;
    }
    const double coefficient = std::exp(-elapsed_ms / static_cast<double>(time_ms));
    return target + (current - target) * coefficient;
}

double decibels_to_gain(double decibels) {
    return std::pow(10.0, decibels / 20.0);
}

double audio_level_dbfs(double amplitude) {
    if (!std::isfinite(amplitude) || amplitude <= 0.00001) {
        return -90.0;
    }
    return std::max(-90.0, 20.0 * std::log10(amplitude));
}

} // namespace lanspeak::core
