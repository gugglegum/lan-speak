#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "gui/gdi_object_cache.h"

namespace lanspeak::gui {

void draw_group_icon(
    HDC dc,
    const RECT& bounds,
    COLORREF color,
    bool crossed,
    GdiObjectCache& objects);

} // namespace lanspeak::gui
