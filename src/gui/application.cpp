#include <windows.h>
#include <commctrl.h>

#include "resource.h"

#include "gui/about_dialog.h"
#include "gui/audio_latency_dialog.h"
#include "gui/application.h"
#include "gui/contact_dialog.h"
#include "gui/contact_list_model.h"
#include "gui/core_process_controller.h"
#include "gui/gdi_object_cache.h"
#include "gui/gdi_surface.h"
#include "gui/hotkey_utils.h"
#include "gui/localization.h"
#include "gui/model.h"
#include "gui/network_adapters.h"
#include "gui/network_settings_dialog.h"
#include "gui/osd_overlay.h"
#include "gui/settings_store.h"
#include "gui/tray_icon.h"
#include "gui/ui_icons.h"
#include "gui/vu_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using lanspeak::gui::smooth_vu_level;
using lanspeak::gui::vu_fill_ratio;
using lanspeak::gui::AppSettings;
using lanspeak::gui::Contact;
using lanspeak::gui::Hotkey;
using lanspeak::gui::LanguageSetting;
using lanspeak::gui::NetworkSettingsValue;
using lanspeak::gui::OsdRow;
using lanspeak::gui::TextId;
using lanspeak::gui::draw_group_icon;
using lanspeak::gui::current_hotkey_modifiers;
using lanspeak::gui::is_modifier_key;
using lanspeak::gui::is_supported_mouse_hotkey;
using lanspeak::gui::kHotkeyAlt;
using lanspeak::gui::kHotkeyCtrl;
using lanspeak::gui::kHotkeyShift;
using lanspeak::gui::kHotkeyWin;
using lanspeak::gui::modifier_mask_for_vk;
using lanspeak::gui::same_hotkey;

constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT WM_APP_GLOBAL_PTT = WM_APP + 3;
constexpr UINT WM_APP_CONTACT_PTT = WM_APP + 4;
constexpr UINT WM_APP_CORE_TELEMETRY = WM_APP + 5;
constexpr UINT WM_APP_CORE_EXIT = WM_APP + 6;
constexpr UINT WM_APP_SHOW_EXISTING = WM_APP + 7;
constexpr UINT WM_APP_NETWORK_FALLBACK = WM_APP + 8;
constexpr UINT_PTR IDT_OSD_HOLD = 4003;
constexpr UINT_PTR ID_TRAY_ICON = 1;
constexpr int IDC_CAPTURE_DEVICE = 1005;
constexpr int IDC_RENDER_DEVICE = 1006;
constexpr int IDC_LOG = 1019;
constexpr int IDC_CONTACT_LIST = 1021;
constexpr int IDC_ADD_CONTACT = 1022;
constexpr int IDC_UPDATE_CONTACT = 1023;
constexpr int IDC_TOGGLE_MUTE = 1027;
constexpr int IDC_PUSH_TO_TALK = 1028;
constexpr int IDC_CONTINUOUS_TALK = 1029;
constexpr int IDC_HOTKEY_DISPLAY = 1030;
constexpr int IDC_HOTKEY_RECORD = 1031;
constexpr int IDC_HOTKEY_CLEAR = 1032;
constexpr int IDC_TOGGLE_GLOBAL_PTT = 1033;
constexpr int IDC_LOCAL_METER = 1034;
constexpr int IDC_HOTKEY_CONTACT_RECORD_BASE = 11000;
constexpr int IDC_HOTKEY_CONTACT_CLEAR_BASE = 12000;
constexpr int IDC_CONTEXT_EDIT = 3001;
constexpr int IDC_CONTEXT_DELETE = 3002;
constexpr int IDC_CONTEXT_CONTINUOUS_TALK = 3003;
constexpr int IDC_MENU_CAPTURE_BASE = 4000;
constexpr int IDC_MENU_RENDER_BASE = 5000;
constexpr int IDC_MENU_LANGUAGE_AUTO = 6001;
constexpr int IDC_MENU_LANGUAGE_ENGLISH = 6002;
constexpr int IDC_MENU_LANGUAGE_RUSSIAN = 6003;
constexpr int IDC_MENU_GLOBAL_HOTKEYS = 6004;
constexpr int IDC_MENU_DEBUG_CONSOLE = 6005;
constexpr int IDC_MENU_ABOUT = 6006;
constexpr int IDC_MENU_AUDIO_LATENCY = 6007;
constexpr int IDC_MENU_NETWORK_SETTINGS = 6008;
constexpr int IDC_TRAY_SHOW = 7001;
constexpr int IDC_TRAY_EXIT = 7002;
constexpr int kMaxMenuDevices = 500;

constexpr int kWindowMargin = 12;
constexpr int kInitialWindowWidth = 600;
constexpr int kInitialWindowHeight = 650;
constexpr int kMinimumWindowWidth = 520;
constexpr int kContactListTop = 52;
constexpr int kContactItemHeight = 64;
constexpr int kLogGap = 10;
constexpr int kLogVisibleLines = 4;
constexpr int kMinimumContactListHeight = 80;
constexpr int kLogHardLimitChars = 1024 * 1024;
constexpr int kLogSoftLimitChars = 128 * 1024;
constexpr int kLogTrimTargetChars = 96 * 1024;
constexpr int kLocalMeterHeight = 28;
constexpr int kLocalMeterGap = 8;
constexpr int kContactMuteButtonSize = 30;
constexpr int kContactPttButtonWidth = 36;
constexpr int kContactPttButtonHeight = 30;
constexpr int kContactGlobalPttButtonSize = 30;
constexpr int kContactGainButtonSize = 22;
constexpr int kContactGainSliderWidth = 96;
constexpr int kContactControlGap = 8;
constexpr int kOsdIconSize = 48;
constexpr int kOsdGap = 14;
constexpr int kOsdPaddingX = 20;
constexpr int kOsdPaddingY = 14;
constexpr int kOsdRowGap = 8;
constexpr int kOsdScreenMargin = 36;
constexpr int kOsdOutlineRadius = 2;
constexpr int kOsdTargetIconSize = 30;
constexpr int kOsdTargetGap = 8;
constexpr int kOsdMeterHeight = 7;
constexpr int kOsdMeterGap = 5;
constexpr ULONGLONG kOsdVoiceHoldMs = 1000;
constexpr double kContactGainMin = 0.0;
constexpr double kContactGainMax = 3.0;
constexpr double kContactGainStep = 0.1;
constexpr int kContactVuStreamMinimumPixels = 10;
constexpr double kVuReleaseDbPerSecond = 24.0;
constexpr double kVuRedrawThresholdDb = 0.1;

enum class CoreMode {
    listener,
    duplex
};

struct ApplicationState {
    HINSTANCE instance = nullptr;
    HWND main_window = nullptr;
    std::uint16_t local_port = 49740;
    std::wstring network_adapter_id;
    std::wstring bind_address = L"0.0.0.0";
    HWND capture_device = nullptr;
    HWND render_device = nullptr;
    HWND push_to_talk_button = nullptr;
    HWND continuous_talk_checkbox = nullptr;
    HWND contact_list = nullptr;
    HWND contact_tooltip = nullptr;
    HWND local_meter = nullptr;
    HWND log = nullptr;
    HFONT contact_name_font = nullptr;
    lanspeak::gui::GdiObjectCache gdi_objects;
    lanspeak::gui::CompatibleSurface contact_surface;
    lanspeak::gui::CompatibleSurface local_meter_surface;
    lanspeak::gui::OsdOverlay osd_overlay;
    int selected_contact_index = -1;
    int contact_scroll_offset = 0;
    int contact_wheel_remainder = 0;
    int dragging_gain_index = -1;
    int mouse_contact_ptt_index = -1;
    int contact_tooltip_index = -1;
    bool contact_tooltip_visible = false;
    std::wstring contact_tooltip_text;

    lanspeak::gui::CoreProcessController core_process;
    HANDLE single_instance_mutex = nullptr;
    HANDLE show_existing_event = nullptr;
    HANDLE single_instance_stop_event = nullptr;
    std::thread single_instance_wait_thread;
    bool stop_requested = false;
    bool restart_after_stop = false;
    bool input_muted = true;
    bool push_to_talk_down = false;
    bool button_ptt_down = false;
    bool hotkey_ptt_active = false;
    bool continuous_talk = false;
    bool restore_latched_talk_after_restart = false;
    bool restart_continuous_talk = false;
    std::vector<bool> restart_contact_ptt_latched;
    bool debug_console_visible = false;
    bool exit_requested = false;
    lanspeak::gui::TrayIcon tray_icon;
    UINT taskbar_created_message = 0;
    HHOOK keyboard_hook = nullptr;
    HHOOK mouse_hook = nullptr;
    Hotkey ptt_all_hotkey;
    bool global_hotkey_ptt_down = false;
    Hotkey contact_hotkey_pressed;
    bool contact_hotkey_down = false;
    std::vector<size_t> contact_hotkey_indices;
    WNDPROC push_to_talk_old_proc = nullptr;
    CoreMode core_mode = CoreMode::duplex;
    CoreMode restart_mode = CoreMode::duplex;
    std::vector<std::wstring> capture_device_selectors;
    std::vector<std::wstring> render_device_selectors;
    std::vector<std::wstring> capture_device_labels;
    std::vector<std::wstring> render_device_labels;
    std::wstring saved_capture_device_selector;
    std::wstring saved_render_device_selector;
    int saved_window_width = kInitialWindowWidth;
    int saved_window_height = kInitialWindowHeight;
    LanguageSetting language_setting = LanguageSetting::automatic;

    std::vector<Contact> contacts;
    lanspeak::gui::ContactMeterBank contact_meters;
    std::vector<unsigned int> contact_ptt_refs;
    std::vector<bool> contact_ptt_latched;
    double local_level_db = -90.0;
    bool local_voice_active = false;
    ULONGLONG local_level_update_ms = 0;
    lanspeak::gui::AudioEndpointTelemetry capture_diagnostics;
    lanspeak::gui::AudioEndpointTelemetry render_diagnostics;
};

ApplicationState g_app;


enum class ContactHitAction {
    none,
    select,
    mute,
    global_ptt,
    contact_ptt,
    gain_minus,
    gain_plus,
    gain_slider
};


void update_osd_overlay();
void hide_osd_overlay();
void hide_contact_tooltip();
void fill_rect_color(HDC dc, const RECT& rect, COLORREF color);

const wchar_t* text(TextId id) {
    return lanspeak::gui::localized_text(id, g_app.language_setting);
}

std::wstring format_hotkey(const Hotkey& hotkey) {
    return lanspeak::gui::format_hotkey(hotkey, text(TextId::hotkey_not_set));
}

std::vector<size_t> matching_contact_hotkey_indices(const Hotkey& hotkey) {
    std::vector<size_t> indices;
    if (!hotkey.valid()) {
        return indices;
    }

    for (size_t index = 0; index < g_app.contacts.size(); ++index) {
        if (same_hotkey(g_app.contacts[index].ptt_hotkey, hotkey)) {
            indices.push_back(index);
        }
    }
    return indices;
}

std::vector<size_t> matching_contact_mouse_hotkey_indices(UINT vk) {
    std::vector<size_t> indices;
    for (size_t index = 0; index < g_app.contacts.size(); ++index) {
        const Hotkey& hotkey = g_app.contacts[index].ptt_hotkey;
        if (hotkey.valid() && is_supported_mouse_hotkey(hotkey.vk) && hotkey.vk == vk) {
            indices.push_back(index);
        }
    }
    return indices;
}

bool any_keyboard_hotkey_configured() {
    if (g_app.ptt_all_hotkey.valid() && !is_supported_mouse_hotkey(g_app.ptt_all_hotkey.vk)) {
        return true;
    }
    for (const Contact& contact : g_app.contacts) {
        if (contact.ptt_hotkey.valid() && !is_supported_mouse_hotkey(contact.ptt_hotkey.vk)) {
            return true;
        }
    }
    return false;
}

bool any_mouse_hotkey_configured() {
    if (g_app.ptt_all_hotkey.valid() && is_supported_mouse_hotkey(g_app.ptt_all_hotkey.vk)) {
        return true;
    }
    for (const Contact& contact : g_app.contacts) {
        if (contact.ptt_hotkey.valid() && is_supported_mouse_hotkey(contact.ptt_hotkey.vk)) {
            return true;
        }
    }
    return false;
}

void post_global_hotkey_ptt(bool active) {
    if (g_app.main_window) {
        PostMessageW(g_app.main_window, WM_APP_GLOBAL_PTT, active ? TRUE : FALSE, 0);
    }
}

void post_contact_hotkey_ptt(size_t index, bool active) {
    if (g_app.main_window) {
        PostMessageW(g_app.main_window, WM_APP_CONTACT_PTT, static_cast<WPARAM>(index), active ? TRUE : FALSE);
    }
}

void deactivate_global_hotkey_ptt() {
    if (!g_app.global_hotkey_ptt_down) {
        return;
    }
    g_app.global_hotkey_ptt_down = false;
    post_global_hotkey_ptt(false);
}

void deactivate_contact_hotkey_ptt() {
    if (!g_app.contact_hotkey_down) {
        return;
    }
    g_app.contact_hotkey_down = false;
    for (size_t index : g_app.contact_hotkey_indices) {
        post_contact_hotkey_ptt(index, false);
    }
    g_app.contact_hotkey_indices.clear();
    g_app.contact_hotkey_pressed = {};
}

LRESULT CALLBACK low_level_keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && any_keyboard_hotkey_configured() && g_app.main_window) {
        const auto* keyboard = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
        const UINT vk = keyboard ? keyboard->vkCode : 0;
        const bool key_down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
        const bool key_up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;

        if (key_down && g_app.ptt_all_hotkey.valid() && vk == g_app.ptt_all_hotkey.vk &&
            current_hotkey_modifiers() == g_app.ptt_all_hotkey.modifiers) {
            if (!g_app.global_hotkey_ptt_down) {
                g_app.global_hotkey_ptt_down = true;
                post_global_hotkey_ptt(true);
            }
            return 1;
        }

        if (key_up && g_app.global_hotkey_ptt_down) {
            if (vk == g_app.ptt_all_hotkey.vk) {
                deactivate_global_hotkey_ptt();
                return 1;
            }
            if ((modifier_mask_for_vk(vk) & g_app.ptt_all_hotkey.modifiers) != 0) {
                deactivate_global_hotkey_ptt();
            }
        }

        if (key_down) {
            if (g_app.contact_hotkey_down && vk == g_app.contact_hotkey_pressed.vk) {
                return 1;
            }
            if (!g_app.contact_hotkey_down && !is_modifier_key(vk) && !(g_app.push_to_talk_down && !g_app.input_muted)) {
                Hotkey pressed;
                pressed.modifiers = current_hotkey_modifiers();
                pressed.vk = vk;
                std::vector<size_t> indices = matching_contact_hotkey_indices(pressed);
                if (!indices.empty()) {
                    g_app.contact_hotkey_pressed = pressed;
                    g_app.contact_hotkey_down = true;
                    g_app.contact_hotkey_indices = std::move(indices);
                    for (size_t index : g_app.contact_hotkey_indices) {
                        post_contact_hotkey_ptt(index, true);
                    }
                    return 1;
                }
            }
        }

        if (key_up && g_app.contact_hotkey_down) {
            if (vk == g_app.contact_hotkey_pressed.vk) {
                deactivate_contact_hotkey_ptt();
                return 1;
            }
            if ((modifier_mask_for_vk(vk) & g_app.contact_hotkey_pressed.modifiers) != 0) {
                deactivate_contact_hotkey_ptt();
            }
        }
    }
    return CallNextHookEx(g_app.keyboard_hook, code, wparam, lparam);
}

UINT mouse_hotkey_vk(WPARAM message, const MSLLHOOKSTRUCT* mouse) {
    switch (message) {
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        return VK_RBUTTON;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        return VK_MBUTTON;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        if (mouse) {
            const WORD button = HIWORD(mouse->mouseData);
            if (button == XBUTTON1) return VK_XBUTTON1;
            if (button == XBUTTON2) return VK_XBUTTON2;
        }
        return 0;
    default:
        return 0;
    }
}

bool is_mouse_hotkey_down_message(WPARAM message) {
    return message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
}

bool is_mouse_hotkey_up_message(WPARAM message) {
    return message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP;
}

LRESULT CALLBACK low_level_mouse_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && any_mouse_hotkey_configured() && g_app.main_window) {
        const auto* mouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lparam);
        const UINT vk = mouse_hotkey_vk(wparam, mouse);
        const bool button_down = is_mouse_hotkey_down_message(wparam);
        const bool button_up = is_mouse_hotkey_up_message(wparam);

        if (button_down && g_app.ptt_all_hotkey.valid() &&
            is_supported_mouse_hotkey(g_app.ptt_all_hotkey.vk) && vk == g_app.ptt_all_hotkey.vk) {
            if (!g_app.global_hotkey_ptt_down) {
                g_app.global_hotkey_ptt_down = true;
                post_global_hotkey_ptt(true);
            }
            return 1;
        }

        if (button_up && g_app.global_hotkey_ptt_down && vk == g_app.ptt_all_hotkey.vk) {
            deactivate_global_hotkey_ptt();
            return 1;
        }

        if (button_down && vk != 0) {
            if (g_app.contact_hotkey_down && vk == g_app.contact_hotkey_pressed.vk) {
                return 1;
            }
            if (!g_app.contact_hotkey_down && !(g_app.push_to_talk_down && !g_app.input_muted)) {
                std::vector<size_t> indices = matching_contact_mouse_hotkey_indices(vk);
                if (!indices.empty()) {
                    g_app.contact_hotkey_pressed = Hotkey{0, vk};
                    g_app.contact_hotkey_down = true;
                    g_app.contact_hotkey_indices = std::move(indices);
                    for (size_t index : g_app.contact_hotkey_indices) {
                        post_contact_hotkey_ptt(index, true);
                    }
                    return 1;
                }
            }
        }

        if (button_up && g_app.contact_hotkey_down && vk == g_app.contact_hotkey_pressed.vk &&
            is_supported_mouse_hotkey(g_app.contact_hotkey_pressed.vk)) {
            deactivate_contact_hotkey_ptt();
            return 1;
        }
    }
    return CallNextHookEx(g_app.mouse_hook, code, wparam, lparam);
}

void update_global_hotkey_hook() {
    const bool needs_keyboard_hook = any_keyboard_hotkey_configured();
    const bool needs_mouse_hook = any_mouse_hotkey_configured();
    if (!needs_keyboard_hook && !needs_mouse_hook) {
        deactivate_global_hotkey_ptt();
        deactivate_contact_hotkey_ptt();
    }

    if (needs_keyboard_hook && !g_app.keyboard_hook) {
        g_app.keyboard_hook = SetWindowsHookExW(
            WH_KEYBOARD_LL,
            low_level_keyboard_proc,
            GetModuleHandleW(nullptr),
            0);
    } else if (!needs_keyboard_hook && g_app.keyboard_hook) {
        UnhookWindowsHookEx(g_app.keyboard_hook);
        g_app.keyboard_hook = nullptr;
    }

    if (needs_mouse_hook && !g_app.mouse_hook) {
        g_app.mouse_hook = SetWindowsHookExW(
            WH_MOUSE_LL,
            low_level_mouse_proc,
            GetModuleHandleW(nullptr),
            0);
    } else if (!needs_mouse_hook && g_app.mouse_hook) {
        UnhookWindowsHookEx(g_app.mouse_hook);
        g_app.mouse_hook = nullptr;
    }
}

