#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "gui/gdi_surface.h"
#include "gui/gdi_object_cache.h"

#include <span>
#include <string>
#include <vector>

namespace lanspeak::gui {

struct OsdRow {
    std::wstring label;
    bool group_target = false;
    double level_db = -90.0;
    bool voice_active = false;
    bool stream_active = false;
};

class OsdOverlay {
public:
    OsdOverlay() = default;
    ~OsdOverlay();
    OsdOverlay(const OsdOverlay&) = delete;
    OsdOverlay& operator=(const OsdOverlay&) = delete;

    bool show(
        HINSTANCE instance,
        std::span<const OsdRow> rows,
        const RECT& monitor_rect,
        HICON speaking_icon);
    void hide();
    void reset();

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    bool ensure_window(HINSTANCE instance);
    HFONT ensure_font(HDC dc);

    HWND window_ = nullptr;
    DibSurface surface_;
    GdiObjectCache gdi_objects_;
    HFONT font_ = nullptr;
    int font_dpi_ = 0;
    std::vector<std::uint32_t> static_pixels_;
    std::wstring static_signature_;
};

} // namespace lanspeak::gui
