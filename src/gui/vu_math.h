#pragma once

#include <cstdint>

namespace lanspeak::gui {

inline constexpr double kVuFloorDb = -60.0;
inline constexpr double kVuCeilingDb = 0.0;

double vu_fill_ratio(double db);
double smooth_vu_level(
    double current_db,
    double measured_db,
    std::uint64_t now_ms,
    std::uint64_t& previous_update_ms,
    double release_db_per_second);

} // namespace lanspeak::gui
