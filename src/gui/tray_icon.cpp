#include "gui/tray_icon.h"

#include <shellapi.h>

#include <algorithm>
#include <iterator>

namespace lanspeak::gui {

TrayIcon::~TrayIcon() {
    remove();
}

bool TrayIcon::add(
    HWND window,
    UINT id,
    UINT callback_message,
    HICON icon,
    std::wstring_view tooltip) {
    if (added_) {
        return true;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = id;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = callback_message;
    data.hIcon = icon;
    const std::size_t count = std::min<std::size_t>(tooltip.size(), std::size(data.szTip) - 1);
    std::copy_n(tooltip.data(), count, data.szTip);
    data.szTip[count] = L'\0';
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        return false;
    }
    window_ = window;
    id_ = id;
    added_ = true;
    return true;
}

void TrayIcon::remove() {
    if (!added_) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = id_;
    Shell_NotifyIconW(NIM_DELETE, &data);
    added_ = false;
    window_ = nullptr;
    id_ = 0;
}

void TrayIcon::invalidate_after_taskbar_restart() {
    added_ = false;
}

bool TrayIcon::added() const {
    return added_;
}

} // namespace lanspeak::gui