void remove_global_hotkey_hook() {
    deactivate_global_hotkey_ptt();
    deactivate_contact_hotkey_ptt();
    if (g_app.keyboard_hook) {
        UnhookWindowsHookEx(g_app.keyboard_hook);
        g_app.keyboard_hook = nullptr;
    }
    if (g_app.mouse_hook) {
        UnhookWindowsHookEx(g_app.mouse_hook);
        g_app.mouse_hook = nullptr;
    }
}

std::wstring trim(const std::wstring& text) {
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return L"";
    }
    const auto last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::wstring get_window_text(HWND window) {
    if (!window) {
        return L"";
    }
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(window, text.data(), length + 1);
    }
    text.resize(static_cast<size_t>(length));
    return trim(text);
}

void set_font(HWND window) {
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

HFONT create_ui_font(HWND window, int point_size, int weight) {
    HDC dc = GetDC(window);
    const int dpi_y = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) {
        ReleaseDC(window, dc);
    }
    return CreateFontW(
        -MulDiv(point_size, dpi_y, 72),
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

int log_height_for_visible_lines() {
    HWND target = g_app.log ? g_app.log : g_app.main_window;
    HDC dc = GetDC(target);
    if (!dc) {
        return 78;
    }

    HFONT font = nullptr;
    HGDIOBJ old_font = nullptr;
    if (g_app.log) {
        font = reinterpret_cast<HFONT>(SendMessageW(g_app.log, WM_GETFONT, 0, 0));
        if (font) {
            old_font = SelectObject(dc, font);
        }
    }

    TEXTMETRICW metrics{};
    const bool has_metrics = GetTextMetricsW(dc, &metrics) != 0;

    if (old_font) {
        SelectObject(dc, old_font);
    }
    ReleaseDC(target, dc);

    if (!has_metrics) {
        return 78;
    }

    return metrics.tmHeight * kLogVisibleLines + GetSystemMetrics(SM_CYEDGE) * 2 + 10;
}

void trim_log_for_append(size_t incoming_chars) {
    if (!g_app.log) {
        return;
    }

    const int current_length = GetWindowTextLengthW(g_app.log);
    if (current_length <= 0) {
        return;
    }

    const size_t projected_length = static_cast<size_t>(current_length) + incoming_chars;
    if (projected_length <= kLogSoftLimitChars) {
        return;
    }

    const size_t desired_current_length =
        incoming_chars < kLogTrimTargetChars ? kLogTrimTargetChars - incoming_chars : 0;
    const size_t keep_count = std::min<size_t>(static_cast<size_t>(current_length), desired_current_length);
    const int delete_count = static_cast<int>(static_cast<size_t>(current_length) - keep_count);
    if (delete_count <= 0) {
        return;
    }

    SendMessageW(g_app.log, EM_SETSEL, 0, delete_count);
    SendMessageW(g_app.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
}

HWND add_label(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                  x, y, w, h, parent, nullptr, g_app.instance, nullptr);
    set_font(control);
    return control;
}

HWND add_hidden_combo(HWND parent, int id) {
    HWND control = CreateWindowExW(0, L"COMBOBOX", L"",
                                  WS_CHILD | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                                  0, 0, 1, 1, parent,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_app.instance, nullptr);
    set_font(control);
    return control;
}

HWND add_button(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, DWORD extra_style = 0) {
    HWND control = CreateWindowExW(0, L"BUTTON", text,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra_style,
                                  x, y, w, h, parent,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_app.instance, nullptr);
    set_font(control);
    return control;
}

void append_log(const std::wstring& text) {
    if (!g_app.log) {
        return;
    }
    std::wstring normalized;
    normalized.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) {
            normalized += L"\r\n";
        } else {
            normalized += text[i];
        }
    }
    trim_log_for_append(normalized.size());
    const int length = GetWindowTextLengthW(g_app.log);
    SendMessageW(g_app.log, EM_SETSEL, length, length);
    SendMessageW(g_app.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(normalized.c_str()));
    SendMessageW(g_app.log, EM_SCROLLCARET, 0, 0);
}

bool decode_bytes(UINT code_page, const char* bytes, int byte_count, std::wstring& decoded) {
    const DWORD flags = code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    const int length = MultiByteToWideChar(code_page, flags, bytes, byte_count, nullptr, 0);
    if (length <= 0) {
        return false;
    }
    decoded.assign(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(code_page, flags, bytes, byte_count, decoded.data(), length);
    return true;
}

std::wstring decode_process_output(const char* bytes, int byte_count) {
    if (byte_count <= 0) {
        return L"";
    }

    int zero_count = 0;
    for (int i = 0; i < byte_count; ++i) {
        if (bytes[i] == '\0') {
            ++zero_count;
        }
    }
    if (zero_count > byte_count / 4 && byte_count >= 2) {
        const int wchar_count = byte_count / 2;
        std::wstring decoded;
        decoded.reserve(static_cast<size_t>(wchar_count));
        for (int i = 0; i + 1 < byte_count; i += 2) {
            const auto low = static_cast<unsigned char>(bytes[i]);
            const auto high = static_cast<unsigned char>(bytes[i + 1]);
            decoded.push_back(static_cast<wchar_t>(low | (high << 8)));
        }
        return decoded;
    }

    std::wstring decoded;
    if (decode_bytes(CP_UTF8, bytes, byte_count, decoded)) {
        return decoded;
    }
    if (decode_bytes(CP_ACP, bytes, byte_count, decoded)) {
        return decoded;
    }
    if (decode_bytes(CP_OEMCP, bytes, byte_count, decoded)) {
        return decoded;
    }

    decoded.reserve(static_cast<size_t>(byte_count));
    for (int i = 0; i < byte_count; ++i) {
        decoded.push_back(static_cast<unsigned char>(bytes[i]));
    }
    return decoded;
}

std::wstring quote_arg(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }

    const bool needs_quotes = argument.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needs_quotes) {
        return argument;
    }

    std::wstring result = L"\"";
    unsigned backslashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(ch);
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(ch);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring join_command_line(const std::vector<std::wstring>& args) {
    std::wstring command_line;
    for (const std::wstring& arg : args) {
        if (!command_line.empty()) {
            command_line.push_back(L' ');
        }
        command_line += quote_arg(arg);
    }
    return command_line;
}

std::wstring module_path() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (length == path.size()) {
        path.assign(path.size() * 2, L'\0');
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    path.resize(length);
    return path;
}

std::wstring module_directory() {
    const std::wstring path = module_path();
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, slash);
}

std::uint64_t fnv1a_hash_utf16(std::wstring value) {
    CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));

    std::uint64_t hash = 14695981039346656037ull;
    for (const wchar_t ch : value) {
        hash ^= static_cast<std::uint64_t>(ch & 0xffu);
        hash *= 1099511628211ull;
        hash ^= static_cast<std::uint64_t>((ch >> 8u) & 0xffu);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::wstring hex_u64(std::uint64_t value) {
    std::wostringstream stream;
    stream << std::hex << std::uppercase << value;
    return stream.str();
}

std::wstring single_instance_suffix() {
    return hex_u64(fnv1a_hash_utf16(module_path()));
}

std::wstring settings_path() {
    return module_directory() + L"\\settings.txt";
}

std::wstring legacy_settings_path() {
    return module_directory() + L"\\LanSpeakGui.settings.txt";
}

bool file_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring find_core_exe() {
    return module_directory() + L"\\LanSpeakCore.exe";
}

bool initialize_single_instance() {
    const std::wstring suffix = single_instance_suffix();
    const std::wstring event_name = L"Local\\LanSpeakGui.Show." + suffix;
    const std::wstring mutex_name = L"Local\\LanSpeakGui.Mutex." + suffix;

    g_app.show_existing_event = CreateEventW(nullptr, FALSE, FALSE, event_name.c_str());
    g_app.single_instance_mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());

    if (g_app.single_instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_app.show_existing_event) {
            SetEvent(g_app.show_existing_event);
            CloseHandle(g_app.show_existing_event);
            g_app.show_existing_event = nullptr;
        }
        CloseHandle(g_app.single_instance_mutex);
        g_app.single_instance_mutex = nullptr;
        return false;
    }

    return true;
}

void start_single_instance_waiter() {
    if (!g_app.show_existing_event || g_app.single_instance_wait_thread.joinable()) {
        return;
    }
    g_app.single_instance_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_app.single_instance_stop_event) {
        return;
    }
    g_app.single_instance_wait_thread = std::thread([]() {
        const HANDLE events[] = {g_app.show_existing_event, g_app.single_instance_stop_event};
        for (;;) {
            const DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0) {
                if (g_app.main_window) {
                    PostMessageW(g_app.main_window, WM_APP_SHOW_EXISTING, 0, 0);
                }
                continue;
            }
            return;
        }
    });
}

void release_single_instance() {
    if (g_app.single_instance_stop_event) {
        SetEvent(g_app.single_instance_stop_event);
    }
    if (g_app.single_instance_wait_thread.joinable()) {
        g_app.single_instance_wait_thread.join();
    }
    if (g_app.single_instance_stop_event) {
        CloseHandle(g_app.single_instance_stop_event);
        g_app.single_instance_stop_event = nullptr;
    }
    if (g_app.show_existing_event) {
        CloseHandle(g_app.show_existing_event);
        g_app.show_existing_event = nullptr;
    }
    if (g_app.single_instance_mutex) {
        ReleaseMutex(g_app.single_instance_mutex);
        CloseHandle(g_app.single_instance_mutex);
        g_app.single_instance_mutex = nullptr;
    }
}

std::wstring selected_device_selector(HWND combo, const std::vector<std::wstring>& selectors) {
    const auto selection = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (selection >= 0 && static_cast<size_t>(selection) < selectors.size()) {
        return selectors[static_cast<size_t>(selection)];
    }
    return L"";
}

std::wstring format_contact_gain(double value);

std::wstring contact_runtime_gain_text(const Contact& contact) {
    if (contact.muted) {
        return L"0";
    }
    return format_contact_gain(contact.gain);
}

std::wstring format_contact_gain(double value) {
    value = std::clamp(value, kContactGainMin, kContactGainMax);
    value = std::round(value / kContactGainStep) * kContactGainStep;
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(1) << value;
    return stream.str();
}

std::wstring format_decimal(double value, int precision) {
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(precision) << value;
    std::wstring result = stream.str();
    while (result.size() > 1 && result.back() == L'0') {
        result.pop_back();
    }
    if (!result.empty() && result.back() == L'.') {
        result.pop_back();
    }
    return result;
}

double contact_gain_slider_ratio(const Contact& contact) {
    const double value = std::clamp(contact.gain, kContactGainMin, kContactGainMax);
    return (value - kContactGainMin) / (kContactGainMax - kContactGainMin);
}

void sync_contact_meter_state() {
    g_app.contact_meters.sync(g_app.contacts.size());
    g_app.contact_ptt_refs.resize(g_app.contacts.size(), 0);
    g_app.contact_ptt_latched.resize(g_app.contacts.size(), false);
}

void reset_contact_meter_state() {
    g_app.contact_meters.reset();
    g_app.local_level_db = -90.0;
    g_app.local_voice_active = false;
    g_app.local_level_update_ms = 0;
    if (g_app.contact_list) {
        InvalidateRect(g_app.contact_list, nullptr, FALSE);
    }
    if (g_app.local_meter) {
        InvalidateRect(g_app.local_meter, nullptr, FALSE);
    }
    hide_osd_overlay();
}

void repaint_contact_talk_state_now() {
    if (g_app.contact_list) {
        RedrawWindow(g_app.contact_list, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
}

bool contact_push_to_talk_active(size_t index) {
    return index < g_app.contact_ptt_refs.size() &&
        index < g_app.contact_ptt_latched.size() &&
        (g_app.contact_ptt_refs[index] > 0 || g_app.contact_ptt_latched[index]);
}

bool contact_push_to_talk_latched(size_t index) {
    return index < g_app.contact_ptt_latched.size() && g_app.contact_ptt_latched[index];
}

bool any_contact_push_to_talk_active() {
    for (size_t index = 0; index < g_app.contacts.size(); ++index) {
        if (contact_push_to_talk_active(index)) {
            return true;
        }
    }
    return false;
}

int contact_content_height() {
    return static_cast<int>(g_app.contacts.size()) * kContactItemHeight;
}

int contact_panel_client_height(HWND panel) {
    if (!panel) {
        return 0;
    }
    RECT client{};
    GetClientRect(panel, &client);
    return std::max(0, static_cast<int>(client.bottom - client.top));
}

int max_contact_scroll_offset(HWND panel) {
    return std::max(0, contact_content_height() - contact_panel_client_height(panel));
}

void update_contact_scrollbar(HWND panel) {
    if (!panel) {
        return;
    }

    const int page_height = contact_panel_client_height(panel);
    const int total_height = contact_content_height();
    g_app.contact_scroll_offset = std::clamp(g_app.contact_scroll_offset, 0, max_contact_scroll_offset(panel));

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = std::max(0, total_height - 1);
    info.nPage = static_cast<UINT>(std::max(1, page_height));
    info.nPos = g_app.contact_scroll_offset;
    SetScrollInfo(panel, SB_VERT, &info, TRUE);
    ShowScrollBar(panel, SB_VERT, total_height > page_height);
}

void set_contact_scroll_offset(HWND panel, int offset) {
    if (!panel) {
        return;
    }

    const int clamped = std::clamp(offset, 0, max_contact_scroll_offset(panel));
    if (clamped == g_app.contact_scroll_offset) {
        update_contact_scrollbar(panel);
        return;
    }

    g_app.contact_scroll_offset = clamped;
    update_contact_scrollbar(panel);
    InvalidateRect(panel, nullptr, TRUE);
}

void ensure_contact_visible(int index) {
    if (!g_app.contact_list || index < 0 || static_cast<size_t>(index) >= g_app.contacts.size()) {
        return;
    }

    const int page_height = contact_panel_client_height(g_app.contact_list);
    const int item_top = index * kContactItemHeight;
    const int item_bottom = item_top + kContactItemHeight;
    if (item_top < g_app.contact_scroll_offset) {
        set_contact_scroll_offset(g_app.contact_list, item_top);
    } else if (item_bottom > g_app.contact_scroll_offset + page_height) {
        set_contact_scroll_offset(g_app.contact_list, item_bottom - page_height);
    }
}

void set_selected_contact_index(int index, bool ensure_visible) {
    sync_contact_meter_state();
    if (g_app.contacts.empty() || index < 0) {
        g_app.selected_contact_index = -1;
    } else {
        g_app.selected_contact_index = std::clamp(index, 0, static_cast<int>(g_app.contacts.size()) - 1);
        if (ensure_visible) {
            ensure_contact_visible(g_app.selected_contact_index);
        }
    }

    if (g_app.contact_list) {
        InvalidateRect(g_app.contact_list, nullptr, TRUE);
    }
}

void invalidate_contact_row(size_t index) {
    if (!g_app.contact_list || index >= g_app.contacts.size()) {
        return;
    }

    RECT client{};
    GetClientRect(g_app.contact_list, &client);
    RECT row_rect{
        client.left,
        static_cast<int>(index) * kContactItemHeight - g_app.contact_scroll_offset,
        client.right,
        static_cast<int>(index + 1) * kContactItemHeight - g_app.contact_scroll_offset};
    if (row_rect.bottom <= client.top || row_rect.top >= client.bottom) {
        return;
    }
    row_rect.top = std::max(row_rect.top, client.top);
    row_rect.bottom = std::min(row_rect.bottom, client.bottom);
    InvalidateRect(g_app.contact_list, &row_rect, FALSE);
}

void update_contact_meter(
    size_t index,
    double db,
    bool active,
    bool stream_active,
    bool defer_osd_update = false) {
    sync_contact_meter_state();
    const bool changed = g_app.contact_meters.update(
        index,
        db,
        active,
        stream_active,
        GetTickCount64(),
        kVuReleaseDbPerSecond,
        kVuRedrawThresholdDb);
    if (changed) {
        invalidate_contact_row(index);
        if (!defer_osd_update) {
            update_osd_overlay();
        }
    }
}

void update_local_meter(double db, bool active, bool defer_osd_update = false) {
    const double previous_db = g_app.local_level_db;
    const double displayed_db = smooth_vu_level(
        previous_db,
        db,
        GetTickCount64(),
        g_app.local_level_update_ms,
        kVuReleaseDbPerSecond);
    const bool changed = std::abs(previous_db - displayed_db) >= kVuRedrawThresholdDb ||
        g_app.local_voice_active != active;
    g_app.local_level_db = displayed_db;
    g_app.local_voice_active = active;
    if (!changed) {
        return;
    }

    if (g_app.local_meter) {
        InvalidateRect(g_app.local_meter, nullptr, FALSE);
    }
    if (!defer_osd_update &&
        ((g_app.push_to_talk_down && !g_app.input_muted) || any_contact_push_to_talk_active())) {
        update_osd_overlay();
    }
}

RECT contact_card_rect_from_row(const RECT& row_rect) {
    RECT card_rect = row_rect;
    card_rect.left += 6;
    card_rect.top += 5;
    card_rect.right -= 6;
    card_rect.bottom -= 5;
    return card_rect;
}

RECT contact_mute_button_rect_from_card(const RECT& card_rect) {
    const int top = card_rect.top + ((card_rect.bottom - card_rect.top) - kContactMuteButtonSize) / 2;
    return RECT{
        card_rect.right - 14 - kContactMuteButtonSize,
        top,
        card_rect.right - 14,
        top + kContactMuteButtonSize};
}

RECT contact_ptt_button_rect_from_card(const RECT& card_rect) {
    const RECT mute = contact_mute_button_rect_from_card(card_rect);
    const int center_y = card_rect.top + (card_rect.bottom - card_rect.top) / 2;
    return RECT{
        mute.left - kContactControlGap - kContactPttButtonWidth,
        center_y - kContactPttButtonHeight / 2,
        mute.left - kContactControlGap,
        center_y + (kContactPttButtonHeight + 1) / 2};
}

RECT contact_global_ptt_button_rect_from_card(const RECT& card_rect) {
    const RECT ptt = contact_ptt_button_rect_from_card(card_rect);
    const int center_y = card_rect.top + (card_rect.bottom - card_rect.top) / 2;
    return RECT{
        ptt.left - kContactControlGap - kContactGlobalPttButtonSize,
        center_y - kContactGlobalPttButtonSize / 2,
        ptt.left - kContactControlGap,
        center_y + (kContactGlobalPttButtonSize + 1) / 2};
}

RECT centered_rect(int right, int center_y, int width, int height) {
    return RECT{
        right - width,
        center_y - height / 2,
        right,
        center_y + (height + 1) / 2};
}

RECT contact_gain_plus_rect_from_card(const RECT& card_rect) {
    const RECT global_ptt = contact_global_ptt_button_rect_from_card(card_rect);
    const int center_y = card_rect.top + (card_rect.bottom - card_rect.top) / 2;
    return centered_rect(
        global_ptt.left - kContactControlGap,
        center_y,
        kContactGainButtonSize,
        kContactGainButtonSize);
}

RECT contact_gain_slider_rect_from_card(const RECT& card_rect) {
    const RECT plus = contact_gain_plus_rect_from_card(card_rect);
    const int center_y = card_rect.top + (card_rect.bottom - card_rect.top) / 2;
    return centered_rect(plus.left - kContactControlGap, center_y, kContactGainSliderWidth, 18);
}

RECT contact_gain_minus_rect_from_card(const RECT& card_rect) {
    const RECT slider = contact_gain_slider_rect_from_card(card_rect);
    const int center_y = card_rect.top + (card_rect.bottom - card_rect.top) / 2;
    return centered_rect(slider.left - kContactControlGap, center_y, kContactGainButtonSize, kContactGainButtonSize);
}

bool contact_gain_controls_visible(const RECT& card_rect) {
    return contact_gain_minus_rect_from_card(card_rect).left >= card_rect.left + 150;
}

RECT contact_row_rect_from_index(HWND panel, int index) {
    RECT client{};
    GetClientRect(panel, &client);
    return RECT{
        client.left,
        index * kContactItemHeight - g_app.contact_scroll_offset,
        client.right,
        (index + 1) * kContactItemHeight - g_app.contact_scroll_offset};
}

int contact_index_from_point(HWND panel, POINT point) {
    if (!panel) {
        return -1;
    }

    RECT client{};
    GetClientRect(panel, &client);
    if (point.x < client.left || point.x >= client.right || point.y < client.top || point.y >= client.bottom) {
        return -1;
    }

    const int index = (point.y + g_app.contact_scroll_offset) / kContactItemHeight;
    if (index < 0 || static_cast<size_t>(index) >= g_app.contacts.size()) {
        return -1;
    }
    return index;
}

ContactHitAction contact_hit_test(HWND panel, POINT point, int& index) {
    index = contact_index_from_point(panel, point);
    if (index < 0) {
        return ContactHitAction::none;
    }

    const RECT row_rect = contact_row_rect_from_index(panel, index);
    const RECT card_rect = contact_card_rect_from_row(row_rect);
    const RECT ptt_rect = contact_ptt_button_rect_from_card(card_rect);
    if (PtInRect(&ptt_rect, point)) {
        return ContactHitAction::contact_ptt;
    }
    const RECT global_ptt_rect = contact_global_ptt_button_rect_from_card(card_rect);
    if (PtInRect(&global_ptt_rect, point)) {
        return ContactHitAction::global_ptt;
    }
    const RECT mute_rect = contact_mute_button_rect_from_card(card_rect);
    if (PtInRect(&mute_rect, point)) {
        return ContactHitAction::mute;
    }
    if (contact_gain_controls_visible(card_rect)) {
        const RECT minus_rect = contact_gain_minus_rect_from_card(card_rect);
        const RECT slider_rect = contact_gain_slider_rect_from_card(card_rect);
        const RECT plus_rect = contact_gain_plus_rect_from_card(card_rect);
        if (PtInRect(&minus_rect, point)) {
            return ContactHitAction::gain_minus;
        }
        if (PtInRect(&plus_rect, point)) {
            return ContactHitAction::gain_plus;
        }
        if (PtInRect(&slider_rect, point)) {
            return ContactHitAction::gain_slider;
        }
    }
    return ContactHitAction::select;
}

TOOLINFOW contact_tooltip_info(HWND panel) {
    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    info.hwnd = panel;
    info.uId = 1;
    return info;
}

void hide_contact_tooltip() {
    if (g_app.contact_tooltip && g_app.contact_tooltip_visible && g_app.contact_list) {
        TOOLINFOW info = contact_tooltip_info(g_app.contact_list);
        SendMessageW(g_app.contact_tooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&info));
    }
    g_app.contact_tooltip_visible = false;
}

void update_contact_tooltip_target(HWND panel, POINT point) {
    int index = -1;
    const ContactHitAction action = contact_hit_test(panel, point, index);
    const int target_index = action == ContactHitAction::global_ptt ? index : -1;
    if (target_index == g_app.contact_tooltip_index) {
        return;
    }

    hide_contact_tooltip();
    g_app.contact_tooltip_index = target_index;

    TRACKMOUSEEVENT tracking{};
    tracking.cbSize = sizeof(tracking);
    tracking.dwFlags = TME_LEAVE | (target_index >= 0 ? TME_HOVER : 0);
    tracking.hwndTrack = panel;
    tracking.dwHoverTime = 500;
    TrackMouseEvent(&tracking);
}

void show_contact_tooltip(HWND panel) {
    if (!g_app.contact_tooltip || g_app.contact_tooltip_index < 0 ||
        static_cast<size_t>(g_app.contact_tooltip_index) >= g_app.contacts.size()) {
        return;
    }

    POINT point{};
    GetCursorPos(&point);
    POINT client_point = point;
    ScreenToClient(panel, &client_point);
    int index = -1;
    if (contact_hit_test(panel, client_point, index) != ContactHitAction::global_ptt ||
        index != g_app.contact_tooltip_index) {
        return;
    }

    const bool enabled = g_app.contacts[static_cast<size_t>(index)].global_ptt_enabled;
    g_app.contact_tooltip_text = text(enabled ? TextId::exclude_from_global_ptt : TextId::include_in_global_ptt);
    TOOLINFOW info = contact_tooltip_info(panel);
    info.lpszText = g_app.contact_tooltip_text.data();
    SendMessageW(g_app.contact_tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
    SendMessageW(g_app.contact_tooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(point.x + 12, point.y + 20));
    SendMessageW(g_app.contact_tooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&info));
    g_app.contact_tooltip_visible = true;
}

void create_contact_tooltip(HWND panel) {
    if (!panel || g_app.contact_tooltip) {
        return;
    }

    g_app.contact_tooltip = CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASSW,
        nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        panel,
        nullptr,
        g_app.instance,
        nullptr);
    if (!g_app.contact_tooltip) {
        return;
    }

    SetWindowPos(
        g_app.contact_tooltip,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    TOOLINFOW info = contact_tooltip_info(panel);
    info.lpszText = const_cast<LPWSTR>(L"");
    SendMessageW(g_app.contact_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    SendMessageW(g_app.contact_tooltip, TTM_SETMAXTIPWIDTH, 0, 320);
}

void refresh_contact_list(int select_index = -1) {
    if (!g_app.contact_list) {
        return;
    }
    const int desired_index = select_index >= 0 ? select_index : g_app.selected_contact_index;
    set_selected_contact_index(desired_index, true);
    update_contact_scrollbar(g_app.contact_list);
    InvalidateRect(g_app.contact_list, nullptr, TRUE);
}

int selected_contact_index() {
    if (!g_app.contact_list || g_app.selected_contact_index < 0 ||
        static_cast<size_t>(g_app.selected_contact_index) >= g_app.contacts.size()) {
        return -1;
    }
    return g_app.selected_contact_index;
}

int selected_combo_index(HWND combo) {
    if (!combo) {
        return 0;
    }
    const auto selection = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    return selection < 0 ? 0 : selection;
}

void append_checked_menu_item(HMENU menu, UINT id, const std::wstring& label, bool checked) {
    AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED), id, label.c_str());
}

