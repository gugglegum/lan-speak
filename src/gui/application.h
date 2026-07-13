#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

namespace lanspeak::gui {

int run_application(HINSTANCE instance, int show_command);

} // namespace lanspeak::gui
