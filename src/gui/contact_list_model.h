#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lanspeak::gui {

struct ContactMeterState {
    double level_db = -90.0;
    std::uint64_t update_ms = 0;
    bool voice_active = false;
    bool stream_active = false;
    std::uint64_t osd_last_active_ms = 0;
};

class ContactMeterBank {
public:
    void sync(std::size_t contact_count);
    void reset();
    void append();
    void erase(std::size_t index);

    bool update(
        std::size_t index,
        double measured_db,
        bool voice_active,
        bool stream_active,
        std::uint64_t now_ms,
        double release_db_per_second,
        double redraw_threshold_db);

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] const ContactMeterState* get(std::size_t index) const;

private:
    std::vector<ContactMeterState> meters_;
};

} // namespace lanspeak::gui
