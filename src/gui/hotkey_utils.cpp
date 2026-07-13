#include "gui/hotkey_utils.h"

#include <iterator>

namespace lanspeak::gui {
namespace {

bool is_extended_key(UINT vk) {
    switch (vk) {
    case VK_RMENU:
    case VK_RCONTROL:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_NUMLOCK:
    case VK_DIVIDE:
        return true;
    default:
        return false;
    }
}

} // namespace

bool is_modifier_key(UINT vk) {
    switch (vk) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        return false;
    }
}

std::uint32_t modifier_mask_for_vk(UINT vk) {
    switch (vk) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return kHotkeyShift;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return kHotkeyCtrl;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return kHotkeyAlt;
    case VK_LWIN:
    case VK_RWIN:
        return kHotkeyWin;
    default:
        return 0;
    }
}

std::uint32_t current_hotkey_modifiers() {
    std::uint32_t modifiers = 0;
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) modifiers |= kHotkeyShift;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) modifiers |= kHotkeyCtrl;
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) modifiers |= kHotkeyAlt;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0) {
        modifiers |= kHotkeyWin;
    }
    return modifiers;
}

std::wstring hotkey_key_name(UINT vk) {
    if ((vk >= L'A' && vk <= L'Z') || (vk >= L'0' && vk <= L'9')) {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return L"F" + std::to_wstring(vk - VK_F1 + 1);
    }
    const UINT scan_code = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (scan_code != 0) {
        LONG parameter = static_cast<LONG>(scan_code << 16u);
        if (is_extended_key(vk)) parameter |= 1L << 24u;
        wchar_t name[64]{};
        if (GetKeyNameTextW(parameter, name, static_cast<int>(std::size(name))) > 0) {
            return name;
        }
    }
    return L"VK " + std::to_wstring(vk);
}

std::wstring format_hotkey(const Hotkey& hotkey, std::wstring_view not_set_text) {
    if (!hotkey.valid()) return std::wstring(not_set_text);
    std::wstring result;
    auto append = [&](std::wstring_view part) {
        if (!result.empty()) result += L"+";
        result.append(part);
    };
    if ((hotkey.modifiers & kHotkeyCtrl) != 0) append(L"Ctrl");
    if ((hotkey.modifiers & kHotkeyAlt) != 0) append(L"Alt");
    if ((hotkey.modifiers & kHotkeyShift) != 0) append(L"Shift");
    if ((hotkey.modifiers & kHotkeyWin) != 0) append(L"Win");
    append(hotkey_key_name(hotkey.vk));
    return result;
}

bool same_hotkey(const Hotkey& left, const Hotkey& right) {
    return left.valid() && right.valid() &&
        left.modifiers == right.modifiers && left.vk == right.vk;
}

} // namespace lanspeak::gui
