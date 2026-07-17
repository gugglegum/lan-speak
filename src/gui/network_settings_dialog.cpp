#include "gui/network_settings_dialog.h"

#include "gui/localization.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace lanspeak::gui {
namespace {

constexpr wchar_t kClassName[] = L"LanSpeakNetworkSettingsDialog";
constexpr int kPortEdit = 101;
constexpr int kAdapterCombo = 102;

struct DialogState {
    LanguageSetting language = LanguageSetting::automatic;
    HWND owner = nullptr;
    const std::vector<NetworkAdapterInfo>* adapters = nullptr;
    NetworkSettingsValue current;
    NetworkSettingsValue result;
    HWND port_edit = nullptr;
    HWND adapter_combo = nullptr;
    bool accepted = false;
    bool owner_restored = false;
};

const wchar_t* text(const DialogState& state, TextId id) {
    return localized_text(id, state.language);
}

void set_font(HWND control) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

HWND add_static(HWND parent, const wchar_t* caption, int x, int y, int width, int height) {
    HWND control = CreateWindowExW(
        0, L"STATIC", caption, WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, width, height, parent, nullptr, nullptr, nullptr);
    set_font(control);
    return control;
}

HWND add_button(HWND parent, int id, const wchar_t* caption, int x, int y, int width, int height, bool primary) {
    HWND control = CreateWindowExW(
        0, L"BUTTON", caption,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | (primary ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
        x, y, width, height, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    set_font(control);
    return control;
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

void restore_owner(DialogState& state) {
    if (state.owner_restored) {
        return;
    }
    state.owner_restored = true;
    if (state.owner != nullptr && IsWindow(state.owner)) {
        EnableWindow(state.owner, TRUE);
        SetActiveWindow(state.owner);
    }
}

void create_controls(HWND window, DialogState& state) {
    add_static(window, text(state, TextId::local_udp_port), 18, 22, 170, 22);
    state.port_edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(state.current.local_port).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
        196, 18, 120, 25, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPortEdit)), nullptr, nullptr);
    set_font(state.port_edit);
    SendMessageW(state.port_edit, EM_SETLIMITTEXT, 5, 0);

    add_static(window, text(state, TextId::network_interface), 18, 62, 170, 22);
    state.adapter_combo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        196, 58, 320, 240, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAdapterCombo)), nullptr, nullptr);
    set_font(state.adapter_combo);

    SendMessageW(
        state.adapter_combo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(text(state, TextId::all_network_interfaces)));
    int selected = 0;
    if (state.adapters != nullptr) {
        for (std::size_t index = 0; index < state.adapters->size(); ++index) {
            const NetworkAdapterInfo& adapter = (*state.adapters)[index];
            const std::wstring label = adapter.friendly_name + L" - " + adapter.ipv4_address;
            SendMessageW(
                state.adapter_combo,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(label.c_str()));
            if (adapter.id == state.current.adapter_id) {
                selected = static_cast<int>(index + 1);
            }
        }
    }
    SendMessageW(state.adapter_combo, CB_SETCURSEL, selected, 0);

    add_button(window, IDOK, text(state, TextId::save), 310, 112, 100, 30, true);
    add_button(window, IDCANCEL, text(state, TextId::cancel), 420, 112, 96, 30, false);
    SendMessageW(window, DM_SETDEFID, IDOK, 0);
    SetFocus(state.port_edit);
    SendMessageW(state.port_edit, EM_SETSEL, 0, -1);
}

bool read_result(HWND window, DialogState& state) {
    wchar_t port_text[16]{};
    GetWindowTextW(state.port_edit, port_text, static_cast<int>(std::size(port_text)));
    wchar_t* end = nullptr;
    const unsigned long port = std::wcstoul(port_text, &end, 10);
    if (end == port_text || *end != L'\0' || port == 0 || port > 65535) {
        MessageBoxW(window, text(state, TextId::network_port_invalid), L"LAN Speak", MB_OK | MB_ICONWARNING);
        SetFocus(state.port_edit);
        return false;
    }

    state.result.local_port = static_cast<std::uint16_t>(port);
    state.result.adapter_id.clear();
    const LRESULT selected = SendMessageW(state.adapter_combo, CB_GETCURSEL, 0, 0);
    if (selected > 0 && state.adapters != nullptr &&
        static_cast<std::size_t>(selected) <= state.adapters->size()) {
        state.result.adapter_id = (*state.adapters)[static_cast<std::size_t>(selected - 1)].id;
    }
    return true;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE:
        state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (state == nullptr) {
            return -1;
        }
        create_controls(window, *state);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK && state != nullptr) {
            if (read_result(window, *state)) {
                state->accepted = true;
                DestroyWindow(window);
            }
            return 0;
        }
        if (LOWORD(wparam) == IDCANCEL) {
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

bool show_network_settings_dialog(
    HINSTANCE instance,
    HWND owner,
    HICON large_icon,
    HICON small_icon,
    LanguageSetting language,
    const std::vector<NetworkAdapterInfo>& adapters,
    const NetworkSettingsValue& current,
    NetworkSettingsValue& result) {
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
            return false;
        }
        registered = true;
    }

    DialogState state;
    state.language = language;
    state.owner = owner;
    state.adapters = &adapters;
    state.current = current;

    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kClassName,
        localized_text(TextId::network_settings, language),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 550, 200,
        owner, nullptr, instance, &state);
    if (dialog == nullptr) {
        return false;
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
    if (state.accepted) {
        result = std::move(state.result);
    }
    return state.accepted;
}

} // namespace lanspeak::gui
