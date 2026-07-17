#include "gui/about_dialog.h"

#include "gui/localization.h"
#include "lanspeak_version.h"

#include <shellapi.h>

namespace lanspeak::gui {
namespace {

constexpr int kRepositoryLinkId = 1001;
constexpr wchar_t kClassName[] = L"LanSpeakAboutDialog";
constexpr wchar_t kRepositoryUrl[] = L"https://github.com/gugglegum/lan-speak";
constexpr wchar_t kRepositoryLabel[] = L"https://github.com/gugglegum/lan-speak";

struct AboutDialogState {
    LanguageSetting language = LanguageSetting::automatic;
    HWND owner = nullptr;
    HICON large_icon = nullptr;
    HWND repository_link = nullptr;
    HFONT heading_font = nullptr;
    HFONT link_font = nullptr;
    bool owner_restored = false;
};

const wchar_t* text(const AboutDialogState& state, TextId id) {
    return localized_text(id, state.language);
}

void set_font(HWND window, HFONT font = nullptr) {
    SendMessageW(
        window,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(font != nullptr ? font : GetStockObject(DEFAULT_GUI_FONT)),
        TRUE);
}

HFONT create_ui_font(HWND window, int point_size, int weight, bool underline = false) {
    LOGFONTW font{};
    GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(font), &font);
    const UINT dpi = GetDpiForWindow(window);
    font.lfHeight = -MulDiv(point_size, dpi != 0 ? static_cast<int>(dpi) : 96, 72);
    font.lfWeight = weight;
    font.lfUnderline = underline ? TRUE : FALSE;
    return CreateFontIndirectW(&font);
}

HWND add_static(
    HWND parent,
    const wchar_t* value,
    int x,
    int y,
    int width,
    int height,
    DWORD style = SS_LEFT) {
    HWND window = CreateWindowExW(
        0,
        L"STATIC",
        value,
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        width,
        height,
        parent,
        nullptr,
        nullptr,
        nullptr);
    set_font(window);
    return window;
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

void restore_owner(AboutDialogState& state) {
    if (state.owner_restored) {
        return;
    }
    state.owner_restored = true;
    if (state.owner != nullptr && IsWindow(state.owner)) {
        EnableWindow(state.owner, TRUE);
        SetActiveWindow(state.owner);
    }
}

void create_controls(HWND window, AboutDialogState& state) {
    state.heading_font = create_ui_font(window, 13, FW_SEMIBOLD);
    state.link_font = create_ui_font(window, 9, FW_NORMAL, true);

    HWND icon = CreateWindowExW(
        0,
        L"STATIC",
        nullptr,
        WS_CHILD | WS_VISIBLE | SS_ICON,
        20,
        22,
        48,
        48,
        window,
        nullptr,
        nullptr,
        nullptr);
    SendMessageW(icon, STM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(state.large_icon));

    const std::wstring product_version =
        std::wstring(text(state, TextId::about_product_version)) + LANSPEAK_VERSION_WSTRING;
    HWND heading = add_static(
        window,
        product_version.c_str(),
        88,
        20,
        310,
        30);
    set_font(heading, state.heading_font);
    add_static(window, text(state, TextId::about_author), 88, 58, 310, 22);
    add_static(window, text(state, TextId::about_github), 88, 91, 52, 22);

    state.repository_link = CreateWindowExW(
        0,
        L"BUTTON",
        kRepositoryLabel,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        140,
        85,
        280,
        30,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRepositoryLinkId)),
        nullptr,
        nullptr);
    set_font(state.repository_link, state.link_font);

    HWND ok = CreateWindowExW(
        0,
        L"BUTTON",
        text(state, TextId::ok),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        166,
        150,
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

void draw_repository_link(const DRAWITEMSTRUCT& item, const AboutDialogState& state) {
    FillRect(item.hDC, &item.rcItem, GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, RGB(0, 102, 204));
    const HGDIOBJ old_font = SelectObject(
        item.hDC,
        state.link_font != nullptr ? state.link_font : GetStockObject(DEFAULT_GUI_FONT));
    RECT text_rect = item.rcItem;
    DrawTextW(
        item.hDC,
        kRepositoryLabel,
        -1,
        &text_rect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, old_font);
    if ((item.itemState & ODS_FOCUS) != 0) {
        DrawFocusRect(item.hDC, &item.rcItem);
    }
}

void open_repository() {
    ShellExecuteW(nullptr, L"open", kRepositoryUrl, nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AboutDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE: {
        state = reinterpret_cast<AboutDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (state == nullptr) {
            return -1;
        }
        create_controls(window, *state);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wparam) == kRepositoryLinkId && HIWORD(wparam) == BN_CLICKED) {
            open_repository();
            return 0;
        }
        break;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (state != nullptr && item != nullptr && item->CtlID == kRepositoryLinkId) {
            draw_repository_link(*item, *state);
            return TRUE;
        }
        break;
    }
    case WM_SETCURSOR:
        if (state != nullptr && reinterpret_cast<HWND>(wparam) == state->repository_link) {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (state != nullptr) {
            restore_owner(*state);
            if (state->heading_font != nullptr) {
                DeleteObject(state->heading_font);
                state->heading_font = nullptr;
            }
            if (state->link_font != nullptr) {
                DeleteObject(state->link_font);
                state->link_font = nullptr;
            }
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

void show_about_dialog(
    HINSTANCE instance,
    HWND owner,
    HICON large_icon,
    HICON small_icon,
    LanguageSetting language) {
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

    AboutDialogState state;
    state.language = language;
    state.owner = owner;
    state.large_icon = large_icon;
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kClassName,
        localized_text(TextId::about, language),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        440,
        240,
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
