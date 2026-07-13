#pragma once

namespace lanspeak::core {

double smooth_toward(double current, double target, double elapsed_ms, int time_ms);
double decibels_to_gain(double decibels);
double audio_level_dbfs(double amplitude);

} // namespace lanspeak::core
