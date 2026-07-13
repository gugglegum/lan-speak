#include "gui/contact_dialog.h"

#include "gui/localization.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

namespace lanspeak::gui {
namespace {

constexpr int kNameId = 1025;
constexpr int kHostId = 1001;
constexpr int kPortId = 1003;
constexpr int kGainId = 1008;
constexpr int kSelfDuckId = 1010;
constexpr int kDuckDbId = 1011;
constexpr int kDuckAttackId = 1012;
constexpr int kDuckHoldId = 1013;
constexpr int kDuckReleaseId = 1014;
constexpr int kDuckThresholdId = 1015;
constexpr double kGainMin = 0.0;
constexpr double kGainMax = 3.0;
constexpr double kGainStep = 0.05;
constexpr wchar_t kClassName[] = L"LanSpeakContactDialog";

struct ContactDialogState {
    HINSTANCE instance = nullptr;
    LanguageSetting language = LanguageSetting::automatic;
    bool edit = false;
    bool accepted = false;
    bool owner_restored = false;
    Contact contact;
    HWND owner = nullptr;
    HWND name = nullptr;
    HWND host = nullptr;
    HWND port = nullptr;
    HWND gain = nullptr;
    HWND self_duck = nullptr;
    HWND duck_db = nullptr;
    HWND duck_threshold = nullptr;
    HWND duck_attack = nullptr;
    HWND duck_hold = nullptr;
    HWND duck_release = nullptr;
};

const wchar_t* text(const ContactDialogState& state, TextId id) {
    return localized_text(id, state.language);
}

void set_font(HWND window) {
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

HWND add_label(HWND parent, const wchar_t* value, int x, int y, int width, int height) {
    HWND window = CreateWindowExW(
        0, L"STATIC", value, WS_CHILD | WS_VISIBLE,
        x, y, width, height, parent, nullptr, nullptr, nullptr);
    set_font(window);
    return window;
}

HWND add_edit(HWND parent, int id, const wchar_t* value, int x, int y, int width, int height) {
    HWND window = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", value,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        x, y, width, height, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    set_font(window);
    return window;
}

HWND add_button(
    HWND parent,
    int id,
    const wchar_t* value,
    int x,
    int y,
    int width,
    int height,
    DWORD style = 0) {
    HWND window = CreateWindowExW(
        0, L"BUTTON", value,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
        x, y, width, height, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    set_font(window);
    return window;
}

std::wstring trim(std::wstring value) {
    const std::size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const std::size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring window_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring result(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(window, result.data(), length + 1);
    }
    result.resize(static_cast<std::size_t>(std::max(0, length)));
    return trim(std::move(result));
}

std::wstring window_text_or(HWND window, const wchar_t* fallback) {
    std::wstring result = window_text(window);
    return result.empty() ? std::wstring(fallback) : result;
}

bool valid_port_text(const std::wstring& value) {
    if (value.empty()) return false;
    int port = 0;
    for (const wchar_t ch : value) {
        if (ch < L'0' || ch > L'9') return false;
        port = port * 10 + static_cast<int>(ch - L'0');
        if (port > 65535) return false;
    }
    return port > 0;
}

std::uint16_t parse_port(const std::wstring& value) {
    return valid_port_text(value)
        ? static_cast<std::uint16_t>(std::wcstoul(value.c_str(), nullptr, 10))
        : 0;
}

double parse_double(
    const std::wstring& value,
    double fallback,
    double minimum,
    double maximum) {
    wchar_t* end = nullptr;
    const double parsed = std::wcstod(value.c_str(), &end);
    return end == value.c_str() || *end != L'\0' || !std::isfinite(parsed)
        ? fallback
        : std::clamp(parsed, minimum, maximum);
}

int parse_int(const std::wstring& value, int fallback, int minimum, int maximum) {
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(value.c_str(), &end, 10);
    return end == value.c_str() || *end != L'\0'
        ? fallback
        : std::clamp(static_cast<int>(parsed), minimum, maximum);
}

std::wstring format_decimal(double value, int precision) {
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(precision) << value;
    std::wstring result = stream.str();
    while (result.size() > 1 && result.back() == L'0') result.pop_back();
    if (!result.empty() && result.back() == L'.') result.pop_back();
    return result;
}

std::wstring format_gain(double value) {
    value = std::clamp(value, kGainMin, kGainMax);
    value = std::round(value / kGainStep) * kGainStep;
    return format_decimal(value, 2);
}

void center_on_owner(HWND window, HWND owner) {
    RECT window_rect{};
    RECT owner_rect{};
    GetWindowRect(window, &window_rect);
    if (owner) GetWindowRect(owner, &owner_rect);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &owner_rect, 0);
    const int width = window_rect.right - window_rect.left;
    const int height = window_rect.bottom - window_rect.top;
    const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
    const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
}

Contact contact_from_dialog(const ContactDialogState& state) {
    Contact contact;
    contact.name = window_text(state.name);
    contact.host = window_text(state.host);
    contact.port = parse_port(window_text(state.port));
    contact.gain = parse_double(window_text_or(state.gain, L"1.0"), 1.0, kGainMin, kGainMax);
    contact.muted = state.contact.muted;
    contact.global_ptt_enabled = state.contact.global_ptt_enabled;
    contact.self_duck = SendMessageW(state.self_duck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    contact.duck_db = parse_double(window_text_or(state.duck_db, L"12"), 12.0, 0.0, 60.0);
    contact.duck_threshold = parse_double(
        window_text_or(state.duck_threshold, L"0.02"), 0.02, 0.0, 1.0);
    contact.duck_attack_ms = parse_int(window_text_or(state.duck_attack, L"8"), 8, 0, 1000);
    contact.duck_hold_ms = parse_int(window_text_or(state.duck_hold, L"80"), 80, 0, 1000);
    contact.duck_release_ms = parse_int(
        window_text_or(state.duck_release, L"120"), 120, 0, 5000);
    contact.ptt_hotkey = state.contact.ptt_hotkey;
    return contact;
}

bool validate(HWND window, const ContactDialogState& state, const Contact& contact) {
    if (contact.name.empty()) {
        MessageBoxW(window, text(state, TextId::name_required), L"LanSpeak", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (contact.host.empty()) {
        MessageBoxW(window, text(state, TextId::host_required), L"LanSpeak", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (contact.port == 0) {
        MessageBoxW(window, text(state, TextId::port_required), L"LanSpeak", MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

void create_controls(HWND window, ContactDialogState& state) {
    add_label(window, text(state, TextId::name), 12, 16, 90, 20);
    state.name = add_edit(window, kNameId, state.contact.name.c_str(), 110, 12, 260, 24);
    add_label(window, text(state, TextId::ip_host), 12, 50, 90, 20);
    state.host = add_edit(window, kHostId, state.contact.host.c_str(), 110, 46, 180, 24);
    add_label(window, text(state, TextId::port), 302, 50, 35, 20);
    state.port = add_edit(
        window, kPortId, std::to_wstring(state.contact.port).c_str(), 340, 46, 70, 24);
    add_label(window, text(state, TextId::contact_gain), 12, 84, 95, 20);
    state.gain = add_edit(window, kGainId, format_gain(state.contact.gain).c_str(), 110, 80, 70, 24);
    state.self_duck = add_button(
        window, kSelfDuckId, text(state, TextId::self_ducking),
        12, 118, 145, 24, BS_AUTOCHECKBOX);
    SendMessageW(
        state.self_duck, BM_SETCHECK,
        state.contact.self_duck ? BST_CHECKED : BST_UNCHECKED, 0);
    add_label(window, L"dB", 165, 122, 25, 20);
    state.duck_db = add_edit(
        window, kDuckDbId, format_decimal(state.contact.duck_db, 2).c_str(), 190, 118, 50, 24);
    add_label(window, text(state, TextId::threshold), 255, 122, 65, 20);
    state.duck_threshold = add_edit(
        window, kDuckThresholdId, format_decimal(state.contact.duck_threshold, 4).c_str(),
        320, 118, 60, 24);
    add_label(window, text(state, TextId::attack_ms), 12, 156, 90, 20);
    state.duck_attack = add_edit(
        window, kDuckAttackId, std::to_wstring(state.contact.duck_attack_ms).c_str(),
        110, 152, 55, 24);
    add_label(window, text(state, TextId::hold_ms), 185, 156, 60, 20);
    state.duck_hold = add_edit(
        window, kDuckHoldId, std::to_wstring(state.contact.duck_hold_ms).c_str(),
        245, 152, 55, 24);
    add_label(window, text(state, TextId::release_ms), 320, 156, 75, 20);
    state.duck_release = add_edit(
        window, kDuckReleaseId, std::to_wstring(state.contact.duck_release_ms).c_str(),
        395, 152, 55, 24);
    add_button(
        window, IDOK, state.edit ? text(state, TextId::save) : text(state, TextId::add),
        244, 210, 100, 28);
    add_button(window, IDCANCEL, text(state, TextId::cancel), 354, 210, 90, 28);
}

void restore_owner(ContactDialogState& state) {
    if (state.owner_restored) return;
    state.owner_restored = true;
    if (state.owner && IsWindow(state.owner)) {
        EnableWindow(state.owner, TRUE);
        SetActiveWindow(state.owner);
    }
}

void close_dialog(HWND window, ContactDialogState* state) {
    if (state) restore_owner(*state);
    DestroyWindow(window);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<ContactDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE:
        state = reinterpret_cast<ContactDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!state) return -1;
        create_controls(window, *state);
        SetFocus(state->name);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK) {
            if (state) {
                Contact contact = contact_from_dialog(*state);
                if (!validate(window, *state, contact)) return 0;
                state->contact = std::move(contact);
                state->accepted = true;
            }
            close_dialog(window, state);
            return 0;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            close_dialog(window, state);
            return 0;
        }
        break;
    case WM_CLOSE:
        close_dialog(window, state);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

bool show_contact_dialog(
    HINSTANCE instance,
    HWND owner,
    HICON large_icon,
    HICON small_icon,
    LanguageSetting language,
    const Contact& initial,
    bool edit,
    Contact& result) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.hIcon = large_icon;
        window_class.hIconSm = small_icon;
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = kClassName;
        if (!RegisterClassExW(&window_class)) return false;
        registered = true;
    }

    ContactDialogState state;
    state.instance = instance;
    state.language = language;
    state.edit = edit;
    state.contact = initial;
    state.owner = owner;
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kClassName,
        localized_text(
            edit ? TextId::edit_contact_title : TextId::add_contact_title,
            language),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 470, 300,
        owner, nullptr, instance, &state);
    if (!dialog) return false;

    center_on_owner(dialog, owner);
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
    if (!state.owner_restored) restore_owner(state);
    SetActiveWindow(owner);
    if (!state.accepted) return false;
    result = state.contact;
    return true;
}

} // namespace lanspeak::gui
