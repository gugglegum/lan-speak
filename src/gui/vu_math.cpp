#include "gui/vu_math.h"

#include <algorithm>
#include <cmath>

namespace lanspeak::gui {

double vu_fill_ratio(double db) {
    return std::clamp(
        (db - kVuFloorDb) / (kVuCeilingDb - kVuFloorDb),
        0.0,
        1.0);
}

double smooth_vu_level(
    double current_db,
    double measured_db,
    std::uint64_t now_ms,
    std::uint64_t& previous_update_ms,
    double release_db_per_second) {
    if (!std::isfinite(measured_db)) {
        measured_db = -90.0;
    }
    measured_db = std::clamp(measured_db, -90.0, 6.0);
    if (previous_update_ms == 0 || !std::isfinite(current_db) || measured_db >= current_db) {
        previous_update_ms = now_ms;
        return measured_db;
    }

    const double elapsed_seconds =
        static_cast<double>(now_ms - previous_update_ms) / 1000.0;
    previous_update_ms = now_ms;
    return std::max(
        measured_db,
        current_db - std::max(0.0, release_db_per_second) * elapsed_seconds);
}

} // namespace lanspeak::gui
