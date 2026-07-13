#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <cstdint>
#include <map>

namespace lanspeak::gui {

class GdiObjectCache {
public:
    GdiObjectCache() = default;
    ~GdiObjectCache();
    GdiObjectCache(const GdiObjectCache&) = delete;
    GdiObjectCache& operator=(const GdiObjectCache&) = delete;

    HBRUSH brush(COLORREF color);
    HPEN pen(COLORREF color, int width = 1, int style = PS_SOLID);
    void reset();

private:
    static std::uint64_t pen_key(COLORREF color, int width, int style);

    std::map<COLORREF, HBRUSH> brushes_;
    std::map<std::uint64_t, HPEN> pens_;
};

} // namespace lanspeak::gui