void append_device_menu_items(
    HMENU menu,
    UINT base_id,
    const std::vector<std::wstring>& labels,
    int selected_index) {
    const size_t count = std::min<size_t>(labels.size(), kMaxMenuDevices);
    for (size_t index = 0; index < count; ++index) {
        append_checked_menu_item(
            menu,
            base_id + static_cast<UINT>(index),
            labels[index],
            static_cast<int>(index) == selected_index);
    }
}

void update_default_device_labels() {
    if (!g_app.capture_device_labels.empty()) {
        g_app.capture_device_labels[0] = text(TextId::default_capture_device);
    }
    if (!g_app.render_device_labels.empty()) {
        g_app.render_device_labels[0] = text(TextId::default_render_device);
    }
}

HICON load_app_icon(int width, int height) {
    HICON icon = static_cast<HICON>(LoadImageW(
        g_app.instance,
        MAKEINTRESOURCEW(IDI_LANSPEAK),
        IMAGE_ICON,
        width,
        height,
        LR_DEFAULTCOLOR | LR_SHARED));
    if (icon) {
        return icon;
    }
    return LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
}

HICON load_app_large_icon() {
    return load_app_icon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
}

HICON load_app_small_icon() {
    return load_app_icon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
}

void apply_window_icons(HWND window) {
    SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(load_app_large_icon()));
    SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(load_app_small_icon()));
}

HICON load_osd_icon() {
    HICON icon = static_cast<HICON>(LoadImageW(
        g_app.instance,
        MAKEINTRESOURCEW(IDI_LANSPEAK_FULL),
        IMAGE_ICON,
        kOsdIconSize,
        kOsdIconSize,
        LR_DEFAULTCOLOR | LR_SHARED));
    return icon ? icon : load_app_large_icon();
}

bool app_has_foreground_window() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) {
        return false;
    }

    DWORD process_id = 0;
    GetWindowThreadProcessId(foreground, &process_id);
    return process_id == GetCurrentProcessId();
}


std::wstring osd_contact_name(size_t index) {
    if (index >= g_app.contacts.size() || g_app.contacts[index].name.empty()) {
        return text(TextId::unnamed_contact);
    }
    return g_app.contacts[index].name;
}

std::vector<OsdRow> active_osd_rows() {
    std::vector<OsdRow> rows;
    sync_contact_meter_state();

    const std::wstring outgoing_prefix = std::wstring(text(TextId::osd_me)) + L" \u2192";
    const bool global_talk_active = g_app.push_to_talk_down && !g_app.input_muted;
    if (global_talk_active) {
        rows.push_back(OsdRow{
            outgoing_prefix,
            true,
            g_app.local_level_db,
            g_app.local_voice_active,
            true});
    }
    for (size_t index = 0; index < g_app.contacts.size(); ++index) {
        if (contact_push_to_talk_active(index) &&
            (!global_talk_active || !g_app.contacts[index].global_ptt_enabled)) {
            rows.push_back(OsdRow{
                outgoing_prefix + L" " + osd_contact_name(index),
                false,
                g_app.local_level_db,
                g_app.local_voice_active,
                true});
        }
    }

    const ULONGLONG now = GetTickCount64();
    for (size_t index = 0; index < g_app.contacts.size(); ++index) {
        const lanspeak::gui::ContactMeterState* meter = g_app.contact_meters.get(index);
        const bool active = meter != nullptr && meter->voice_active;
        const bool held = meter != nullptr && meter->osd_last_active_ms != 0 &&
            now - meter->osd_last_active_ms <= kOsdVoiceHoldMs;
        if (active || held) {
            rows.push_back(OsdRow{
                osd_contact_name(index),
                false,
                meter != nullptr ? meter->level_db : -90.0,
                active,
                meter != nullptr && meter->stream_active});
        }
    }
    return rows;
}

void hide_osd_overlay() {
    if (g_app.main_window) {
        KillTimer(g_app.main_window, IDT_OSD_HOLD);
    }
    g_app.osd_overlay.hide();
}

RECT foreground_monitor_rect() {
    HWND foreground = GetForegroundWindow();
    HMONITOR monitor = MonitorFromWindow(foreground ? foreground : g_app.main_window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        return info.rcMonitor;
    }

    return RECT{
        0,
        0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN)};
}

void schedule_osd_hold_timer() {
    if (!g_app.main_window) {
        return;
    }
    KillTimer(g_app.main_window, IDT_OSD_HOLD);
    const ULONGLONG now = GetTickCount64();
    ULONGLONG nearest_remaining = 0;
    for (std::size_t index = 0; index < g_app.contact_meters.size(); ++index) {
        const lanspeak::gui::ContactMeterState* meter = g_app.contact_meters.get(index);
        if (meter == nullptr) {
            continue;
        }
        const bool active = meter->voice_active;
        const ULONGLONG last_active = meter->osd_last_active_ms;
        if (active || last_active == 0 || now < last_active) {
            continue;
        }
        const ULONGLONG elapsed = now - last_active;
        if (elapsed >= kOsdVoiceHoldMs) {
            continue;
        }
        const ULONGLONG remaining = kOsdVoiceHoldMs - elapsed;
        nearest_remaining = nearest_remaining == 0
            ? remaining
            : std::min(nearest_remaining, remaining);
    }
    if (nearest_remaining > 0) {
        SetTimer(
            g_app.main_window,
            IDT_OSD_HOLD,
            static_cast<UINT>(std::max<ULONGLONG>(1, nearest_remaining)),
            nullptr);
    }
}

void update_osd_overlay() {
    if (app_has_foreground_window()) {
        hide_osd_overlay();
        return;
    }

    const std::vector<OsdRow> rows = active_osd_rows();
    if (rows.empty()) {
        hide_osd_overlay();
        return;
    }

    if (g_app.osd_overlay.show(
            g_app.instance,
            rows,
            foreground_monitor_rect(),
            load_osd_icon())) {
        schedule_osd_hold_timer();
    }
}

bool add_tray_icon(HWND window) {
    return g_app.tray_icon.add(
        window,
        static_cast<UINT>(ID_TRAY_ICON),
        WM_APP_TRAY,
        load_app_small_icon(),
        L"LanSpeak");
}

void remove_tray_icon(HWND) {
    g_app.tray_icon.remove();
}

void show_main_window_from_tray(HWND window) {
    ShowWindow(window, SW_SHOW);
    ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOWNORMAL);
    SetForegroundWindow(window);
}

void hide_main_window_to_tray(HWND window) {
    if (add_tray_icon(window)) {
        ShowWindow(window, SW_HIDE);
    } else {
        ShowWindow(window, SW_MINIMIZE);
    }
}

void toggle_main_window_from_tray(HWND window) {
    if (IsWindowVisible(window) && !IsIconic(window)) {
        hide_main_window_to_tray(window);
    } else {
        show_main_window_from_tray(window);
    }
}

void request_application_exit(HWND window) {
    g_app.exit_requested = true;
    remove_tray_icon(window);
    DestroyWindow(window);
}

void show_tray_context_menu(HWND window) {
    POINT cursor{};
    GetCursorPos(&cursor);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDC_TRAY_SHOW, text(TextId::tray_show));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDC_TRAY_EXIT, text(TextId::tray_exit));

    SetForegroundWindow(window);
    const UINT command = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        cursor.x,
        cursor.y,
        0,
        window,
        nullptr);
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);

    if (command == IDC_TRAY_SHOW) {
        show_main_window_from_tray(window);
    } else if (command == IDC_TRAY_EXIT) {
        request_application_exit(window);
    }
}

void rebuild_menu_bar() {
    if (!g_app.main_window) {
        return;
    }

    HMENU main_menu = CreateMenu();
    HMENU file_menu = CreatePopupMenu();
    HMENU input_menu = CreatePopupMenu();
    HMENU output_menu = CreatePopupMenu();
    HMENU settings_menu = CreatePopupMenu();
    HMENU language_menu = CreatePopupMenu();
    HMENU help_menu = CreatePopupMenu();

    append_device_menu_items(
        input_menu,
        IDC_MENU_CAPTURE_BASE,
        g_app.capture_device_labels,
        selected_combo_index(g_app.capture_device));
    append_device_menu_items(
        output_menu,
        IDC_MENU_RENDER_BASE,
        g_app.render_device_labels,
        selected_combo_index(g_app.render_device));

    append_checked_menu_item(
        language_menu,
        IDC_MENU_LANGUAGE_AUTO,
        text(TextId::language_auto),
        g_app.language_setting == LanguageSetting::automatic);
    append_checked_menu_item(
        language_menu,
        IDC_MENU_LANGUAGE_ENGLISH,
        text(TextId::language_english),
        g_app.language_setting == LanguageSetting::english);
    append_checked_menu_item(
        language_menu,
        IDC_MENU_LANGUAGE_RUSSIAN,
        text(TextId::language_russian),
        g_app.language_setting == LanguageSetting::russian);
    AppendMenuW(settings_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(input_menu), text(TextId::capture_device));
    AppendMenuW(settings_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(output_menu), text(TextId::render_device));
    AppendMenuW(settings_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(settings_menu, MF_STRING, IDC_MENU_NETWORK_SETTINGS, text(TextId::network_settings));
    AppendMenuW(
        settings_menu,
        MF_STRING,
        IDC_MENU_AUDIO_LATENCY,
        text(TextId::audio_latency_diagnostics));
    AppendMenuW(settings_menu, MF_STRING, IDC_MENU_GLOBAL_HOTKEYS, text(TextId::global_hotkeys));
    append_checked_menu_item(
        settings_menu,
        IDC_MENU_DEBUG_CONSOLE,
        text(TextId::debug_console),
        g_app.debug_console_visible);
    AppendMenuW(settings_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(settings_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(language_menu), text(TextId::language));

    AppendMenuW(file_menu, MF_STRING, IDC_TRAY_EXIT, text(TextId::tray_exit));
    AppendMenuW(help_menu, MF_STRING, IDC_MENU_ABOUT, text(TextId::about));

    AppendMenuW(main_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), text(TextId::file));
    AppendMenuW(main_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(settings_menu), text(TextId::settings));
    AppendMenuW(main_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help_menu), text(TextId::help));

    HMENU old_menu = GetMenu(g_app.main_window);
    SetMenu(g_app.main_window, main_menu);
    if (old_menu) {
        DestroyMenu(old_menu);
    }
    DrawMenuBar(g_app.main_window);
}

void apply_language_to_main_window(bool refresh_contacts = true) {
    hide_contact_tooltip();
    if (g_app.main_window) {
        SetWindowTextW(g_app.main_window, L"LanSpeak");
    }
    if (g_app.push_to_talk_button) {
        SetWindowTextW(g_app.push_to_talk_button, text(g_app.push_to_talk_down ? TextId::talking : TextId::talk));
    }
    if (g_app.continuous_talk_checkbox) {
        SetWindowTextW(g_app.continuous_talk_checkbox, text(TextId::continuous_talk));
    }
    rebuild_menu_bar();
    if (refresh_contacts) {
        refresh_contact_list(selected_contact_index());
    }
}

void fill_rect_color(HDC dc, const RECT& rect, COLORREF color) {
    FillRect(dc, &rect, g_app.gdi_objects.brush(color));
}

void draw_contact_mute_button(HDC dc, const RECT& button_rect, bool muted, bool selected) {
    const COLORREF background = muted
        ? RGB(245, 220, 224)
        : (selected ? RGB(213, 229, 250) : RGB(238, 242, 247));
    const COLORREF border = muted ? RGB(197, 77, 90) : RGB(176, 185, 198);
    const COLORREF icon = muted ? RGB(170, 48, 62) : RGB(72, 84, 99);

    HBRUSH brush = g_app.gdi_objects.brush(background);
    HPEN border_pen = g_app.gdi_objects.pen(border);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, border_pen);
    RoundRect(dc, button_rect.left, button_rect.top, button_rect.right, button_rect.bottom, 8, 8);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);

    HPEN icon_pen = g_app.gdi_objects.pen(icon, 2);
    HGDIOBJ old_icon_pen = SelectObject(dc, icon_pen);
    const int center_y = button_rect.top + (button_rect.bottom - button_rect.top) / 2;
    const int speaker_left = button_rect.left + 8;
    const int speaker_top = center_y - 5;
    POINT speaker[] = {
        {speaker_left, center_y - 3},
        {speaker_left + 4, center_y - 3},
        {speaker_left + 9, speaker_top},
        {speaker_left + 9, speaker_top + 10},
        {speaker_left + 4, center_y + 3},
        {speaker_left, center_y + 3},
        {speaker_left, center_y - 3}};
    Polyline(dc, speaker, static_cast<int>(sizeof(speaker) / sizeof(speaker[0])));
    if (muted) {
        MoveToEx(dc, button_rect.right - 12, center_y - 6, nullptr);
        LineTo(dc, button_rect.right - 5, center_y + 6);
        MoveToEx(dc, button_rect.right - 5, center_y - 6, nullptr);
        LineTo(dc, button_rect.right - 12, center_y + 6);
    } else {
        Arc(dc, button_rect.right - 17, center_y - 7, button_rect.right - 5, center_y + 7,
            button_rect.right - 10, center_y - 7, button_rect.right - 10, center_y + 7);
    }
    SelectObject(dc, old_icon_pen);
}

