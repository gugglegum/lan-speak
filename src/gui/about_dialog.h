#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "gui/model.h"

namespace lanspeak::gui {

void show_about_dialog(
    HINSTANCE instance,
    HWND owner,
    HICON large_icon,
    HICON small_icon,
    LanguageSetting language);

} // namespace lanspeak::gui
