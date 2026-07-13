#include "gui/contact_list_model.h"

#include "gui/vu_math.h"

#include <cmath>

namespace lanspeak::gui {

void ContactMeterBank::sync(std::size_t contact_count) {
    meters_.resize(contact_count);
}

void ContactMeterBank::reset() {
    for (ContactMeterState& meter : meters_) {
        meter = {};
    }
}

void ContactMeterBank::append() {
    meters_.emplace_back();
}

void ContactMeterBank::erase(std::size_t index) {
    if (index < meters_.size()) {
        meters_.erase(meters_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

bool ContactMeterBank::update(
    std::size_t index,
    double measured_db,
    bool voice_active,
    bool stream_active,
    std::uint64_t now_ms,
    double release_db_per_second,
    double redraw_threshold_db) {
    if (index >= meters_.size()) return false;
    ContactMeterState& meter = meters_[index];
    const double previous_db = meter.level_db;
    const double displayed_db = smooth_vu_level(
        previous_db,
        measured_db,
        now_ms,
        meter.update_ms,
        release_db_per_second);
    const bool changed = std::abs(previous_db - displayed_db) >= redraw_threshold_db ||
        meter.voice_active != voice_active || meter.stream_active != stream_active;
    meter.level_db = displayed_db;
    meter.voice_active = voice_active;
    meter.stream_active = stream_active;
    if (voice_active) meter.osd_last_active_ms = now_ms;
    return changed;
}

std::size_t ContactMeterBank::size() const {
    return meters_.size();
}

const ContactMeterState* ContactMeterBank::get(std::size_t index) const {
    return index < meters_.size() ? &meters_[index] : nullptr;
}

} // namespace lanspeak::gui
