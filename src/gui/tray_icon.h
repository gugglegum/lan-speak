#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <string_view>

namespace lanspeak::gui {

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool add(
        HWND window,
        UINT id,
        UINT callback_message,
        HICON icon,
        std::wstring_view tooltip);
    void remove();
    void invalidate_after_taskbar_restart();
    [[nodiscard]] bool added() const;

private:
    HWND window_ = nullptr;
    UINT id_ = 0;
    bool added_ = false;
};

} // namespace lanspeak::gui
