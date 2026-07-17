#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "gui/model.h"
#include "gui/telemetry_protocol.h"

namespace lanspeak::gui {

void show_audio_latency_dialog(
    HINSTANCE instance,
    HWND owner,
    HICON large_icon,
    HICON small_icon,
    LanguageSetting language,
    const AudioEndpointTelemetry& capture,
    const AudioEndpointTelemetry& render);

} // namespace lanspeak::gui