void draw_contact_ptt_button(
    HDC dc,
    const RECT& button_rect,
    bool active,
    bool global_active,
    bool latched,
    bool selected) {
    const COLORREF background = active
        ? (global_active ? RGB(93, 142, 204) : RGB(44, 121, 209))
        : (selected ? RGB(213, 229, 250) : RGB(238, 242, 247));
    const COLORREF border = active
        ? (global_active ? RGB(67, 111, 173) : RGB(28, 92, 170))
        : RGB(176, 185, 198);
    const COLORREF text_color = active ? RGB(255, 255, 255) : RGB(64, 74, 87);

    HBRUSH brush = g_app.gdi_objects.brush(background);
    HPEN border_pen = g_app.gdi_objects.pen(border);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, border_pen);
    RoundRect(dc, button_rect.left, button_rect.top, button_rect.right, button_rect.bottom, 8, 8);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text_color);
    RECT text_rect = button_rect;
    DrawTextW(dc, L"PTT", 3, &text_rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);

    if (latched) {
        const COLORREF lock_color = RGB(255, 202, 58);
        HPEN lock_pen = g_app.gdi_objects.pen(lock_color, 1);
        HBRUSH lock_brush = g_app.gdi_objects.brush(lock_color);
        HGDIOBJ old_lock_pen = SelectObject(dc, lock_pen);
        HGDIOBJ old_lock_brush = SelectObject(dc, lock_brush);
        const int left = button_rect.right - 7;
        const int top = button_rect.top + 2;
        MoveToEx(dc, left + 1, top + 5, nullptr);
        LineTo(dc, left + 1, top + 3);
        LineTo(dc, left + 2, top + 1);
        LineTo(dc, left + 4, top + 1);
        LineTo(dc, left + 5, top + 3);
        LineTo(dc, left + 5, top + 5);
        Rectangle(dc, left, top + 5, left + 6, top + 10);
        SelectObject(dc, old_lock_brush);
        SelectObject(dc, old_lock_pen);
    }
}

void draw_contact_global_ptt_button(HDC dc, const RECT& button_rect, bool enabled, bool selected) {
    const COLORREF background = enabled
        ? (selected ? RGB(213, 229, 250) : RGB(238, 242, 247))
        : RGB(250, 235, 207);
    const COLORREF border = enabled ? RGB(176, 185, 198) : RGB(205, 148, 62);
    const COLORREF icon = enabled ? RGB(72, 84, 99) : RGB(167, 105, 24);

    HBRUSH brush = g_app.gdi_objects.brush(background);
    HPEN border_pen = g_app.gdi_objects.pen(border);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, border_pen);
    RoundRect(dc, button_rect.left, button_rect.top, button_rect.right, button_rect.bottom, 8, 8);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);

    draw_group_icon(dc, button_rect, icon, !enabled, g_app.gdi_objects);
}

void draw_global_ptt_button(const DRAWITEMSTRUCT& item) {
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool physically_pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool active = g_app.push_to_talk_down;
    const COLORREF background = disabled
        ? GetSysColor(COLOR_BTNFACE)
        : (active ? RGB(93, 142, 204)
                  : (physically_pressed ? RGB(218, 225, 234) : RGB(242, 244, 247)));
    const COLORREF border = disabled
        ? GetSysColor(COLOR_BTNSHADOW)
        : (active ? RGB(67, 111, 173) : RGB(155, 165, 178));
    const COLORREF foreground = disabled
        ? GetSysColor(COLOR_GRAYTEXT)
        : (active ? RGB(255, 255, 255) : RGB(54, 64, 77));

    HBRUSH brush = g_app.gdi_objects.brush(background);
    HPEN border_pen = g_app.gdi_objects.pen(border);
    HGDIOBJ old_brush = SelectObject(item.hDC, brush);
    HGDIOBJ old_pen = SelectObject(item.hDC, border_pen);
    RoundRect(
        item.hDC,
        item.rcItem.left,
        item.rcItem.top,
        item.rcItem.right,
        item.rcItem.bottom,
        5,
        5);
    SelectObject(item.hDC, old_pen);
    SelectObject(item.hDC, old_brush);

    const std::wstring label = get_window_text(item.hwndItem);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ old_font = font ? SelectObject(item.hDC, font) : nullptr;
    SIZE text_size{};
    GetTextExtentPoint32W(item.hDC, label.c_str(), static_cast<int>(label.size()), &text_size);

    constexpr int icon_size = 22;
    constexpr int content_gap = 5;
    const int content_width = icon_size + content_gap + text_size.cx;
    const int button_width = item.rcItem.right - item.rcItem.left;
    const int button_height = item.rcItem.bottom - item.rcItem.top;
    const int press_offset = physically_pressed && !active ? 1 : 0;
    const int content_left = item.rcItem.left + std::max(6, (button_width - content_width) / 2) + press_offset;
    const int center_y = item.rcItem.top + button_height / 2 + press_offset;
    RECT icon_rect{
        content_left,
        center_y - icon_size / 2,
        content_left + icon_size,
        center_y + (icon_size + 1) / 2};
    draw_group_icon(item.hDC, icon_rect, foreground, false, g_app.gdi_objects);

    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, foreground);
    RECT text_rect{
        icon_rect.right + content_gap,
        item.rcItem.top + press_offset,
        item.rcItem.right - 6 + press_offset,
        item.rcItem.bottom + press_offset};
    DrawTextW(
        item.hDC,
        label.c_str(),
        static_cast<int>(label.size()),
        &text_rect,
        DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (old_font) {
        SelectObject(item.hDC, old_font);
    }
    if ((item.itemState & ODS_FOCUS) != 0) {
        RECT focus_rect = item.rcItem;
        InflateRect(&focus_rect, -3, -3);
        DrawFocusRect(item.hDC, &focus_rect);
    }
}

void draw_contact_gain_button(HDC dc, const RECT& button_rect, wchar_t symbol, bool selected) {
    const COLORREF background = selected ? RGB(213, 229, 250) : RGB(238, 242, 247);
    const COLORREF border = RGB(176, 185, 198);
    const COLORREF icon = RGB(64, 74, 87);

    HBRUSH brush = g_app.gdi_objects.brush(background);
    HPEN border_pen = g_app.gdi_objects.pen(border);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, border_pen);
    RoundRect(dc, button_rect.left, button_rect.top, button_rect.right, button_rect.bottom, 6, 6);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, icon);
    RECT text_rect = button_rect;
    const wchar_t text[] = {symbol, L'\0'};
    DrawTextW(dc, text, 1, &text_rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
}

void draw_contact_gain_slider(HDC dc, const RECT& slider_rect, const Contact& contact, bool selected) {
    const int center_y = slider_rect.top + (slider_rect.bottom - slider_rect.top) / 2;
    RECT track{
        slider_rect.left + 4,
        center_y - 2,
        slider_rect.right - 4,
        center_y + 2};
    const double ratio = contact_gain_slider_ratio(contact);
    const int knob_x = track.left + static_cast<int>(
        std::lround(static_cast<double>(track.right - track.left) * ratio));

    fill_rect_color(dc, track, selected ? RGB(194, 210, 231) : RGB(215, 222, 231));
    RECT fill = track;
    fill.right = std::clamp<LONG>(static_cast<LONG>(knob_x), track.left, track.right);
    if (fill.right > fill.left) {
        fill_rect_color(dc, fill, contact.muted ? RGB(116, 126, 139) : RGB(70, 130, 205));
    }

    HBRUSH knob_brush = g_app.gdi_objects.brush(
        contact.muted ? RGB(78, 88, 101) : RGB(52, 105, 184));
    HPEN knob_pen = g_app.gdi_objects.pen(RGB(255, 255, 255));
    HGDIOBJ old_brush = SelectObject(dc, knob_brush);
    HGDIOBJ old_pen = SelectObject(dc, knob_pen);
    const int knob_radius = 6;
    Ellipse(dc, knob_x - knob_radius, center_y - knob_radius, knob_x + knob_radius + 1, center_y + knob_radius + 1);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
}

void draw_microphone_icon(HDC dc, const RECT& bounds, COLORREF color) {
    const int center_x = bounds.left + (bounds.right - bounds.left) / 2;
    const int center_y = bounds.top + (bounds.bottom - bounds.top) / 2;
    HPEN pen = g_app.gdi_objects.pen(color, 2);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));

    RoundRect(dc, center_x - 4, center_y - 9, center_x + 5, center_y + 4, 8, 8);
    POINT cradle[] = {
        {center_x - 8, center_y - 1},
        {center_x - 8, center_y + 3},
        {center_x - 6, center_y + 6},
        {center_x - 3, center_y + 8},
        {center_x + 3, center_y + 8},
        {center_x + 6, center_y + 6},
        {center_x + 8, center_y + 3},
        {center_x + 8, center_y - 1}};
    Polyline(dc, cradle, static_cast<int>(sizeof(cradle) / sizeof(cradle[0])));
    MoveToEx(dc, center_x, center_y + 8, nullptr);
    LineTo(dc, center_x, center_y + 11);
    MoveToEx(dc, center_x - 5, center_y + 11, nullptr);
    LineTo(dc, center_x + 6, center_y + 11);

    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
}

void draw_local_meter_to_dc(HDC dc, const RECT& client) {
    fill_rect_color(dc, client, GetSysColor(COLOR_WINDOW));

    RECT icon_rect{client.left + 2, client.top, client.left + 28, client.bottom};
    draw_microphone_icon(dc, icon_rect, RGB(72, 84, 99));

    const int center_y = client.top + (client.bottom - client.top) / 2;
    RECT track{client.left + 34, center_y - 4, client.right - 2, center_y + 4};
    if (track.right <= track.left) {
        return;
    }

    fill_rect_color(dc, track, RGB(226, 231, 238));
    const LONG track_width = track.right - track.left;
    const LONG fill_width = std::clamp<LONG>(
        static_cast<LONG>(std::lround(static_cast<double>(track_width) * vu_fill_ratio(g_app.local_level_db))),
        0,
        track_width);
    if (fill_width > 0) {
        RECT fill = track;
        fill.right = fill.left + fill_width;
        fill_rect_color(dc, fill, g_app.local_voice_active ? RGB(42, 168, 91) : RGB(102, 128, 154));
    }
}

void paint_local_meter(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    if (!dc) {
        return;
    }

    RECT client{};
    GetClientRect(window, &client);
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    const int height = std::max(1, static_cast<int>(client.bottom - client.top));
    if (g_app.local_meter_surface.ensure(dc, width, height)) {
        draw_local_meter_to_dc(g_app.local_meter_surface.dc(), client);
        BitBlt(dc, 0, 0, width, height, g_app.local_meter_surface.dc(), 0, 0, SRCCOPY);
    } else {
        draw_local_meter_to_dc(dc, client);
    }
    EndPaint(window, &paint);
}

LRESULT CALLBACK local_meter_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_local_meter(window);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool register_local_meter_class() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = local_meter_proc;
    window_class.hInstance = g_app.instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = L"LanSpeakLocalMeter";
    if (!RegisterClassExW(&window_class)) {
        return false;
    }
    registered = true;
    return true;
}

void draw_contact_card(HDC dc, const RECT& row_rect, size_t index, bool focused) {
    RECT card_rect = contact_card_rect_from_row(row_rect);

    const bool selected = static_cast<int>(index) == g_app.selected_contact_index;
    const bool muted = index < g_app.contacts.size() && g_app.contacts[index].muted;
    const COLORREF card_color = selected ? RGB(226, 239, 255) : RGB(248, 249, 251);
    const COLORREF border_color = selected ? RGB(55, 125, 220) : RGB(218, 223, 230);
    const COLORREF text_color = selected ? RGB(18, 43, 77) : RGB(26, 31, 38);

    HBRUSH card_brush = g_app.gdi_objects.brush(card_color);
    HPEN border_pen = g_app.gdi_objects.pen(border_color);
    HGDIOBJ old_brush = SelectObject(dc, card_brush);
    HGDIOBJ old_pen = SelectObject(dc, border_pen);
    RoundRect(dc, card_rect.left, card_rect.top, card_rect.right, card_rect.bottom, 10, 10);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);

    std::wstring name = text(TextId::unnamed_contact);
    if (index < g_app.contacts.size() && !g_app.contacts[index].name.empty()) {
        name = g_app.contacts[index].name;
    }

    RECT text_rect = card_rect;
    text_rect.left += 16;
    const RECT mute_button = contact_mute_button_rect_from_card(card_rect);
    const RECT ptt_button = contact_ptt_button_rect_from_card(card_rect);
    const RECT global_ptt_button = contact_global_ptt_button_rect_from_card(card_rect);
    const RECT gain_minus = contact_gain_minus_rect_from_card(card_rect);
    const RECT gain_slider = contact_gain_slider_rect_from_card(card_rect);
    const RECT gain_plus = contact_gain_plus_rect_from_card(card_rect);
    const bool show_gain_controls = contact_gain_controls_visible(card_rect);
    const int controls_left = show_gain_controls ? gain_minus.left : global_ptt_button.left;
    text_rect.right = std::max<LONG>(text_rect.left + 24, static_cast<LONG>(controls_left - 12));
    text_rect.top += 2;
    text_rect.bottom -= 2;

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text_color);
    HGDIOBJ old_font = nullptr;
    if (g_app.contact_name_font) {
        old_font = SelectObject(dc, g_app.contact_name_font);
    }
    DrawTextW(
        dc,
        name.c_str(),
        static_cast<int>(name.size()),
        &text_rect,
        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (old_font) {
        SelectObject(dc, old_font);
    }

    const lanspeak::gui::ContactMeterState* meter = g_app.contact_meters.get(index);
    const double level_db = meter != nullptr ? meter->level_db : -90.0;
    const bool voice_active = meter != nullptr && meter->voice_active;
    const bool stream_active = meter != nullptr && meter->stream_active;
    RECT vu_track{
        card_rect.left + 16,
        card_rect.bottom - 17,
        controls_left - 14,
        card_rect.bottom - 10};
    if (vu_track.right > vu_track.left + 12) {
        const COLORREF vu_track_color = muted
            ? (selected ? RGB(190, 198, 208) : RGB(214, 219, 225))
            : (selected ? RGB(202, 218, 238) : RGB(226, 231, 238));
        const COLORREF vu_fill_color = muted
            ? (voice_active ? RGB(74, 84, 97) : RGB(116, 126, 139))
            : (voice_active ? RGB(42, 168, 91) : RGB(102, 128, 154));
        fill_rect_color(dc, vu_track, vu_track_color);
        RECT vu_fill = vu_track;
        const LONG track_width = vu_track.right - vu_track.left;
        LONG fill_width = static_cast<LONG>(
            std::lround(static_cast<double>(track_width) * vu_fill_ratio(level_db)));
        if (stream_active) {
            fill_width = std::max<LONG>(
                fill_width,
                std::min<LONG>(track_width, kContactVuStreamMinimumPixels));
        }
        vu_fill.right = vu_fill.left + std::clamp<LONG>(fill_width, 0, track_width);
        if (vu_fill.right > vu_fill.left) {
            fill_rect_color(dc, vu_fill, vu_fill_color);
        }
    }
    if (show_gain_controls && index < g_app.contacts.size()) {
        draw_contact_gain_button(dc, gain_minus, L'-', selected);
        draw_contact_gain_slider(dc, gain_slider, g_app.contacts[index], selected);
        draw_contact_gain_button(dc, gain_plus, L'+', selected);
    }
    const bool global_ptt_enabled = index < g_app.contacts.size() && g_app.contacts[index].global_ptt_enabled;
    const bool global_talk_active = g_app.push_to_talk_down && !g_app.input_muted && global_ptt_enabled;
    const bool personal_talk_active = contact_push_to_talk_active(index);
    draw_contact_ptt_button(
        dc,
        ptt_button,
        global_talk_active || personal_talk_active,
        global_talk_active,
        contact_push_to_talk_latched(index),
        selected);
    draw_contact_global_ptt_button(dc, global_ptt_button, global_ptt_enabled, selected);
    draw_contact_mute_button(dc, mute_button, muted, selected);

    if (focused && selected) {
        RECT focus_rect = card_rect;
        InflateRect(&focus_rect, -3, -3);
        DrawFocusRect(dc, &focus_rect);
    }
}

void draw_contact_panel_to_dc(HWND panel, HDC dc, const RECT& client) {
    FillRect(dc, &client, g_app.gdi_objects.brush(GetSysColor(COLOR_WINDOW)));

    if (g_app.contacts.empty()) {
        return;
    }

    const bool focused = GetFocus() == panel;
    const int first_index = std::max(0, g_app.contact_scroll_offset / kContactItemHeight);
    int y = first_index * kContactItemHeight - g_app.contact_scroll_offset;

    for (size_t index = static_cast<size_t>(first_index);
         index < g_app.contacts.size() && y < client.bottom;
         ++index, y += kContactItemHeight) {
        RECT row_rect{client.left, y, client.right, y + kContactItemHeight};
        draw_contact_card(dc, row_rect, index, focused);
    }
}

void paint_contact_panel(HWND panel) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(panel, &paint);
    if (!dc) {
        return;
    }

    RECT client{};
    GetClientRect(panel, &client);
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    const int height = std::max(1, static_cast<int>(client.bottom - client.top));

    if (g_app.contact_surface.ensure(dc, width, height)) {
        draw_contact_panel_to_dc(panel, g_app.contact_surface.dc(), client);
        const RECT dirty = paint.rcPaint;
        BitBlt(
            dc,
            dirty.left,
            dirty.top,
            dirty.right - dirty.left,
            dirty.bottom - dirty.top,
            g_app.contact_surface.dc(),
            dirty.left,
            dirty.top,
            SRCCOPY);
    } else {
        draw_contact_panel_to_dc(panel, dc, client);
    }
    EndPaint(panel, &paint);
}

void move_contact_selection(int index) {
    set_selected_contact_index(index, true);
}

double contact_gain_from_slider_point(HWND panel, int index, POINT point);
void set_contact_gain(size_t index, double value, bool persist);
void adjust_selected_contact_gain(double delta);
void toggle_selected_contact_global_ptt();
void save_settings();
void set_button_push_to_talk_active(bool active);
void set_hotkey_push_to_talk_active(bool active);
void set_continuous_talk_active(bool active);
void reset_push_to_talk_state();
void update_push_to_talk_button_text();
bool set_contact_push_to_talk_active(size_t index, bool active);
void reset_contact_push_to_talk_state();
void restore_latched_talk_state_after_restart();

