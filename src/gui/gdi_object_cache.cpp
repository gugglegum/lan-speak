#include "gui/gdi_object_cache.h"

#include <algorithm>

namespace lanspeak::gui {

GdiObjectCache::~GdiObjectCache() {
    reset();
}

HBRUSH GdiObjectCache::brush(COLORREF color) {
    const auto existing = brushes_.find(color);
    if (existing != brushes_.end()) {
        return existing->second;
    }
    HBRUSH value = CreateSolidBrush(color);
    if (value != nullptr) {
        brushes_.emplace(color, value);
    }
    return value;
}

HPEN GdiObjectCache::pen(COLORREF color, int width, int style) {
    const std::uint64_t key = pen_key(color, width, style);
    const auto existing = pens_.find(key);
    if (existing != pens_.end()) {
        return existing->second;
    }
    HPEN value = CreatePen(style, std::max(1, width), color);
    if (value != nullptr) {
        pens_.emplace(key, value);
    }
    return value;
}

void GdiObjectCache::reset() {
    for (const auto& [unused, brush_value] : brushes_) {
        static_cast<void>(unused);
        DeleteObject(brush_value);
    }
    brushes_.clear();
    for (const auto& [unused, pen_value] : pens_) {
        static_cast<void>(unused);
        DeleteObject(pen_value);
    }
    pens_.clear();
}

std::uint64_t GdiObjectCache::pen_key(COLORREF color, int width, int style) {
    return static_cast<std::uint64_t>(color) |
        (static_cast<std::uint64_t>(static_cast<std::uint16_t>(width)) << 32u) |
        (static_cast<std::uint64_t>(static_cast<std::uint16_t>(style)) << 48u);
}

} // namespace lanspeak::gui
