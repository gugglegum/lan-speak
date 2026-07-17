#include "gui/audio_latency_dialog.h"

#include "common/text_codec.h"
#include "gui/localization.h"

#include <iomanip>
#include <sstream>
#include <string>

namespace lanspeak::gui {
namespace {

constexpr wchar_t kClassName[] = L"LanSpeakAudioLatencyDialog";

struct AudioLatencyDialogState {
    LanguageSetting language = LanguageSetting::automatic;
    HWND owner = nullptr;
    AudioEndpointTelemetry capture;
    AudioEndpointTelemetry render;
    bool owner_restored = false;
};

const wchar_t* text(const AudioLatencyDialogState& state, TextId id) {
    return localized_text(id, state.language);
}

void set_font(HWND window) {
    SendMessageW(
        window,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
        TRUE);
}

HWND add_static(
    HWND parent,
    const std::wstring& value,
    int x,
    int y,
    int width,
    int height) {
    HWND control = CreateWindowExW(
        0,
        L"STATIC",
        value.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x,
        y,
        width,
        height,
        parent,
        nullptr,
        nullptr,
        nullptr);
    set_font(control);
    return control;
}

HWND add_group_box(
    HWND parent,
    const wchar_t* caption,
    int x,
    int y,
    int width,
    int height) {
    HWND control = CreateWindowExW(
        0,
        L"BUTTON",
        caption,
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        x,
        y,
        width,
        height,
        parent,
        nullptr,
        nullptr,
        nullptr);
    set_font(control);
    return control;
}

std::wstring format_ms(const AudioLatencyDialogState& state, double value) {
    if (value < 0.0) {
        return text(state, TextId::diagnostics_not_available_short);
    }
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(2) << value << L" "
           << text(state, TextId::milliseconds_short);
    return stream.str();
}

std::wstring format_endpoint(
    const AudioLatencyDialogState& state,
    const AudioEndpointTelemetry& endpoint,
    bool include_padding) {
    if (!endpoint.valid) {
        return text(state, TextId::diagnostics_unavailable);
    }

    std::wstring name = common::utf8_to_wide(endpoint.name_utf8);
    if (name.empty()) {
        name = text(state, TextId::diagnostics_not_available_short);
    }

    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << text(state, TextId::device_name) << L": " << name << L"\r\n"
           << text(state, TextId::audio_format) << L": "
           << endpoint.sample_rate << L" Hz, "
           << endpoint.channels << L" " << text(state, TextId::channels_short) << L", "
           << endpoint.bits_per_sample << L" " << text(state, TextId::bits_short) << L"\r\n"
           << text(state, TextId::audio_engine_period) << L": "
           << format_ms(state, endpoint.engine_period_ms) << L"\r\n"
           << text(state, TextId::endpoint_buffer) << L": "
           << format_ms(state, endpoint.buffer_ms) << L"\r\n"
           << text(state, TextId::maximum_stream_latency) << L": "
           << format_ms(state, endpoint.stream_latency_ms) << L"\r\n";
    if (include_padding) {
        stream << text(state, TextId::current_render_padding) << L": "
               << format_ms(state, endpoint.current_padding_ms) << L"\r\n";
    }
    stream << text(state, TextId::stream_mode) << L": "
           << text(
                  state,
                  endpoint.low_latency_shared
                      ? TextId::low_latency_shared_mode
                      : TextId::standard_shared_mode);
    return stream.str();
}

void center_on_owner(HWND window, HWND owner) {
    RECT window_rect{};
    RECT owner_rect{};
    GetWindowRect(window, &window_rect);
    if (owner != nullptr && IsWindow(owner)) {
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

void restore_owner(AudioLatencyDialogState& state) {
    if (state.owner_restored) {
        return;
    }
    state.owner_restored = true;
    if (state.owner != nullptr && IsWindow(state.owner)) {
        EnableWindow(state.owner, TRUE);
        SetActiveWindow(state.owner);
    }
}

void create_controls(HWND window, const AudioLatencyDialogState& state) {
    add_group_box(window, text(state, TextId::capture_device), 16, 14, 572, 154);
    add_static(window, format_endpoint(state, state.capture, false), 30, 38, 540, 118);

    add_group_box(window, text(state, TextId::render_device), 16, 178, 572, 174);
    add_static(window, format_endpoint(state, state.render, true), 30, 202, 540, 140);

    add_static(window, text(state, TextId::latency_diagnostics_note), 20, 366, 564, 42);

    HWND ok = CreateWindowExW(
        0,
        L"BUTTON",
        text(state, TextId::ok),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        252,
        420,
        100,
        30,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
        nullptr,
        nullptr);
    set_font(ok);
    SendMessageW(window, DM_SETDEFID, IDOK, 0);
    SetFocus(ok);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AudioLatencyDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE:
        state = reinterpret_cast<AudioLatencyDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (state == nullptr) {
            return -1;
        }
        create_controls(window, *state);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (state != nullptr) {
            restore_owner(*state);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

void show_audio_latency_dialog(
    HINSTANCE instance,
    HWND owner,
    HICON large_icon,
    HICON small_icon,
    LanguageSetting language,
    const AudioEndpointTelemetry& capture,
    const AudioEndpointTelemetry& render) {
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
        if (!RegisterClassExW(&window_class)) {
            return;
        }
        registered = true;
    }

    AudioLatencyDialogState state;
    state.language = language;
    state.owner = owner;
    state.capture = capture;
    state.render = render;

    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kClassName,
        localized_text(TextId::audio_latency_diagnostics, language),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        620,
        500,
        owner,
        nullptr,
        instance,
        &state);
    if (dialog == nullptr) {
        return;
    }

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
    if (!state.owner_restored) {
        restore_owner(state);
    }
}

} // namespace lanspeak::gui