LRESULT CALLBACK contact_panel_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        hide_contact_tooltip();
        update_contact_scrollbar(window);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_PAINT:
        paint_contact_panel(window);
        return 0;
    case WM_VSCROLL: {
        hide_contact_tooltip();
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_ALL;
        GetScrollInfo(window, SB_VERT, &info);

        int target = g_app.contact_scroll_offset;
        switch (LOWORD(wparam)) {
        case SB_LINEUP:
            target -= 32;
            break;
        case SB_LINEDOWN:
            target += 32;
            break;
        case SB_PAGEUP:
            target -= static_cast<int>(info.nPage);
            break;
        case SB_PAGEDOWN:
            target += static_cast<int>(info.nPage);
            break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK:
            target = info.nTrackPos;
            break;
        case SB_TOP:
            target = 0;
            break;
        case SB_BOTTOM:
            target = max_contact_scroll_offset(window);
            break;
        default:
            break;
        }
        set_contact_scroll_offset(window, target);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        hide_contact_tooltip();
        const int delta = static_cast<short>(HIWORD(wparam));
        g_app.contact_wheel_remainder += delta * 48;
        const int pixels = g_app.contact_wheel_remainder / WHEEL_DELTA;
        g_app.contact_wheel_remainder %= WHEEL_DELTA;
        if (pixels != 0) {
            set_contact_scroll_offset(window, g_app.contact_scroll_offset - pixels);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        hide_contact_tooltip();
        g_app.contact_tooltip_index = -1;
        SetFocus(window);
        POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        int index = -1;
        const ContactHitAction action = contact_hit_test(window, point, index);
        if (index >= 0) {
            set_selected_contact_index(index, false);
        }
        if (action == ContactHitAction::mute) {
            PostMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(IDC_TOGGLE_MUTE, 0), 0);
            return 0;
        }
        if (action == ContactHitAction::global_ptt) {
            PostMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(IDC_TOGGLE_GLOBAL_PTT, 0), 0);
            return 0;
        }
        if (action == ContactHitAction::contact_ptt && index >= 0) {
            if (!(g_app.push_to_talk_down && !g_app.input_muted)) {
                if (set_contact_push_to_talk_active(static_cast<size_t>(index), true)) {
                    g_app.mouse_contact_ptt_index = index;
                    SetCapture(window);
                }
            }
            return 0;
        }
        if (action == ContactHitAction::gain_minus) {
            adjust_selected_contact_gain(-kContactGainStep);
            return 0;
        }
        if (action == ContactHitAction::gain_plus) {
            adjust_selected_contact_gain(kContactGainStep);
            return 0;
        }
        if (action == ContactHitAction::gain_slider && index >= 0) {
            g_app.dragging_gain_index = index;
            SetCapture(window);
            set_contact_gain(
                static_cast<size_t>(index),
                contact_gain_from_slider_point(window, index, point),
                false);
            return 0;
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_app.dragging_gain_index >= 0 && (wparam & MK_LBUTTON) != 0) {
            POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            set_contact_gain(
                static_cast<size_t>(g_app.dragging_gain_index),
                contact_gain_from_slider_point(window, g_app.dragging_gain_index, point),
                false);
            return 0;
        }
        if ((wparam & MK_LBUTTON) == 0) {
            POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            update_contact_tooltip_target(window, point);
        }
        break;
    case WM_MOUSEHOVER:
        show_contact_tooltip(window);
        return 0;
    case WM_MOUSELEAVE:
        hide_contact_tooltip();
        g_app.contact_tooltip_index = -1;
        return 0;
    case WM_LBUTTONUP:
        if (g_app.mouse_contact_ptt_index >= 0) {
            set_contact_push_to_talk_active(static_cast<size_t>(g_app.mouse_contact_ptt_index), false);
            g_app.mouse_contact_ptt_index = -1;
            if (GetCapture() == window) {
                ReleaseCapture();
            }
            return 0;
        }
        if (g_app.dragging_gain_index >= 0) {
            POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            const int index = g_app.dragging_gain_index;
            g_app.dragging_gain_index = -1;
            set_contact_gain(
                static_cast<size_t>(index),
                contact_gain_from_slider_point(window, index, point),
                true);
            if (GetCapture() == window) {
                ReleaseCapture();
            }
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (g_app.mouse_contact_ptt_index >= 0) {
            set_contact_push_to_talk_active(static_cast<size_t>(g_app.mouse_contact_ptt_index), false);
            g_app.mouse_contact_ptt_index = -1;
        }
        if (g_app.dragging_gain_index >= 0) {
            g_app.dragging_gain_index = -1;
            save_settings();
        }
        break;
    case WM_LBUTTONDBLCLK: {
        POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        int hit_index = -1;
        const ContactHitAction action = contact_hit_test(window, point, hit_index);
        if (action != ContactHitAction::select) {
            if (action != ContactHitAction::none) {
                SendMessageW(window, WM_LBUTTONDOWN, wparam, lparam);
            }
            return 0;
        }
        const int index = contact_index_from_point(window, point);
        if (index >= 0) {
            set_selected_contact_index(index, false);
            PostMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(IDC_UPDATE_CONTACT, 0), 0);
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        SetFocus(window);
        POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        const int index = contact_index_from_point(window, point);
        if (index >= 0) {
            set_selected_contact_index(index, false);
        }
        return 0;
    }
    case WM_CONTEXTMENU:
        SendMessageW(GetParent(window), WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window), lparam);
        return 0;
    case WM_KEYDOWN:
        if (g_app.contacts.empty()) {
            return 0;
        }
        switch (wparam) {
        case VK_UP:
            move_contact_selection(selected_contact_index() < 0 ? 0 : selected_contact_index() - 1);
            return 0;
        case VK_DOWN:
            move_contact_selection(selected_contact_index() < 0 ? 0 : selected_contact_index() + 1);
            return 0;
        case VK_PRIOR: {
            const int page_items = std::max(1, contact_panel_client_height(window) / kContactItemHeight);
            move_contact_selection(selected_contact_index() < 0 ? 0 : selected_contact_index() - page_items);
            return 0;
        }
        case VK_NEXT: {
            const int page_items = std::max(1, contact_panel_client_height(window) / kContactItemHeight);
            move_contact_selection(selected_contact_index() < 0 ? 0 : selected_contact_index() + page_items);
            return 0;
        }
        case VK_HOME:
            move_contact_selection(0);
            return 0;
        case VK_END:
            move_contact_selection(static_cast<int>(g_app.contacts.size()) - 1);
            return 0;
        case VK_RETURN:
            if (selected_contact_index() >= 0) {
                PostMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(IDC_UPDATE_CONTACT, 0), 0);
            }
            return 0;
        default:
            break;
        }
        break;
    default:
        break;
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

bool register_contact_panel_class() {
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_DBLCLKS;
    window_class.lpfnWndProc = contact_panel_proc;
    window_class.hInstance = g_app.instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = L"LanSpeakContactPanel";
    if (!RegisterClassExW(&window_class)) {
        return false;
    }
    registered = true;
    return true;
}

void center_window_on_owner(HWND window, HWND owner) {
    RECT window_rect{};
    RECT owner_rect{};
    GetWindowRect(window, &window_rect);
    if (owner) {
        GetWindowRect(owner, &owner_rect);
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &owner_rect, 0);
    }

    const int width = window_rect.right - window_rect.left;
    const int height = window_rect.bottom - window_rect.top;
    const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
    const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
}

constexpr int kHotkeyRecordingNone = -2;
constexpr int kHotkeyRecordingAll = -1;
constexpr int kHotkeyContactRowHeight = 36;
constexpr int kHotkeyContactVisibleRows = 6;
constexpr int kHotkeyContactListTop = 82;
constexpr int kHotkeyContactListWidth = 528;

struct HotkeyDialogState {
    HWND window = nullptr;
    HWND owner = nullptr;
    HWND display = nullptr;
    HWND record_button = nullptr;
    HWND clear_button = nullptr;
    HWND contact_heading = nullptr;
    HWND contact_list = nullptr;
    Hotkey ptt_all_hotkey;
    std::vector<Hotkey> contact_hotkeys;
    std::vector<HWND> contact_labels;
    std::vector<HWND> contact_displays;
    std::vector<HWND> contact_record_buttons;
    std::vector<HWND> contact_clear_buttons;
    HFONT heading_font = nullptr;
    bool accepted = false;
    int recording_index = kHotkeyRecordingNone;
    UINT recording_mouse_vk = 0;
    int contact_scroll_offset = 0;
    bool owner_restored = false;
};

int hotkey_contact_content_height(const HotkeyDialogState& state) {
    return static_cast<int>(state.contact_hotkeys.size()) * kHotkeyContactRowHeight;
}

int hotkey_contact_visible_height(const HotkeyDialogState& state) {
    return std::min(hotkey_contact_content_height(state), kHotkeyContactVisibleRows * kHotkeyContactRowHeight);
}

bool hotkey_contact_needs_scroll(const HotkeyDialogState& state) {
    return hotkey_contact_content_height(state) > kHotkeyContactVisibleRows * kHotkeyContactRowHeight;
}

int hotkey_dialog_button_y(const HotkeyDialogState& state) {
    return kHotkeyContactListTop + hotkey_contact_visible_height(state) + 18;
}

int hotkey_dialog_window_height(const HotkeyDialogState& state) {
    return hotkey_dialog_button_y(state) + 86;
}

Hotkey& hotkey_dialog_hotkey(HotkeyDialogState& state, int index) {
    return index == kHotkeyRecordingAll ? state.ptt_all_hotkey : state.contact_hotkeys[static_cast<size_t>(index)];
}

const Hotkey& hotkey_dialog_hotkey(const HotkeyDialogState& state, int index) {
    return index == kHotkeyRecordingAll ? state.ptt_all_hotkey : state.contact_hotkeys[static_cast<size_t>(index)];
}

HWND hotkey_dialog_display(const HotkeyDialogState& state, int index) {
    if (index == kHotkeyRecordingAll) {
        return state.display;
    }
    const size_t contact_index = static_cast<size_t>(index);
    return contact_index < state.contact_displays.size() ? state.contact_displays[contact_index] : nullptr;
}

HWND hotkey_dialog_record_button(const HotkeyDialogState& state, int index) {
    if (index == kHotkeyRecordingAll) {
        return state.record_button;
    }
    const size_t contact_index = static_cast<size_t>(index);
    return contact_index < state.contact_record_buttons.size() ? state.contact_record_buttons[contact_index] : nullptr;
}

int max_hotkey_contact_scroll_offset(const HotkeyDialogState& state) {
    if (!state.contact_list) {
        return 0;
    }
    RECT client{};
    GetClientRect(state.contact_list, &client);
    const int page_height = std::max(0, static_cast<int>(client.bottom - client.top));
    return std::max(0, hotkey_contact_content_height(state) - page_height);
}

void layout_hotkey_contact_rows(HotkeyDialogState& state) {
    const int y_offset = state.contact_scroll_offset;
    for (size_t index = 0; index < state.contact_hotkeys.size(); ++index) {
        const int y = static_cast<int>(index) * kHotkeyContactRowHeight - y_offset;
        if (index < state.contact_labels.size() && state.contact_labels[index]) {
            MoveWindow(state.contact_labels[index], 10, y + 4, 142, 20, TRUE);
        }
        if (index < state.contact_displays.size() && state.contact_displays[index]) {
            MoveWindow(state.contact_displays[index], 162, y, 174, 26, TRUE);
        }
        if (index < state.contact_record_buttons.size() && state.contact_record_buttons[index]) {
            MoveWindow(state.contact_record_buttons[index], 348, y - 1, 70, 28, TRUE);
        }
        if (index < state.contact_clear_buttons.size() && state.contact_clear_buttons[index]) {
            MoveWindow(state.contact_clear_buttons[index], 428, y - 1, 82, 28, TRUE);
        }
    }
}

void update_hotkey_contact_scrollbar(HotkeyDialogState& state) {
    if (!state.contact_list) {
        return;
    }

    RECT client{};
    GetClientRect(state.contact_list, &client);
    const int page_height = std::max(1, static_cast<int>(client.bottom - client.top));
    const int content_height = hotkey_contact_content_height(state);
    const bool needs_scroll = content_height > page_height;
    state.contact_scroll_offset = std::clamp(state.contact_scroll_offset, 0, max_hotkey_contact_scroll_offset(state));

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = std::max(0, content_height - 1);
    info.nPage = static_cast<UINT>(page_height);
    info.nPos = state.contact_scroll_offset;
    SetScrollInfo(state.contact_list, SB_VERT, &info, TRUE);
    ShowScrollBar(state.contact_list, SB_VERT, needs_scroll);
}

void set_hotkey_contact_scroll_offset(HotkeyDialogState& state, int offset) {
    state.contact_scroll_offset = std::clamp(offset, 0, max_hotkey_contact_scroll_offset(state));
    update_hotkey_contact_scrollbar(state);
    layout_hotkey_contact_rows(state);
    if (state.contact_list) {
        InvalidateRect(state.contact_list, nullptr, TRUE);
    }
}

void update_hotkey_dialog_display(HotkeyDialogState& state, int index) {
    if (index == kHotkeyRecordingNone) {
        return;
    }
    HWND display = hotkey_dialog_display(state, index);
    if (!display) {
        return;
    }

    const std::wstring display_text = state.recording_index == index
        ? std::wstring(text(TextId::hotkey_recording))
        : format_hotkey(hotkey_dialog_hotkey(state, index));
    SetWindowTextW(display, display_text.c_str());

    HWND record_button = hotkey_dialog_record_button(state, index);
    if (record_button) {
        SetWindowTextW(record_button, text(TextId::hotkey_record));
    }
}

void update_hotkey_dialog_displays(HotkeyDialogState& state) {
    update_hotkey_dialog_display(state, kHotkeyRecordingAll);
    for (size_t index = 0; index < state.contact_hotkeys.size(); ++index) {
        update_hotkey_dialog_display(state, static_cast<int>(index));
    }
}

void set_hotkey_dialog_recording(HotkeyDialogState& state, int index) {
    const int previous = state.recording_index;
    state.recording_index = index;
    state.recording_mouse_vk = 0;
    if (index == kHotkeyRecordingNone) {
        if (state.window && GetCapture() == state.window) {
            ReleaseCapture();
        }
    } else if (state.window) {
        SetCapture(state.window);
    }
    update_hotkey_dialog_display(state, previous);
    update_hotkey_dialog_display(state, index);
}

LRESULT CALLBACK hotkey_contact_list_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<HotkeyDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_SIZE:
        if (state) {
            update_hotkey_contact_scrollbar(*state);
            layout_hotkey_contact_rows(*state);
        }
        return 0;
    case WM_VSCROLL:
        if (state) {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_ALL;
            GetScrollInfo(window, SB_VERT, &info);

            int target = state->contact_scroll_offset;
            switch (LOWORD(wparam)) {
            case SB_LINEUP:
                target -= 24;
                break;
            case SB_LINEDOWN:
                target += 24;
                break;
            case SB_PAGEUP:
                target -= static_cast<int>(info.nPage);
                break;
            case SB_PAGEDOWN:
                target += static_cast<int>(info.nPage);
                break;
            case SB_THUMBPOSITION:
            case SB_THUMBTRACK:
                target = info.nTrackPos;
                break;
            case SB_TOP:
                target = 0;
                break;
            case SB_BOTTOM:
                target = max_hotkey_contact_scroll_offset(*state);
                break;
            default:
                break;
            }
            set_hotkey_contact_scroll_offset(*state, target);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (state) {
            const int delta = static_cast<short>(HIWORD(wparam));
            set_hotkey_contact_scroll_offset(*state, state->contact_scroll_offset - delta * 48 / WHEEL_DELTA);
        }
        return 0;
    case WM_COMMAND:
        SendMessageW(GetParent(window), WM_COMMAND, wparam, lparam);
        return 0;
    case WM_ERASEBKGND: {
        RECT client{};
        GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wparam), &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        return 1;
    }
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void create_hotkey_dialog_row(
    HWND window,
    const std::wstring& label,
    int y,
    int display_id,
    int record_id,
    int clear_id,
    HWND& display,
    HWND& record_button,
    HWND& clear_button) {
    add_label(window, label.c_str(), 16, y + 4, 150, 20);
    display = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"STATIC",
        L"",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_LEFTNOWORDWRAP,
        178,
        y,
        190,
        26,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(display_id)),
        g_app.instance,
        nullptr);
    set_font(display);

    record_button = add_button(window, record_id, text(TextId::hotkey_record), 382, y - 1, 70, 28);
    clear_button = add_button(window, clear_id, text(TextId::hotkey_clear), 462, y - 1, 82, 28);
}

void create_hotkey_contact_dialog_row(
    HWND parent,
    const std::wstring& label,
    int y,
    int display_id,
    int record_id,
    int clear_id,
    HWND& label_control,
    HWND& display,
    HWND& record_button,
    HWND& clear_button) {
    label_control = add_label(parent, label.c_str(), 10, y + 4, 142, 20);
    display = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"STATIC",
        L"",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_LEFTNOWORDWRAP,
        162,
        y,
        174,
        26,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(display_id)),
        g_app.instance,
        nullptr);
    set_font(display);

    record_button = add_button(parent, record_id, text(TextId::hotkey_record), 348, y - 1, 70, 28);
    clear_button = add_button(parent, clear_id, text(TextId::hotkey_clear), 428, y - 1, 82, 28);
}

void create_hotkey_dialog_controls(HWND window, HotkeyDialogState& state) {
    create_hotkey_dialog_row(
        window,
        text(TextId::ptt_for_all),
        16,
        IDC_HOTKEY_DISPLAY,
        IDC_HOTKEY_RECORD,
        IDC_HOTKEY_CLEAR,
        state.display,
        state.record_button,
        state.clear_button);

    state.contact_heading = add_label(window, text(TextId::contact_hotkeys), 16, 56, 150, 22);
    state.heading_font = create_ui_font(window, 10, FW_SEMIBOLD);
    if (state.heading_font) {
        SendMessageW(state.contact_heading, WM_SETFONT, reinterpret_cast<WPARAM>(state.heading_font), TRUE);
    }

    const int list_height = hotkey_contact_visible_height(state);
    if (list_height > 0) {
        DWORD list_style = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        if (hotkey_contact_needs_scroll(state)) {
            list_style |= WS_VSCROLL;
        }
        state.contact_list = CreateWindowExW(
            WS_EX_CONTROLPARENT,
            L"LanSpeakHotkeyContactList",
            L"",
            list_style,
            16,
            kHotkeyContactListTop,
            kHotkeyContactListWidth,
            list_height,
            window,
            nullptr,
            g_app.instance,
            &state);
    }

    state.contact_labels.reserve(state.contact_hotkeys.size());
    state.contact_displays.reserve(state.contact_hotkeys.size());
    state.contact_record_buttons.reserve(state.contact_hotkeys.size());
    state.contact_clear_buttons.reserve(state.contact_hotkeys.size());
    for (size_t index = 0; state.contact_list && index < state.contact_hotkeys.size(); ++index) {
        std::wstring name = text(TextId::unnamed_contact);
        if (index < g_app.contacts.size() && !g_app.contacts[index].name.empty()) {
            name = g_app.contacts[index].name;
        }

        HWND label_control = nullptr;
        HWND display = nullptr;
        HWND record_button = nullptr;
        HWND clear_button = nullptr;
        create_hotkey_contact_dialog_row(
            state.contact_list,
            name,
            static_cast<int>(index) * kHotkeyContactRowHeight,
            IDC_HOTKEY_DISPLAY + 100 + static_cast<int>(index),
            IDC_HOTKEY_CONTACT_RECORD_BASE + static_cast<int>(index),
            IDC_HOTKEY_CONTACT_CLEAR_BASE + static_cast<int>(index),
            label_control,
            display,
            record_button,
            clear_button);
        state.contact_labels.push_back(label_control);
        state.contact_displays.push_back(display);
        state.contact_record_buttons.push_back(record_button);
        state.contact_clear_buttons.push_back(clear_button);
    }
    update_hotkey_contact_scrollbar(state);
    layout_hotkey_contact_rows(state);

    const int button_y = hotkey_dialog_button_y(state);
    add_button(window, IDOK, text(TextId::save), 350, button_y, 94, 28);
    add_button(window, IDCANCEL, text(TextId::cancel), 454, button_y, 90, 28);
    update_hotkey_dialog_displays(state);
}

