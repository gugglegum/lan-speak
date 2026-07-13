#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "gui/model.h"

namespace lanspeak::gui {

bool show_contact_dialog(
    HINSTANCE instance,
    HWND owner,
    HICON large_icon,
    HICON small_icon,
    LanguageSetting language,
    const Contact& initial,
    bool edit,
    Contact& result);

} // namespace lanspeak::gui
