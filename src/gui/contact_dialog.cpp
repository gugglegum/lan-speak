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
constexpr int kReceiveBufferEditId = 1016;
constexpr int kReceiveBufferSliderId = 1017;
constexpr double kGainMin = 0.0;
constexpr double kGainMax = 3.0;
constexpr double kGainStep = 0.05;
constexpr int kReceiveBufferMinMs = 5;
constexpr int kReceiveBufferMaxMs = 100;
constexpr wchar_t kClassName[] = L"LanSpeakContactDialog";
constexpr wchar_t kReceiveBufferSliderClassName[] = L"LanSpeakReceiveBufferSlider";

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
    HWND receive_buffer_edit = nullptr;
    HWND receive_buffer_slider = nullptr;
    int receive_buffer_value_ms = 20;
    bool syncing_receive_buffer = false;
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

HWND add_receive_buffer_slider(
    HWND parent,
    int id,
    int x,
    int y,
    int width,
    int height,
    ContactDialogState* state) {
    HWND window = CreateWindowExW(
        0,
        kReceiveBufferSliderClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        x,
        y,
        width,
        height,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
        state);
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

void set_receive_buffer_value(
    ContactDialogState& state,
    int value,
    bool update_edit) {
    state.receive_buffer_value_ms = std::clamp(
        value,
        kReceiveBufferMinMs,
        kReceiveBufferMaxMs);
    HWND receive_buffer_edit = state.receive_buffer_slider
        ? GetDlgItem(GetParent(state.receive_buffer_slider), kReceiveBufferEditId)
        : state.receive_buffer_edit;
    if (update_edit && receive_buffer_edit) {
        const std::wstring value_text = std::to_wstring(state.receive_buffer_value_ms);
        if (window_text(receive_buffer_edit) != value_text) {
            state.syncing_receive_buffer = true;
            SetWindowTextW(receive_buffer_edit, value_text.c_str());
            state.syncing_receive_buffer = false;
        }
    }
    if (state.receive_buffer_slider) {
        InvalidateRect(state.receive_buffer_slider, nullptr, FALSE);
    }
}

int receive_buffer_value_from_x(HWND window, int x) {
    RECT client{};
    GetClientRect(window, &client);
    constexpr int padding = 8;
    const int usable_width = std::max(
        1,
        static_cast<int>(client.right - client.left) - padding * 2);
    const int clamped_x = std::clamp(x - padding, 0, usable_width);
    const double ratio = static_cast<double>(clamped_x) / static_cast<double>(usable_width);
    return kReceiveBufferMinMs + static_cast<int>(std::lround(
        ratio * static_cast<double>(kReceiveBufferMaxMs - kReceiveBufferMinMs)));
}

LRESULT CALLBACK receive_buffer_slider_proc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    auto* state = reinterpret_cast<ContactDialogState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));

        constexpr int padding = 8;
        const int center_y = (client.bottom - client.top) / 2;
        RECT track{padding, center_y - 2, client.right - padding, center_y + 2};
        HBRUSH track_brush = CreateSolidBrush(RGB(210, 218, 228));
        FillRect(dc, &track, track_brush);
        DeleteObject(track_brush);

        const int value = state
            ? state->receive_buffer_value_ms
            : 20;
        const double ratio = static_cast<double>(value - kReceiveBufferMinMs) /
            static_cast<double>(kReceiveBufferMaxMs - kReceiveBufferMinMs);
        const int thumb_x = padding + static_cast<int>(std::lround(
            ratio * static_cast<double>(std::max(
                1,
                static_cast<int>(client.right) - padding * 2))));
        RECT active_track = track;
        active_track.right = thumb_x;
        HBRUSH accent_brush = CreateSolidBrush(RGB(57, 120, 205));
        FillRect(dc, &active_track, accent_brush);
        HGDIOBJ old_brush = SelectObject(dc, accent_brush);
        HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, thumb_x - 6, center_y - 6, thumb_x + 7, center_y + 7);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(accent_brush);

        if (GetFocus() == window) {
            RECT focus = client;
            InflateRect(&focus, -1, -1);
            DrawFocusRect(dc, &focus);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(window);
        SetCapture(window);
        if (state) {
            set_receive_buffer_value(
                *state,
                receive_buffer_value_from_x(window, static_cast<short>(LOWORD(lparam))),
                true);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (state && (wparam & MK_LBUTTON) != 0 && GetCapture() == window) {
            set_receive_buffer_value(
                *state,
                receive_buffer_value_from_x(window, static_cast<short>(LOWORD(lparam))),
                true);
        }
        return 0;
    case WM_LBUTTONUP:
        if (state) {
            set_receive_buffer_value(
                *state,
                receive_buffer_value_from_x(window, static_cast<short>(LOWORD(lparam))),
                true);
        }
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        return 0;
    case WM_KEYDOWN:
        if (state) {
            int value = state->receive_buffer_value_ms;
            if (wparam == VK_LEFT || wparam == VK_DOWN) --value;
            else if (wparam == VK_RIGHT || wparam == VK_UP) ++value;
            else if (wparam == VK_PRIOR) value += 5;
            else if (wparam == VK_NEXT) value -= 5;
            else if (wparam == VK_HOME) value = kReceiveBufferMinMs;
            else if (wparam == VK_END) value = kReceiveBufferMaxMs;
            else return DefWindowProcW(window, message, wparam, lparam);
            set_receive_buffer_value(*state, value, true);
        }
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
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
    contact.receive_buffer_ms = parse_int(
        window_text_or(state.receive_buffer_edit, L"20"),
        20,
        kReceiveBufferMinMs,
        kReceiveBufferMaxMs);
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
    add_label(window, text(state, TextId::receive_buffer_ms), 12, 194, 105, 20);
    state.receive_buffer_slider = add_receive_buffer_slider(
        window, kReceiveBufferSliderId, 115, 184, 225, 32, &state);
    const int receive_buffer_ms = std::clamp(
        state.contact.receive_buffer_ms,
        kReceiveBufferMinMs,
        kReceiveBufferMaxMs);
    state.receive_buffer_value_ms = receive_buffer_ms;
    state.receive_buffer_edit = add_edit(
        window,
        kReceiveBufferEditId,
        std::to_wstring(receive_buffer_ms).c_str(),
        350,
        186,
        50,
        24);
    add_label(window, text(state, TextId::milliseconds_short), 407, 190, 35, 20);
    add_button(
        window, IDOK, state.edit ? text(state, TextId::save) : text(state, TextId::add),
        244, 240, 100, 28);
    add_button(window, IDCANCEL, text(state, TextId::cancel), 354, 240, 90, 28);
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
        if (state &&
            LOWORD(wparam) == kReceiveBufferEditId &&
            HIWORD(wparam) == EN_CHANGE &&
            !state->syncing_receive_buffer) {
            const std::wstring value = window_text(state->receive_buffer_edit);
            wchar_t* end = nullptr;
            const long parsed = std::wcstol(value.c_str(), &end, 10);
            if (end != value.c_str() && *end == L'\0' &&
                parsed >= kReceiveBufferMinMs && parsed <= kReceiveBufferMaxMs) {
                set_receive_buffer_value(*state, static_cast<int>(parsed), false);
            }
            return 0;
        }
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
        WNDCLASSEXW slider_class{};
        slider_class.cbSize = sizeof(slider_class);
        slider_class.lpfnWndProc = receive_buffer_slider_proc;
        slider_class.hInstance = instance;
        slider_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        slider_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        slider_class.lpszClassName = kReceiveBufferSliderClassName;
        if (!RegisterClassExW(&slider_class)) return false;

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
        CW_USEDEFAULT, CW_USEDEFAULT, 470, 330,
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