void restore_hotkey_dialog_owner(HotkeyDialogState& state) {
    if (state.owner_restored) {
        return;
    }
    state.owner_restored = true;
    if (state.owner && IsWindow(state.owner)) {
        EnableWindow(state.owner, TRUE);
        SetActiveWindow(state.owner);
    }
}

void close_hotkey_dialog(HWND window, HotkeyDialogState* state) {
    if (state) {
        set_hotkey_dialog_recording(*state, kHotkeyRecordingNone);
        restore_hotkey_dialog_owner(*state);
    }
    DestroyWindow(window);
    if (state && state->heading_font) {
        DeleteObject(state->heading_font);
        state->heading_font = nullptr;
    }
}

LRESULT CALLBACK hotkey_dialog_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<HotkeyDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE:
        state = reinterpret_cast<HotkeyDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (state) {
            state->window = window;
            create_hotkey_dialog_controls(window, *state);
            SetFocus(state->record_button);
            return 0;
        }
        return -1;
    case WM_GETDLGCODE:
        if (state && state->recording_index != kHotkeyRecordingNone) {
            return DLGC_WANTALLKEYS;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (state && state->recording_index != kHotkeyRecordingNone) {
            const UINT vk = static_cast<UINT>(wparam);
            if (vk == VK_ESCAPE) {
                set_hotkey_dialog_recording(*state, kHotkeyRecordingNone);
                return 0;
            }
            if (is_modifier_key(vk)) {
                return 0;
            }

            Hotkey& hotkey = hotkey_dialog_hotkey(*state, state->recording_index);
            hotkey.modifiers = current_hotkey_modifiers();
            hotkey.vk = vk;
            set_hotkey_dialog_recording(*state, kHotkeyRecordingNone);
            return 0;
        }
        break;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        if (state && state->recording_index != kHotkeyRecordingNone) {
            UINT vk = 0;
            if (message == WM_RBUTTONDOWN) vk = VK_RBUTTON;
            if (message == WM_MBUTTONDOWN) vk = VK_MBUTTON;
            if (message == WM_XBUTTONDOWN) {
                const WORD button = HIWORD(wparam);
                if (button == XBUTTON1) vk = VK_XBUTTON1;
                if (button == XBUTTON2) vk = VK_XBUTTON2;
            }
            if (is_supported_mouse_hotkey(vk)) {
                Hotkey& hotkey = hotkey_dialog_hotkey(*state, state->recording_index);
                hotkey.modifiers = 0;
                hotkey.vk = vk;
                state->recording_mouse_vk = vk;
                return message == WM_XBUTTONDOWN ? TRUE : 0;
            }
        }
        break;
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP:
        if (state && state->recording_index != kHotkeyRecordingNone && state->recording_mouse_vk != 0) {
            UINT vk = 0;
            if (message == WM_RBUTTONUP) vk = VK_RBUTTON;
            if (message == WM_MBUTTONUP) vk = VK_MBUTTON;
            if (message == WM_XBUTTONUP) {
                const WORD button = HIWORD(wparam);
                if (button == XBUTTON1) vk = VK_XBUTTON1;
                if (button == XBUTTON2) vk = VK_XBUTTON2;
            }
            if (vk == state->recording_mouse_vk) {
                set_hotkey_dialog_recording(*state, kHotkeyRecordingNone);
                return message == WM_XBUTTONUP ? TRUE : 0;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        if (state && state->recording_index != kHotkeyRecordingNone) {
            set_hotkey_dialog_recording(*state, kHotkeyRecordingNone);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (state && state->recording_index != kHotkeyRecordingNone &&
            reinterpret_cast<HWND>(lparam) != state->window) {
            set_hotkey_dialog_recording(*state, kHotkeyRecordingNone);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_HOTKEY_RECORD:
            if (state) {
                set_hotkey_dialog_recording(*state, kHotkeyRecordingAll);
                SetFocus(window);
            }
            return 0;
        case IDC_HOTKEY_CLEAR:
            if (state) {
                state->ptt_all_hotkey = {};
                set_hotkey_dialog_recording(*state, kHotkeyRecordingNone);
                update_hotkey_dialog_display(*state, kHotkeyRecordingAll);
            }
            return 0;
        case IDOK:
            if (state) {
                state->accepted = true;
            }
            close_hotkey_dialog(window, state);
            return 0;
        case IDCANCEL:
            close_hotkey_dialog(window, state);
            return 0;
        default:
            if (state) {
                const int command_id = LOWORD(wparam);
                const int record_index = command_id - IDC_HOTKEY_CONTACT_RECORD_BASE;
                if (record_index >= 0 && static_cast<size_t>(record_index) < state->contact_hotkeys.size()) {
                    set_hotkey_dialog_recording(*state, record_index);
                    SetFocus(window);
                    return 0;
                }

                const int clear_index = command_id - IDC_HOTKEY_CONTACT_CLEAR_BASE;
                if (clear_index >= 0 && static_cast<size_t>(clear_index) < state->contact_hotkeys.size()) {
                    state->contact_hotkeys[static_cast<size_t>(clear_index)] = {};
                    set_hotkey_dialog_recording(*state, kHotkeyRecordingNone);
                    update_hotkey_dialog_display(*state, clear_index);
                    return 0;
                }
            }
            break;
        }
        break;
    case WM_CLOSE:
        close_hotkey_dialog(window, state);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void show_global_hotkeys_dialog(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = hotkey_dialog_proc;
        window_class.hInstance = g_app.instance;
        window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.hIcon = load_app_large_icon();
        window_class.hIconSm = load_app_small_icon();
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = L"LanSpeakHotkeyDialog";
        if (!RegisterClassExW(&window_class)) {
            return;
        }

        WNDCLASSEXW list_class{};
        list_class.cbSize = sizeof(list_class);
        list_class.lpfnWndProc = hotkey_contact_list_proc;
        list_class.hInstance = g_app.instance;
        list_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        list_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        list_class.lpszClassName = L"LanSpeakHotkeyContactList";
        if (!RegisterClassExW(&list_class)) {
            return;
        }
        registered = true;
    }

    HotkeyDialogState state;
    state.owner = owner;
    state.ptt_all_hotkey = g_app.ptt_all_hotkey;
    state.contact_hotkeys.reserve(g_app.contacts.size());
    for (const Contact& contact : g_app.contacts) {
        state.contact_hotkeys.push_back(contact.ptt_hotkey);
    }

    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        L"LanSpeakHotkeyDialog",
        text(TextId::global_hotkeys),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        572,
        hotkey_dialog_window_height(state),
        owner,
        nullptr,
        g_app.instance,
        &state);
    if (!dialog) {
        return;
    }

    remove_global_hotkey_hook();
    center_window_on_owner(dialog, owner);
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (!state.owner_restored) {
        restore_hotkey_dialog_owner(state);
    }
    SetActiveWindow(owner);
    if (state.accepted) {
        deactivate_global_hotkey_ptt();
        deactivate_contact_hotkey_ptt();
        g_app.ptt_all_hotkey = state.ptt_all_hotkey;
        const size_t count = std::min(g_app.contacts.size(), state.contact_hotkeys.size());
        for (size_t index = 0; index < count; ++index) {
            g_app.contacts[index].ptt_hotkey = state.contact_hotkeys[index];
        }
        save_settings();
    }
    update_global_hotkey_hook();
}

std::wstring build_command_line(
    const std::wstring& exe_path,
    CoreMode mode,
    HANDLE telemetry_write,
    HANDLE control_read) {
    std::vector<std::wstring> args;
    args.push_back(exe_path);

    args.push_back(L"--capture-role");
    args.push_back(L"console");
    args.push_back(L"--render-role");
    args.push_back(L"console");

    args.push_back(L"--input-gain");
    args.push_back(L"1.0");
    args.push_back(L"--input-muted");

    args.push_back(L"--output-gain");
    args.push_back(L"1.0");

    args.push_back(L"--bind-address");
    args.push_back(g_app.bind_address);

    if (telemetry_write && telemetry_write != INVALID_HANDLE_VALUE) {
        args.push_back(L"--telemetry-handle");
        args.push_back(std::to_wstring(reinterpret_cast<std::uintptr_t>(telemetry_write)));
    }
    if (control_read && control_read != INVALID_HANDLE_VALUE) {
        args.push_back(L"--control-handle");
        args.push_back(std::to_wstring(reinterpret_cast<std::uintptr_t>(control_read)));
    }

    const std::wstring capture_device = selected_device_selector(g_app.capture_device, g_app.capture_device_selectors);
    if (!capture_device.empty()) {
        args.push_back(L"--capture-device");
        args.push_back(capture_device);
    }

    const std::wstring render_device = selected_device_selector(g_app.render_device, g_app.render_device_selectors);
    if (!render_device.empty()) {
        args.push_back(L"--render-device");
        args.push_back(render_device);
    }

    args.push_back(L"--room");
    args.push_back(std::to_wstring(g_app.local_port));
    if (mode == CoreMode::listener) {
        args.push_back(L"--listen-only");
    }
    for (const Contact& contact : g_app.contacts) {
        args.push_back(L"--peer");
        args.push_back(contact.host);
        args.push_back(std::to_wstring(contact.port));
        args.push_back(contact_runtime_gain_text(contact));
        args.push_back(contact.self_duck ? format_decimal(contact.duck_db, 2) : L"0");
        args.push_back(format_decimal(contact.duck_threshold, 4));
        args.push_back(std::to_wstring(contact.duck_attack_ms));
        args.push_back(std::to_wstring(contact.duck_hold_ms));
        args.push_back(std::to_wstring(contact.duck_release_ms));
        args.push_back(contact.global_ptt_enabled ? L"1" : L"0");
        args.push_back(std::to_wstring(contact.receive_buffer_ms));
    }

    return join_command_line(args);
}

bool run_core_command_capture(const std::vector<std::wstring>& core_args, std::wstring& output, DWORD& exit_code) {
    output.clear();
    exit_code = STILL_ACTIVE;

    const std::wstring exe_path = find_core_exe();
    if (!file_exists(exe_path)) {
        output = std::wstring(text(TextId::core_missing)) + exe_path;
        return false;
    }

    std::vector<std::wstring> args;
    args.push_back(exe_path);
    args.insert(args.end(), core_args.begin(), core_args.end());
    std::wstring command_line = join_command_line(args);

    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &security_attributes, 0)) {
        output = text(TextId::create_pipe_failed);
        return false;
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &security_attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdOutput = stdout_write;
    startup_info.hStdError = stdout_write;
    startup_info.hStdInput = null_input != INVALID_HANDLE_VALUE ? null_input : GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process_info{};
    const std::wstring working_directory = module_directory();
    const BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, working_directory.c_str(),
                                        &startup_info, &process_info);

    CloseHandle(stdout_write);
    if (null_input != INVALID_HANDLE_VALUE) {
        CloseHandle(null_input);
    }

    if (!created) {
        CloseHandle(stdout_read);
        std::wstringstream message;
        message << text(TextId::create_process_failed) << GetLastError();
        output = message.str();
        return false;
    }

    std::vector<char> bytes;
    char buffer[4096];
    for (;;) {
        DWORD bytes_read = 0;
        const BOOL ok = ReadFile(stdout_read, buffer, sizeof(buffer), &bytes_read, nullptr);
        if (!ok || bytes_read == 0) {
            break;
        }
        bytes.insert(bytes.end(), buffer, buffer + bytes_read);
    }

    WaitForSingleObject(process_info.hProcess, INFINITE);
    GetExitCodeProcess(process_info.hProcess, &exit_code);
    CloseHandle(stdout_read);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    output = bytes.empty() ? L"" : decode_process_output(bytes.data(), static_cast<int>(bytes.size()));
    return true;
}

std::vector<std::wstring> split_tab_fields(const std::wstring& line) {
    std::vector<std::wstring> fields;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find(L'\t', start);
        if (tab == std::wstring::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return fields;
}

std::wstring unescape_device_list_field(const std::wstring& value) {
    std::wstring unescaped;
    unescaped.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        const wchar_t ch = value[index];
        if (ch != L'\\' || index + 1 >= value.size()) {
            unescaped.push_back(ch);
            continue;
        }

        const wchar_t next = value[++index];
        switch (next) {
        case L't':
            unescaped.push_back(L'\t');
            break;
        case L'r':
            unescaped.push_back(L'\r');
            break;
        case L'n':
            unescaped.push_back(L'\n');
            break;
        case L'\\':
            unescaped.push_back(L'\\');
            break;
        default:
            unescaped.push_back(next);
            break;
        }
    }
    return unescaped;
}

void select_combo_by_selector(HWND combo, const std::vector<std::wstring>& selectors, const std::wstring& selector) {
    if (selector.empty()) {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        return;
    }
    for (size_t index = 0; index < selectors.size(); ++index) {
        if (selectors[index] == selector) {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
            return;
        }
    }
}

SIZE normal_window_size() {
    SIZE size{kInitialWindowWidth, kInitialWindowHeight};
    if (!g_app.main_window || !IsWindow(g_app.main_window)) {
        return size;
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    RECT rect{};
    if (GetWindowPlacement(g_app.main_window, &placement)) {
        rect = placement.rcNormalPosition;
    } else if (!GetWindowRect(g_app.main_window, &rect)) {
        return size;
    }

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width > 0 && height > 0) {
        size.cx = width;
        size.cy = height;
    }
    return size;
}

void restore_saved_window_size() {
    if (!g_app.main_window || !IsWindow(g_app.main_window)) {
        return;
    }

    RECT work_area{};
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    const HMONITOR monitor = MonitorFromWindow(g_app.main_window, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &monitor_info)) {
        work_area = monitor_info.rcWork;
    } else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0)) {
        work_area = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }

    const int available_width = std::max(1, static_cast<int>(work_area.right - work_area.left));
    const int available_height = std::max(1, static_cast<int>(work_area.bottom - work_area.top));
    const int minimum_width = std::min(kMinimumWindowWidth, available_width);
    const int minimum_height = std::min(GetSystemMetrics(SM_CYMINTRACK), available_height);
    const int width = std::clamp(g_app.saved_window_width, minimum_width, available_width);
    const int height = std::clamp(g_app.saved_window_height, minimum_height, available_height);

    SetWindowPos(
        g_app.main_window,
        nullptr,
        0,
        0,
        width,
        height,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void save_settings() {
    const std::wstring path = settings_path();
    AppSettings settings;
    const SIZE window_size = normal_window_size();
    settings.window_width = window_size.cx;
    settings.window_height = window_size.cy;
    settings.language = g_app.language_setting;
    settings.ptt_all_hotkey = g_app.ptt_all_hotkey;
    settings.debug_console_visible = g_app.debug_console_visible;
    settings.local_port = g_app.local_port;
    settings.network_adapter_id = g_app.network_adapter_id;
    settings.capture_device_selector = selected_device_selector(g_app.capture_device, g_app.capture_device_selectors);
    settings.render_device_selector = selected_device_selector(g_app.render_device, g_app.render_device_selectors);
    settings.contacts = g_app.contacts;

    if (!lanspeak::gui::save_settings_file(path, settings)) {
        append_log(std::wstring(text(TextId::save_settings_failed)) + path + L"\r\n");
    }
}

void load_settings() {
    AppSettings settings;
    const std::wstring current_path = settings_path();
    if (!lanspeak::gui::load_settings_file(current_path, settings)) {
        const std::wstring legacy_path = legacy_settings_path();
        if (!lanspeak::gui::load_settings_file(legacy_path, settings)) {
            return;
        }
        if (!lanspeak::gui::save_settings_file(current_path, settings)) {
            append_log(std::wstring(text(TextId::save_settings_failed)) + current_path + L"\r\n");
        }
    }

    g_app.contacts = std::move(settings.contacts);
    g_app.saved_window_width = settings.window_width;
    g_app.saved_window_height = settings.window_height;
    g_app.language_setting = settings.language;
    g_app.ptt_all_hotkey = settings.ptt_all_hotkey;
    g_app.debug_console_visible = settings.debug_console_visible;
    g_app.saved_capture_device_selector = std::move(settings.capture_device_selector);
    g_app.saved_render_device_selector = std::move(settings.render_device_selector);
    g_app.local_port = settings.local_port;
    g_app.network_adapter_id = std::move(settings.network_adapter_id);

    update_global_hotkey_hook();
    if (g_app.log) {
        ShowWindow(g_app.log, g_app.debug_console_visible ? SW_SHOW : SW_HIDE);
    }
    apply_language_to_main_window(false);
    refresh_contact_list(0);
    restore_saved_window_size();
}

bool resolve_saved_network_binding() {
    const std::vector<lanspeak::gui::NetworkAdapterInfo> adapters =
        lanspeak::gui::enumerate_active_ipv4_adapters();
    const std::optional<std::wstring> address =
        lanspeak::gui::resolve_network_bind_address(adapters, g_app.network_adapter_id);
    if (address) {
        g_app.bind_address = *address;
        return false;
    }

    g_app.network_adapter_id.clear();
    g_app.bind_address = L"0.0.0.0";
    save_settings();
    return true;
}

void add_device_combo_item(
    HWND combo,
    std::vector<std::wstring>& selectors,
    std::vector<std::wstring>& labels,
    const std::wstring& label,
    const std::wstring& selector) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    selectors.push_back(selector);
    labels.push_back(label);
}

void reset_device_combos() {
    SendMessageW(g_app.capture_device, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_app.render_device, CB_RESETCONTENT, 0, 0);
    g_app.capture_device_selectors.clear();
    g_app.render_device_selectors.clear();
    g_app.capture_device_labels.clear();
    g_app.render_device_labels.clear();

    add_device_combo_item(
        g_app.capture_device,
        g_app.capture_device_selectors,
        g_app.capture_device_labels,
        text(TextId::default_capture_device),
        L"");
    add_device_combo_item(
        g_app.render_device,
        g_app.render_device_selectors,
        g_app.render_device_labels,
        text(TextId::default_render_device),
        L"");
    SendMessageW(g_app.capture_device, CB_SETCURSEL, 0, 0);
    SendMessageW(g_app.render_device, CB_SETCURSEL, 0, 0);
}

bool parse_device_list_output(const std::wstring& output, int& capture_count, int& render_count) {
    capture_count = 0;
    render_count = 0;

    size_t start = 0;
    while (start < output.size()) {
        size_t end = output.find(L'\n', start);
        if (end == std::wstring::npos) {
            end = output.size();
        }

        std::wstring line = output.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        start = end + 1;

        if (line.rfind(L"DEVICE\t", 0) != 0) {
            continue;
        }

        const std::vector<std::wstring> fields = split_tab_fields(line);
        if (fields.size() < 6) {
            continue;
        }

        const std::wstring flow = fields[1];
        const std::wstring index = fields[2];
        const std::wstring name = unescape_device_list_field(fields[3]);
        const std::wstring id = unescape_device_list_field(fields[4]);
        const std::wstring markers = unescape_device_list_field(fields[5]);

        std::wstring label = L"[" + index + L"] " + name;
        if (!markers.empty()) {
            label += L" (" + markers + L")";
        }

        if (flow == L"capture") {
            add_device_combo_item(g_app.capture_device, g_app.capture_device_selectors, g_app.capture_device_labels, label, id);
            ++capture_count;
        } else if (flow == L"render") {
            add_device_combo_item(g_app.render_device, g_app.render_device_selectors, g_app.render_device_labels, label, id);
            ++render_count;
        }
    }

    SendMessageW(g_app.capture_device, CB_SETCURSEL, 0, 0);
    SendMessageW(g_app.render_device, CB_SETCURSEL, 0, 0);
    return capture_count > 0 || render_count > 0;
}

void refresh_device_lists() {
    const std::wstring current_capture_selector = selected_device_selector(g_app.capture_device, g_app.capture_device_selectors);
    const std::wstring current_render_selector = selected_device_selector(g_app.render_device, g_app.render_device_selectors);
    reset_device_combos();

    std::wstring output;
    DWORD exit_code = 0;
    if (!run_core_command_capture({L"--list-devices"}, output, exit_code)) {
        append_log(std::wstring(text(TextId::device_refresh_failed)) + output + L"\r\n");
        return;
    }

    int capture_count = 0;
    int render_count = 0;
    const bool parsed = parse_device_list_output(output, capture_count, render_count);
    if (!parsed || exit_code != 0) {
        append_log(text(TextId::device_refresh_unusable));
        if (!output.empty()) {
            append_log(output + L"\r\n");
        }
        return;
    }

    select_combo_by_selector(
        g_app.capture_device,
        g_app.capture_device_selectors,
        current_capture_selector.empty() ? g_app.saved_capture_device_selector : current_capture_selector);
    select_combo_by_selector(
        g_app.render_device,
        g_app.render_device_selectors,
        current_render_selector.empty() ? g_app.saved_render_device_selector : current_render_selector);

    std::wstringstream message;
    message << text(TextId::loaded_devices) << L"capture=" << capture_count
            << L", render=" << render_count << L"\r\n";
    append_log(message.str());
    rebuild_menu_bar();
}

bool process_is_running() {
    return g_app.core_process.running();
}

void update_button_state() {
    const BOOL talk_enabled =
        !g_app.stop_requested &&
        process_is_running() &&
        g_app.core_mode == CoreMode::duplex &&
        !g_app.contacts.empty();
    if (g_app.push_to_talk_button) {
        EnableWindow(g_app.push_to_talk_button, talk_enabled);
    }
    if (g_app.continuous_talk_checkbox) {
        EnableWindow(g_app.continuous_talk_checkbox, talk_enabled);
    }
    if (!talk_enabled && g_app.push_to_talk_down) {
        reset_push_to_talk_state();
    }
    update_push_to_talk_button_text();
}

void set_running_state(bool) {
    update_button_state();
}

void set_stopping_state() {
    if (g_app.push_to_talk_button) {
        EnableWindow(g_app.push_to_talk_button, FALSE);
    }
    if (g_app.continuous_talk_checkbox) {
        EnableWindow(g_app.continuous_talk_checkbox, FALSE);
    }
    reset_contact_push_to_talk_state();
    reset_push_to_talk_state();
}

void apply_telemetry_snapshot(const lanspeak::gui::TelemetrySnapshot& snapshot) {
    if (snapshot.capture.valid) {
        g_app.capture_diagnostics = snapshot.capture;
    }
    if (snapshot.render.valid) {
        g_app.render_diagnostics = snapshot.render;
    }
    if (snapshot.local.valid) {
        update_local_meter(
            snapshot.local.level_db,
            snapshot.local.voice_active,
            true);
    }
    const std::size_t count = std::min(g_app.contacts.size(), snapshot.peers.size());
    for (std::size_t index = 0; index < count; ++index) {
        const lanspeak::gui::MeterTelemetry& meter = snapshot.peers[index];
        if (meter.valid) {
            update_contact_meter(
                index,
                meter.level_db,
                meter.voice_active,
                meter.stream_active,
                true);
        }
    }
    update_osd_overlay();
}

void start_core(CoreMode mode) {
    if (process_is_running()) {
        append_log(text(TextId::already_running));
        return;
    }
    if (g_app.contacts.empty()) {
        append_log(text(TextId::add_contact_before_start));
        update_button_state();
        return;
    }

    if (resolve_saved_network_binding()) {
        PostMessageW(g_app.main_window, WM_APP_NETWORK_FALLBACK, 0, 0);
    }

    const std::wstring exe_path = find_core_exe();
    if (!file_exists(exe_path)) {
        append_log(std::wstring(text(TextId::core_missing)) + exe_path + L"\r\n");
        return;
    }

    append_log(std::wstring(text(TextId::starting)) + text(mode == CoreMode::listener
                                                           ? TextId::listener_mode
                                                           : TextId::duplex_mode) +
               L"...\r\n");
    std::wstring command_line;
    DWORD error = ERROR_SUCCESS;
    const bool started = g_app.core_process.start(
        module_directory(),
        [&](HANDLE telemetry_write, HANDLE control_read) {
            command_line = build_command_line(exe_path, mode, telemetry_write, control_read);
            return command_line;
        },
        g_app.main_window,
        WM_APP_LOG,
        WM_APP_CORE_TELEMETRY,
        WM_APP_CORE_EXIT,
        error);
    if (!command_line.empty()) {
        append_log(L"> " + command_line + L"\r\n\r\n");
    }
    if (!started) {
        std::wstringstream message;
        message << text(TextId::create_process_failed) << error << L"\r\n";
        append_log(message.str());
        return;
    }

    reset_contact_meter_state();
    reset_contact_push_to_talk_state();
    reset_push_to_talk_state();
    g_app.core_mode = mode;
    set_running_state(true);
    restore_latched_talk_state_after_restart();
}

void cleanup_finished_process(std::uint64_t generation);

void stop_core(bool restart_after_stop = false, CoreMode restart_mode = CoreMode::duplex) {
    if (!process_is_running()) {
        if (restart_after_stop) {
            start_core(restart_mode);
        }
        return;
    }

    g_app.restart_after_stop = g_app.restart_after_stop || restart_after_stop;
    g_app.restart_mode = restart_mode;
    if (g_app.stop_requested) {
        return;
    }

    g_app.stop_requested = true;
    set_stopping_state();
    append_log(restart_after_stop ? text(TextId::restarting) : text(TextId::stopping));
    g_app.core_process.request_stop();
}

void cleanup_finished_process(std::uint64_t generation) {
    if (!g_app.core_process.finalize(generation)) {
        return;
    }
    reset_contact_push_to_talk_state();
    reset_contact_meter_state();
    const bool restart = g_app.restart_after_stop;
    const CoreMode restart_mode = g_app.restart_mode;
    g_app.restart_after_stop = false;
    g_app.restart_mode = CoreMode::duplex;
    g_app.stop_requested = false;
    reset_contact_push_to_talk_state();
    reset_push_to_talk_state();
    set_running_state(false);
    append_log(text(TextId::stopped));
    if (restart) {
        start_core(restart_mode);
    }
}

void restart_core_after_settings_change() {
    if (g_app.contacts.empty()) {
        if (process_is_running()) {
            append_log(text(TextId::no_contacts_left));
            stop_core(false);
        }
        update_button_state();
        return;
    }

    if (!process_is_running()) {
        start_core(CoreMode::duplex);
        return;
    }

    if (!g_app.restore_latched_talk_after_restart) {
        sync_contact_meter_state();
        g_app.restore_latched_talk_after_restart = true;
        g_app.restart_continuous_talk = g_app.continuous_talk;
        g_app.restart_contact_ptt_latched = g_app.contact_ptt_latched;
    }
    stop_core(true, CoreMode::duplex);
}

void select_menu_device(HWND combo, const std::vector<std::wstring>& selectors, size_t index) {
    if (!combo || index >= selectors.size()) {
        return;
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
    save_settings();
    rebuild_menu_bar();
    restart_core_after_settings_change();
}

void select_menu_language(LanguageSetting setting) {
    g_app.language_setting = setting;
    update_default_device_labels();
    apply_language_to_main_window();
    save_settings();
}

void ensure_core_running() {
    if (!process_is_running() && !g_app.contacts.empty()) {
        start_core(CoreMode::duplex);
    } else {
        update_button_state();
    }
}

bool write_core_control_line(const std::string& line) {
    return g_app.core_process.write_control(line);
}

bool send_contact_runtime_settings(size_t index, const Contact& contact) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "peer_settings\t"
           << index << "\t"
           << (contact.muted ? 0.0 : contact.gain) << "\t"
           << (contact.self_duck ? contact.duck_db : 0.0) << "\t"
           << contact.duck_threshold << "\t"
           << contact.duck_attack_ms << "\t"
           << contact.duck_hold_ms << "\t"
           << contact.duck_release_ms << "\t"
           << (contact.global_ptt_enabled ? 1 : 0) << "\n";
    return write_core_control_line(stream.str());
}

bool send_contact_push_to_talk_state(size_t index, bool active) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "peer_talk\t" << index << "\t" << (active ? 1 : 0) << "\n";
    return write_core_control_line(stream.str());
}

bool set_contact_push_to_talk_active(size_t index, bool active) {
    sync_contact_meter_state();
    if (index >= g_app.contacts.size()) {
        return false;
    }

    if (active) {
        if (!process_is_running() || g_app.core_mode != CoreMode::duplex ||
            (g_app.push_to_talk_down && !g_app.input_muted)) {
            return false;
        }
        const bool was_active = contact_push_to_talk_active(index);
        ++g_app.contact_ptt_refs[index];
        if (!was_active && !send_contact_push_to_talk_state(index, true)) {
            --g_app.contact_ptt_refs[index];
            repaint_contact_talk_state_now();
            update_button_state();
            update_osd_overlay();
            return false;
        }
        repaint_contact_talk_state_now();
        update_button_state();
        update_osd_overlay();
        return true;
    }

    if (g_app.contact_ptt_refs[index] == 0) {
        return false;
    }

    const bool was_active = contact_push_to_talk_active(index);
    --g_app.contact_ptt_refs[index];
    if (was_active && !contact_push_to_talk_active(index)) {
        send_contact_push_to_talk_state(index, false);
    }
    repaint_contact_talk_state_now();
    update_button_state();
    update_osd_overlay();
    return true;
}

bool set_contact_push_to_talk_latched(size_t index, bool active) {
    sync_contact_meter_state();
    if (index >= g_app.contacts.size() ||
        index >= g_app.contact_ptt_latched.size() ||
        g_app.contact_ptt_latched[index] == active) {
        return false;
    }
    if (active && (!process_is_running() || g_app.core_mode != CoreMode::duplex)) {
        return false;
    }

    const bool was_active = contact_push_to_talk_active(index);
    g_app.contact_ptt_latched[index] = active;
    const bool is_active = contact_push_to_talk_active(index);
    if (was_active != is_active && !send_contact_push_to_talk_state(index, is_active)) {
        g_app.contact_ptt_latched[index] = !active;
        repaint_contact_talk_state_now();
        update_button_state();
        update_osd_overlay();
        return false;
    }

    repaint_contact_talk_state_now();
    update_button_state();
    update_osd_overlay();
    return true;
}

void reset_contact_push_to_talk_state() {
    g_app.mouse_contact_ptt_index = -1;
    g_app.contact_hotkey_down = false;
    g_app.contact_hotkey_indices.clear();
    g_app.contact_hotkey_pressed = {};
    sync_contact_meter_state();
    for (size_t index = 0; index < g_app.contact_ptt_refs.size(); ++index) {
        const bool was_active = contact_push_to_talk_active(index);
        g_app.contact_ptt_refs[index] = 0;
        g_app.contact_ptt_latched[index] = false;
        if (was_active) {
            send_contact_push_to_talk_state(index, false);
        }
    }
    repaint_contact_talk_state_now();
    update_button_state();
    update_osd_overlay();
}

void update_push_to_talk_button_text() {
    if (g_app.push_to_talk_button) {
        SetWindowTextW(g_app.push_to_talk_button, text(g_app.push_to_talk_down ? TextId::talking : TextId::talk));
        SendMessageW(
            g_app.push_to_talk_button,
            BM_SETSTATE,
            g_app.continuous_talk ? TRUE : FALSE,
            0);
        InvalidateRect(g_app.push_to_talk_button, nullptr, FALSE);
    }
    if (g_app.continuous_talk_checkbox) {
        SendMessageW(
            g_app.continuous_talk_checkbox,
            BM_SETCHECK,
            g_app.continuous_talk ? BST_CHECKED : BST_UNCHECKED,
            0);
    }
}

bool send_input_muted_state(bool muted) {
    g_app.input_muted = muted;
    const bool sent = write_core_control_line(std::string("input_muted\t") + (muted ? "1\n" : "0\n"));
    if (!sent && !muted) {
        g_app.input_muted = true;
        g_app.push_to_talk_down = false;
        g_app.button_ptt_down = false;
        g_app.hotkey_ptt_active = false;
        g_app.continuous_talk = false;
        update_push_to_talk_button_text();
    }
    return sent;
}

void reset_push_to_talk_state() {
    g_app.push_to_talk_down = false;
    g_app.button_ptt_down = false;
    g_app.hotkey_ptt_active = false;
    g_app.continuous_talk = false;
    g_app.input_muted = true;
    update_push_to_talk_button_text();
    repaint_contact_talk_state_now();
    update_osd_overlay();
}

void restore_latched_talk_state_after_restart() {
    if (!g_app.restore_latched_talk_after_restart) {
        return;
    }

    const bool continuous_talk = g_app.restart_continuous_talk;
    std::vector<bool> contact_latches = std::move(g_app.restart_contact_ptt_latched);
    g_app.restore_latched_talk_after_restart = false;
    g_app.restart_continuous_talk = false;
    g_app.restart_contact_ptt_latched.clear();

    if (continuous_talk) {
        set_continuous_talk_active(true);
    }
    const size_t count = std::min(g_app.contacts.size(), contact_latches.size());
    for (size_t index = 0; index < count; ++index) {
        if (contact_latches[index]) {
            set_contact_push_to_talk_latched(index, true);
        }
    }
}

bool can_start_global_talk() {
    return process_is_running() &&
        g_app.core_mode == CoreMode::duplex &&
        !g_app.contacts.empty();
}

void apply_global_talk_sources() {
    const bool active =
        g_app.button_ptt_down ||
        g_app.hotkey_ptt_active ||
        g_app.continuous_talk;
    if (active && !can_start_global_talk()) {
        return;
    }

    g_app.capture_diagnostics = {};
    g_app.render_diagnostics = {};

    const bool target_muted = !active;
    if (g_app.push_to_talk_down == active && g_app.input_muted == target_muted) {
        update_push_to_talk_button_text();
        return;
    }

    g_app.push_to_talk_down = active;
    update_push_to_talk_button_text();
    send_input_muted_state(target_muted);
    repaint_contact_talk_state_now();
    update_osd_overlay();
}

void set_button_push_to_talk_active(bool active) {
    if (active && !can_start_global_talk()) {
        return;
    }
    g_app.button_ptt_down = active;
    apply_global_talk_sources();
}

void set_hotkey_push_to_talk_active(bool active) {
    if (active && !can_start_global_talk()) {
        return;
    }
    g_app.hotkey_ptt_active = active;
    apply_global_talk_sources();
}

void set_continuous_talk_active(bool active) {
    if (active && !can_start_global_talk()) {
        update_push_to_talk_button_text();
        return;
    }
    g_app.continuous_talk = active;
    apply_global_talk_sources();
}

LRESULT CALLBACK push_to_talk_button_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (IsWindowEnabled(window)) {
            set_button_push_to_talk_active(true);
            SetCapture(window);
        }
        break;
    case WM_LBUTTONUP:
        set_button_push_to_talk_active(false);
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        break;
    case WM_CAPTURECHANGED:
        if (g_app.button_ptt_down && reinterpret_cast<HWND>(lparam) != window) {
            set_button_push_to_talk_active(false);
        }
        break;
    case WM_KEYDOWN:
        if (wparam == VK_SPACE || wparam == VK_RETURN) {
            set_button_push_to_talk_active(true);
        }
        break;
    case WM_KEYUP:
        if (wparam == VK_SPACE || wparam == VK_RETURN) {
            set_button_push_to_talk_active(false);
        }
        break;
    default:
        break;
    }

    const LRESULT result = g_app.push_to_talk_old_proc
        ? CallWindowProcW(g_app.push_to_talk_old_proc, window, message, wparam, lparam)
        : DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_LBUTTONUP || message == WM_KEYDOWN ||
        message == WM_KEYUP || message == WM_CAPTURECHANGED) {
        update_push_to_talk_button_text();
    }
    return result;
}

bool contact_requires_core_restart(const Contact& before, const Contact& after) {
    return before.host != after.host ||
        before.port != after.port ||
        before.receive_buffer_ms != after.receive_buffer_ms;
}

double contact_gain_from_slider_point(HWND panel, int index, POINT point) {
    const RECT row_rect = contact_row_rect_from_index(panel, index);
    const RECT card_rect = contact_card_rect_from_row(row_rect);
    const RECT slider_rect = contact_gain_slider_rect_from_card(card_rect);
    const int track_left = slider_rect.left + 4;
    const int track_right = slider_rect.right - 4;
    if (track_right <= track_left) {
        return g_app.contacts[static_cast<size_t>(index)].gain;
    }

    const double ratio = std::clamp(
        static_cast<double>(point.x - track_left) / static_cast<double>(track_right - track_left),
        0.0,
        1.0);
    const double value = kContactGainMin + ratio * (kContactGainMax - kContactGainMin);
    return std::round(value / kContactGainStep) * kContactGainStep;
}

void set_contact_gain(size_t index, double value, bool persist) {
    if (index >= g_app.contacts.size()) {
        return;
    }

    Contact& contact = g_app.contacts[index];
    const double new_gain = std::round(
        std::clamp(value, kContactGainMin, kContactGainMax) / kContactGainStep) *
        kContactGainStep;
    if (std::abs(contact.gain - new_gain) < 0.0001) {
        if (persist) {
            save_settings();
        }
        return;
    }

    contact.gain = new_gain;
    invalidate_contact_row(index);
    if (persist) {
        save_settings();
    }
    if (!send_contact_runtime_settings(index, contact)) {
        restart_core_after_settings_change();
    }
}

void adjust_selected_contact_gain(double delta) {
    const int index = selected_contact_index();
    if (index < 0 || static_cast<size_t>(index) >= g_app.contacts.size()) {
        return;
    }

    const double current = g_app.contacts[static_cast<size_t>(index)].gain;
    set_contact_gain(static_cast<size_t>(index), current + delta, true);
}

void toggle_selected_contact_mute() {
    const int index = selected_contact_index();
    if (index < 0 || static_cast<size_t>(index) >= g_app.contacts.size()) {
        return;
    }

    Contact& contact = g_app.contacts[static_cast<size_t>(index)];
    contact.muted = !contact.muted;
    invalidate_contact_row(static_cast<size_t>(index));
    save_settings();
    if (!send_contact_runtime_settings(static_cast<size_t>(index), contact)) {
        restart_core_after_settings_change();
    }
}

void toggle_selected_contact_global_ptt() {
    const int index = selected_contact_index();
    if (index < 0 || static_cast<size_t>(index) >= g_app.contacts.size()) {
        return;
    }

    Contact& contact = g_app.contacts[static_cast<size_t>(index)];
    contact.global_ptt_enabled = !contact.global_ptt_enabled;
    invalidate_contact_row(static_cast<size_t>(index));
    save_settings();
    if (!send_contact_runtime_settings(static_cast<size_t>(index), contact)) {
        restart_core_after_settings_change();
    }
}

void add_contact_from_editor() {
    Contact initial;
    Contact contact;
    if (lanspeak::gui::show_contact_dialog(
            g_app.instance,
            g_app.main_window,
            load_app_large_icon(),
            load_app_small_icon(),
            g_app.language_setting,
            initial,
            false,
            contact)) {
        g_app.contacts.push_back(contact);
        g_app.contact_meters.append();
        g_app.contact_ptt_refs.push_back(0);
        g_app.contact_ptt_latched.push_back(false);
        refresh_contact_list(static_cast<int>(g_app.contacts.size()) - 1);
        save_settings();
        restart_core_after_settings_change();
    }
}

void update_selected_contact_from_editor() {
    const int index = selected_contact_index();
    if (index < 0 || static_cast<size_t>(index) >= g_app.contacts.size()) {
        return;
    }
    const Contact previous = g_app.contacts[static_cast<size_t>(index)];
    Contact contact;
    if (lanspeak::gui::show_contact_dialog(
            g_app.instance,
            g_app.main_window,
            load_app_large_icon(),
            load_app_small_icon(),
            g_app.language_setting,
            previous,
            true,
            contact)) {
        g_app.contacts[static_cast<size_t>(index)] = contact;
        refresh_contact_list(index);
        save_settings();
        if (contact_requires_core_restart(previous, contact) ||
            !send_contact_runtime_settings(static_cast<size_t>(index), contact)) {
            restart_core_after_settings_change();
        }
    }
}

void remove_selected_contact() {
    const int index = selected_contact_index();
    if (index < 0 || static_cast<size_t>(index) >= g_app.contacts.size()) {
        return;
    }
    reset_contact_push_to_talk_state();
    g_app.contacts.erase(g_app.contacts.begin() + index);
    g_app.contact_meters.erase(static_cast<size_t>(index));
    if (static_cast<size_t>(index) < g_app.contact_ptt_refs.size()) {
        g_app.contact_ptt_refs.erase(g_app.contact_ptt_refs.begin() + index);
    }
    if (static_cast<size_t>(index) < g_app.contact_ptt_latched.size()) {
        g_app.contact_ptt_latched.erase(g_app.contact_ptt_latched.begin() + index);
    }
    update_global_hotkey_hook();
    refresh_contact_list(std::min(index, static_cast<int>(g_app.contacts.size()) - 1));
    save_settings();
    restart_core_after_settings_change();
}

void create_controls(HWND window) {
    g_app.capture_device = add_hidden_combo(window, IDC_CAPTURE_DEVICE);
    g_app.render_device = add_hidden_combo(window, IDC_RENDER_DEVICE);
    g_app.contact_name_font = create_ui_font(window, 13, FW_SEMIBOLD);

    g_app.push_to_talk_button = add_button(window, IDC_PUSH_TO_TALK, L"", 12, 12, 118, 28, BS_OWNERDRAW);
    g_app.push_to_talk_old_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_app.push_to_talk_button, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(push_to_talk_button_proc)));
    EnableWindow(g_app.push_to_talk_button, FALSE);
    g_app.continuous_talk_checkbox = add_button(
        window,
        IDC_CONTINUOUS_TALK,
        text(TextId::continuous_talk),
        148,
        12,
        190,
        28,
        BS_AUTOCHECKBOX);
    EnableWindow(g_app.continuous_talk_checkbox, FALSE);

    if (register_contact_panel_class()) {
        g_app.contact_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LanSpeakContactPanel", L"",
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
                                         kWindowMargin, kContactListTop, 760, 150, window,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CONTACT_LIST)),
                                         g_app.instance, nullptr);
    }
    create_contact_tooltip(g_app.contact_list);

    if (register_local_meter_class()) {
        g_app.local_meter = CreateWindowExW(
            0,
            L"LanSpeakLocalMeter",
            L"",
            WS_CHILD | WS_VISIBLE,
            kWindowMargin,
            210,
            760,
            kLocalMeterHeight,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOCAL_METER)),
            g_app.instance,
            nullptr);
    }

    g_app.log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                ES_AUTOVSCROLL | ES_LEFT,
                            kWindowMargin, 214, 760, log_height_for_visible_lines(), window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOG)),
                            g_app.instance, nullptr);
    set_font(g_app.log);
    SendMessageW(g_app.log, EM_SETLIMITTEXT, static_cast<WPARAM>(kLogHardLimitChars), 0);
    apply_language_to_main_window(false);
}

void layout_controls(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int content_width = std::max(100, width - kWindowMargin * 2);
    const int content_bottom = height - kWindowMargin;
    const int log_height = g_app.debug_console_visible ? log_height_for_visible_lines() : 0;
    const int log_top = g_app.debug_console_visible
                            ? std::max(
                                  kContactListTop + kMinimumContactListHeight + kLocalMeterGap +
                                      kLocalMeterHeight + kLogGap,
                                  content_bottom - log_height)
                            : content_bottom;
    const int meter_bottom = g_app.debug_console_visible ? log_top - kLogGap : content_bottom;
    const int meter_top = meter_bottom - kLocalMeterHeight;
    const int contact_height = std::max(
        kMinimumContactListHeight,
        meter_top - kLocalMeterGap - kContactListTop);
    if (g_app.contact_list) {
        MoveWindow(g_app.contact_list, kWindowMargin, kContactListTop, content_width, contact_height, TRUE);
    }
    if (g_app.local_meter) {
        MoveWindow(
            g_app.local_meter,
            kWindowMargin,
            meter_top,
            content_width,
            kLocalMeterHeight,
            TRUE);
    }
    if (g_app.log) {
        if (g_app.debug_console_visible) {
            MoveWindow(g_app.log, kWindowMargin, log_top, content_width, log_height, TRUE);
            ShowWindow(g_app.log, SW_SHOW);
        } else {
            ShowWindow(g_app.log, SW_HIDE);
        }
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (g_app.taskbar_created_message != 0 &&
        message == g_app.taskbar_created_message &&
        g_app.tray_icon.added()) {
        g_app.tray_icon.invalidate_after_taskbar_restart();
        add_tray_icon(window);
        return 0;
    }

    switch (message) {
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        if (limits) {
            limits->ptMinTrackSize.x = kMinimumWindowWidth;
        }
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }
    case WM_CREATE:
        g_app.main_window = window;
        create_controls(window);
        load_settings();
        refresh_device_lists();
        if (resolve_saved_network_binding()) {
            PostMessageW(window, WM_APP_NETWORK_FALLBACK, 0, 0);
        }
        add_tray_icon(window);
        start_single_instance_waiter();
        ensure_core_running();
        return 0;
    case WM_SIZE:
        layout_controls(window);
        return 0;
    case WM_EXITSIZEMOVE:
        save_settings();
        return 0;
    case WM_CLOSE:
        if (!g_app.exit_requested) {
            hide_main_window_to_tray(window);
            return 0;
        }
        break;
    case WM_TIMER:
        if (wparam == IDT_OSD_HOLD) {
            KillTimer(window, IDT_OSD_HOLD);
            update_osd_overlay();
            return 0;
        }
        break;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (item && item->CtlID == IDC_PUSH_TO_TALK) {
            draw_global_ptt_button(*item);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        const UINT command_id = LOWORD(wparam);
        if (command_id >= IDC_MENU_CAPTURE_BASE &&
            command_id < IDC_MENU_CAPTURE_BASE + kMaxMenuDevices) {
            select_menu_device(
                g_app.capture_device,
                g_app.capture_device_selectors,
                static_cast<size_t>(command_id - IDC_MENU_CAPTURE_BASE));
            return 0;
        }
        if (command_id >= IDC_MENU_RENDER_BASE &&
            command_id < IDC_MENU_RENDER_BASE + kMaxMenuDevices) {
            select_menu_device(
                g_app.render_device,
                g_app.render_device_selectors,
                static_cast<size_t>(command_id - IDC_MENU_RENDER_BASE));
            return 0;
        }
        if (command_id == IDC_MENU_LANGUAGE_AUTO) {
            select_menu_language(LanguageSetting::automatic);
            return 0;
        }
        if (command_id == IDC_MENU_LANGUAGE_ENGLISH) {
            select_menu_language(LanguageSetting::english);
            return 0;
        }
        if (command_id == IDC_MENU_LANGUAGE_RUSSIAN) {
            select_menu_language(LanguageSetting::russian);
            return 0;
        }
        if (command_id == IDC_MENU_GLOBAL_HOTKEYS) {
            show_global_hotkeys_dialog(window);
            return 0;
        }
        if (command_id == IDC_MENU_AUDIO_LATENCY) {
            lanspeak::gui::show_audio_latency_dialog(
                g_app.instance,
                window,
                load_app_large_icon(),
                load_app_small_icon(),
                g_app.language_setting,
                g_app.capture_diagnostics,
                g_app.render_diagnostics);
            return 0;
        }
        if (command_id == IDC_MENU_NETWORK_SETTINGS) {
            const std::vector<lanspeak::gui::NetworkAdapterInfo> adapters =
                lanspeak::gui::enumerate_active_ipv4_adapters();
            const NetworkSettingsValue current{g_app.local_port, g_app.network_adapter_id};
            NetworkSettingsValue result;
            if (lanspeak::gui::show_network_settings_dialog(
                    g_app.instance,
                    window,
                    load_app_large_icon(),
                    load_app_small_icon(),
                    g_app.language_setting,
                    adapters,
                    current,
                    result)) {
                g_app.local_port = result.local_port;
                g_app.network_adapter_id = std::move(result.adapter_id);
                const std::optional<std::wstring> bind_address =
                    lanspeak::gui::resolve_network_bind_address(adapters, g_app.network_adapter_id);
                g_app.bind_address = bind_address.value_or(L"0.0.0.0");
                save_settings();
                restart_core_after_settings_change();
            }
            return 0;
        }
        if (command_id == IDC_MENU_DEBUG_CONSOLE) {
            g_app.debug_console_visible = !g_app.debug_console_visible;
            save_settings();
            rebuild_menu_bar();
            layout_controls(window);
            return 0;
        }
        if (command_id == IDC_MENU_ABOUT) {
            lanspeak::gui::show_about_dialog(
                g_app.instance,
                window,
                load_app_large_icon(),
                load_app_small_icon(),
                g_app.language_setting);
            return 0;
        }
        if (command_id == IDC_TRAY_EXIT) {
            request_application_exit(window);
            return 0;
        }

        switch (command_id) {
        case IDC_CAPTURE_DEVICE:
        case IDC_RENDER_DEVICE:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                save_settings();
                restart_core_after_settings_change();
                return 0;
            }
            break;
        case IDC_CONTINUOUS_TALK:
            if (HIWORD(wparam) == BN_CLICKED) {
                const bool checked =
                    SendMessageW(g_app.continuous_talk_checkbox, BM_GETCHECK, 0, 0) ==
                    BST_CHECKED;
                set_continuous_talk_active(checked);
                return 0;
            }
            break;
        case IDC_ADD_CONTACT:
            add_contact_from_editor();
            return 0;
        case IDC_UPDATE_CONTACT:
            update_selected_contact_from_editor();
            return 0;
        case IDC_TOGGLE_MUTE:
            toggle_selected_contact_mute();
            return 0;
        case IDC_TOGGLE_GLOBAL_PTT:
            toggle_selected_contact_global_ptt();
            return 0;
        default:
            break;
        }
        break;
    }
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wparam) == g_app.contact_list) {
            POINT screen_point{};
            bool clicked_contact = selected_contact_index() >= 0;
            if (lparam == -1) {
                RECT rect{};
                GetWindowRect(g_app.contact_list, &rect);
                screen_point.x = rect.left + 24;
                screen_point.y = rect.top + 24;
                clicked_contact = selected_contact_index() >= 0;
            } else {
                screen_point.x = static_cast<short>(LOWORD(lparam));
                screen_point.y = static_cast<short>(HIWORD(lparam));

                POINT list_point = screen_point;
                ScreenToClient(g_app.contact_list, &list_point);
                const int index = contact_index_from_point(g_app.contact_list, list_point);
                if (index >= 0) {
                    set_selected_contact_index(index, false);
                    clicked_contact = true;
                } else {
                    set_selected_contact_index(-1, false);
                    clicked_contact = false;
                }
            }

            HMENU menu = CreatePopupMenu();
            const int context_contact_index = selected_contact_index();
            const bool continuous_talk = clicked_contact && context_contact_index >= 0 &&
                contact_push_to_talk_latched(static_cast<size_t>(context_contact_index));
            AppendMenuW(
                menu,
                MF_STRING |
                    (clicked_contact ? MF_ENABLED : MF_GRAYED) |
                    (continuous_talk ? MF_CHECKED : MF_UNCHECKED),
                IDC_CONTEXT_CONTINUOUS_TALK,
                text(TextId::continuous_talk));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDC_ADD_CONTACT, text(TextId::add));
            AppendMenuW(menu, MF_STRING | (clicked_contact ? MF_ENABLED : MF_GRAYED),
                        IDC_CONTEXT_EDIT, text(TextId::edit));
            AppendMenuW(menu, MF_STRING | (clicked_contact ? MF_ENABLED : MF_GRAYED),
                        IDC_CONTEXT_DELETE, text(TextId::delete_contact));
            if (clicked_contact) {
                SetMenuDefaultItem(menu, IDC_CONTEXT_EDIT, FALSE);
            }
            const UINT command = TrackPopupMenu(
                menu,
                TPM_RETURNCMD | TPM_RIGHTBUTTON,
                screen_point.x,
                screen_point.y,
                0,
                window,
                nullptr);
            DestroyMenu(menu);

            if (command == IDC_CONTEXT_CONTINUOUS_TALK &&
                clicked_contact &&
                context_contact_index >= 0) {
                set_contact_push_to_talk_latched(
                    static_cast<size_t>(context_contact_index),
                    !continuous_talk);
            } else if (command == IDC_ADD_CONTACT) {
                add_contact_from_editor();
            } else if (command == IDC_CONTEXT_EDIT && clicked_contact) {
                update_selected_contact_from_editor();
            } else if (command == IDC_CONTEXT_DELETE && clicked_contact) {
                remove_selected_contact();
            }
            return 0;
        }
        break;
    case WM_APP_LOG: {
        std::unique_ptr<lanspeak::gui::CoreOutputChunk> chunk(
            reinterpret_cast<lanspeak::gui::CoreOutputChunk*>(lparam));
        if (chunk && chunk->generation == g_app.core_process.generation() && !chunk->bytes.empty()) {
            append_log(decode_process_output(
                chunk->bytes.data(), static_cast<int>(chunk->bytes.size())));
        }
        return 0;
    }
    case WM_APP_CORE_TELEMETRY: {
        lanspeak::gui::TelemetrySnapshot snapshot;
        if (g_app.core_process.take_latest_telemetry(
                static_cast<std::uint64_t>(wparam), snapshot)) {
            apply_telemetry_snapshot(snapshot);
        }
        return 0;
    }
    case WM_APP_CORE_EXIT: {
        std::unique_ptr<lanspeak::gui::CoreExitNotice> notice(
            reinterpret_cast<lanspeak::gui::CoreExitNotice*>(lparam));
        if (notice) {
            cleanup_finished_process(notice->generation);
        }
        return 0;
    }
    case WM_APP_SHOW_EXISTING:
        show_main_window_from_tray(window);
        return 0;
    case WM_APP_NETWORK_FALLBACK:
        MessageBoxW(
            window,
            text(TextId::network_adapter_missing),
            L"LAN Speak",
            MB_OK | MB_ICONWARNING);
        return 0;
    case WM_APP_TRAY:
        if (wparam == ID_TRAY_ICON) {
            const auto tray_message = static_cast<UINT>(lparam);
            if (tray_message == WM_LBUTTONUP) {
                toggle_main_window_from_tray(window);
                return 0;
            }
            if (tray_message == WM_RBUTTONUP || tray_message == WM_CONTEXTMENU) {
                show_tray_context_menu(window);
                return 0;
            }
        }
        return 0;
    case WM_ACTIVATEAPP:
        update_osd_overlay();
        return 0;
    case WM_APP_GLOBAL_PTT:
        set_hotkey_push_to_talk_active(wparam != FALSE);
        return 0;
    case WM_APP_CONTACT_PTT:
        set_contact_push_to_talk_active(static_cast<size_t>(wparam), lparam != FALSE);
        return 0;
    case WM_DESTROY:
        save_settings();
        remove_global_hotkey_hook();
        reset_push_to_talk_state();
        remove_tray_icon(window);
        KillTimer(window, IDT_OSD_HOLD);
        g_app.core_process.shutdown_and_wait();
        if (g_app.contact_name_font) {
            DeleteObject(g_app.contact_name_font);
            g_app.contact_name_font = nullptr;
        }
        g_app.contact_surface.reset();
        g_app.local_meter_surface.reset();
        g_app.osd_overlay.reset();
        g_app.gdi_objects.reset();
        if (g_app.contact_tooltip) {
            DestroyWindow(g_app.contact_tooltip);
            g_app.contact_tooltip = nullptr;
        }
        release_single_instance();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int lanspeak::gui::run_application(HINSTANCE instance, int show_command) {
    g_app.instance = instance;
    INITCOMMONCONTROLSEX common_controls{};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&common_controls);
    g_app.taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    if (!initialize_single_instance()) {
        return 0;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hIcon = load_app_large_icon();
    window_class.hIconSm = load_app_small_icon();
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = L"LanSpeakGuiWindow";

    if (!RegisterClassExW(&window_class)) {
        release_single_instance();
        return 1;
    }

    g_app.main_window = CreateWindowExW(0, window_class.lpszClassName, L"LanSpeak",
                                   WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                   kInitialWindowWidth, kInitialWindowHeight,
                                   nullptr, nullptr, instance, nullptr);
    if (!g_app.main_window) {
        release_single_instance();
        return 1;
    }
    apply_window_icons(g_app.main_window);
    apply_language_to_main_window(false);

    ShowWindow(g_app.main_window, show_command);
    UpdateWindow(g_app.main_window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
