#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>

#include "gui/peer_discovery_dialog.h"

#include "common/text_codec.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace lanspeak::gui {
namespace {

constexpr wchar_t kWindowClass[] = L"LanSpeakPeerDiscoveryDialog";
constexpr UINT_PTR kSearchTimer = 1;
constexpr int kListId = 101;
constexpr int kRefreshId = 102;
constexpr int kAddId = 103;
constexpr int kCloseId = 104;
constexpr int kMargin = 12;
constexpr int kButtonWidth = 132;
constexpr int kButtonHeight = 28;
constexpr int kGap = 8;

void set_default_font(HWND window) {
    SendMessageW(
        window,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
        TRUE);
}

std::uint64_t next_request_id() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    std::uint64_t value = static_cast<std::uint64_t>(counter.QuadPart) ^
        (GetTickCount64() << 19u) ^
        (static_cast<std::uint64_t>(GetCurrentProcessId()) << 43u);
    return value == 0 ? 1 : value;
}

} // namespace

bool PeerDiscoveryDialog::show(
    HINSTANCE instance,
    HWND owner,
    HICON large_icon,
    HICON small_icon,
    PeerDiscoveryDialogText text,
    RefreshCallback refresh,
    AddCallback add) {
    if (window_) return false;
    instance_ = instance;
    owner_ = owner;
    large_icon_ = large_icon;
    small_icon_ = small_icon;
    text_ = text;
    refresh_ = std::move(refresh);
    add_ = std::move(add);
    peers_.clear();
    added_endpoints_.clear();
    added_any_ = false;

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    RegisterClassExW(&window_class);

    window_ = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kWindowClass,
        text_.title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        650,
        400,
        owner_,
        nullptr,
        instance_,
        this);
    if (!window_) return false;
    EnableWindow(owner_, FALSE);
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
    begin_search();

    MSG message{};
    while (window_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window_, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner_, TRUE);
    SetActiveWindow(owner_);
    return added_any_;
}

void PeerDiscoveryDialog::apply_telemetry(const TelemetrySnapshot& snapshot) {
    if (!window_ || request_id_ == 0) return;
    std::vector<DiscoveredPeer> incoming;
    for (const DiscoveryPeerTelemetry& peer : snapshot.discovery_peers) {
        if (peer.request_id != request_id_) continue;
        DiscoveredPeer discovered{
            peer.request_id,
            peer.session_id,
            common::utf8_to_wide(peer.computer_name_utf8),
            common::utf8_to_wide(peer.ip_utf8),
            peer.voice_port,
            peer.already_contact};
        discovered.already_contact = discovered.already_contact || std::any_of(
            added_endpoints_.begin(),
            added_endpoints_.end(),
            [&](const auto& endpoint) {
                return endpoint.second == discovered.voice_port &&
                    _wcsicmp(endpoint.first.c_str(), discovered.ip_address.c_str()) == 0;
            });
        incoming.push_back(std::move(discovered));
    }
    if (!incoming.empty() || !peers_.empty()) {
        peers_ = std::move(incoming);
        std::sort(peers_.begin(), peers_.end(), [](const auto& left, const auto& right) {
            return _wcsicmp(left.computer_name.c_str(), right.computer_name.c_str()) < 0;
        });
        rebuild_list();
    }
    if (snapshot.discovery_error.valid &&
        snapshot.discovery_error.request_id == request_id_) {
        error_code_ = snapshot.discovery_error.error_code;
        finish_search_status();
    }
}

LRESULT CALLBACK PeerDiscoveryDialog::window_proc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    PeerDiscoveryDialog* self = reinterpret_cast<PeerDiscoveryDialog*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<PeerDiscoveryDialog*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle_message(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT PeerDiscoveryDialog::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        if (large_icon_) SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon_));
        if (small_icon_) SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon_));
        create_controls();
        return 0;
    case WM_SIZE:
        layout_controls(LOWORD(lparam), HIWORD(lparam));
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = 560;
        info->ptMinTrackSize.y = 320;
        return 0;
    }
    case WM_TIMER:
        if (wparam == kSearchTimer) {
            KillTimer(window_, kSearchTimer);
            search_finished_ = true;
            finish_search_status();
        }
        return 0;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header && header->hwndFrom == list_) {
            if (header->code == LVN_ITEMCHANGED) update_add_button();
            if (header->code == NM_DBLCLK) add_selected();
            if (header->code == NM_CUSTOMDRAW) {
                auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
                if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    const std::size_t index = static_cast<std::size_t>(draw->nmcd.dwItemSpec);
                    if (index < peers_.size() && peers_[index].already_contact) {
                        draw->clrText = GetSysColor(COLOR_GRAYTEXT);
                    }
                    return CDRF_DODEFAULT;
                }
            }
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kRefreshId: begin_search(); return 0;
        case kAddId: add_selected(); return 0;
        case kCloseId: DestroyWindow(window_); return 0;
        default: break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        KillTimer(window_, kSearchTimer);
        window_ = nullptr;
        list_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

void PeerDiscoveryDialog::create_controls() {
    list_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)),
        instance_,
        nullptr);
    SendMessageW(
        list_,
        LVM_SETEXTENDEDLISTVIEWSTYLE,
        0,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t* headings[] = {text_.computer_name, text_.ip_address, text_.port, text_.state};
    const int widths[] = {205, 135, 70, 150};
    for (int index = 0; index < 4; ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(headings[index]);
        column.cx = widths[index];
        column.iSubItem = index;
        SendMessageW(list_, LVM_INSERTCOLUMNW, index, reinterpret_cast<LPARAM>(&column));
    }
    status_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    refresh_button_ = CreateWindowExW(0, L"BUTTON", text_.refresh,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshId)), instance_, nullptr);
    add_button_ = CreateWindowExW(0, L"BUTTON", text_.add,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddId)), instance_, nullptr);
    close_button_ = CreateWindowExW(0, L"BUTTON", text_.close,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseId)), instance_, nullptr);
    for (HWND control : {list_, status_, refresh_button_, add_button_, close_button_}) set_default_font(control);
    RECT client{};
    GetClientRect(window_, &client);
    layout_controls(client.right, client.bottom);
    update_add_button();
}

void PeerDiscoveryDialog::layout_controls(int width, int height) {
    const int bottom = height - kMargin - kButtonHeight;
    const int buttons_width = kButtonWidth * 3 + kGap * 2;
    MoveWindow(list_, kMargin, kMargin, std::max(1, width - 2 * kMargin),
        std::max(1, bottom - kMargin - kGap), TRUE);
    MoveWindow(status_, kMargin, bottom + 6,
        std::max(1, width - 2 * kMargin - buttons_width - kGap), kButtonHeight, TRUE);
    int x = width - kMargin - buttons_width;
    MoveWindow(refresh_button_, x, bottom, kButtonWidth, kButtonHeight, TRUE);
    x += kButtonWidth + kGap;
    MoveWindow(add_button_, x, bottom, kButtonWidth, kButtonHeight, TRUE);
    x += kButtonWidth + kGap;
    MoveWindow(close_button_, x, bottom, kButtonWidth, kButtonHeight, TRUE);
}

void PeerDiscoveryDialog::begin_search() {
    request_id_ = next_request_id();
    peers_.clear();
    error_code_ = 0;
    search_finished_ = false;
    ListView_DeleteAllItems(list_);
    SetWindowTextW(status_, text_.searching);
    update_add_button();
    KillTimer(window_, kSearchTimer);
    SetTimer(window_, kSearchTimer, 2000, nullptr);
    if (!refresh_ || !refresh_(request_id_)) {
        error_code_ = ERROR_BROKEN_PIPE;
        finish_search_status();
    }
}

void PeerDiscoveryDialog::rebuild_list() {
    const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    ListView_DeleteAllItems(list_);
    for (std::size_t index = 0; index < peers_.size(); ++index) {
        const DiscoveredPeer& peer = peers_[index];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<wchar_t*>(peer.computer_name.c_str());
        item.lParam = static_cast<LPARAM>(index);
        SendMessageW(list_, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        LVITEMW subitem{};
        subitem.iSubItem = 1;
        subitem.pszText = const_cast<wchar_t*>(peer.ip_address.c_str());
        SendMessageW(list_, LVM_SETITEMTEXTW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&subitem));
        const std::wstring port = std::to_wstring(peer.voice_port);
        subitem.iSubItem = 2;
        subitem.pszText = const_cast<wchar_t*>(port.c_str());
        SendMessageW(list_, LVM_SETITEMTEXTW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&subitem));
        subitem.iSubItem = 3;
        subitem.pszText = const_cast<wchar_t*>(
            peer.already_contact ? text_.already_in_contacts : text_.available);
        SendMessageW(list_, LVM_SETITEMTEXTW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&subitem));
    }
    if (selected >= 0 && selected < static_cast<int>(peers_.size())) {
        ListView_SetItemState(list_, selected, LVIS_SELECTED | LVIS_FOCUSED,
            LVIS_SELECTED | LVIS_FOCUSED);
    }
    update_add_button();
    if (search_finished_ || error_code_ != 0) finish_search_status();
}

void PeerDiscoveryDialog::update_add_button() {
    const int selected = list_ ? ListView_GetNextItem(list_, -1, LVNI_SELECTED) : -1;
    const bool enabled = selected >= 0 && selected < static_cast<int>(peers_.size()) &&
        !peers_[static_cast<std::size_t>(selected)].already_contact;
    if (add_button_) EnableWindow(add_button_, enabled);
}

void PeerDiscoveryDialog::add_selected() {
    const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (selected < 0 || selected >= static_cast<int>(peers_.size())) return;
    DiscoveredPeer& peer = peers_[static_cast<std::size_t>(selected)];
    if (peer.already_contact || !add_ || !add_(peer)) return;
    peer.already_contact = true;
    added_endpoints_.emplace_back(peer.ip_address, peer.voice_port);
    added_any_ = true;
    rebuild_list();
}

void PeerDiscoveryDialog::finish_search_status() {
    if (!status_) return;
    if (error_code_ != 0) {
        std::wostringstream message;
        message << text_.error << L" " << error_code_;
        SetWindowTextW(status_, message.str().c_str());
    } else if (peers_.empty()) {
        SetWindowTextW(status_, text_.nothing_found);
    } else {
        std::wostringstream message;
        message << text_.found << L" " << peers_.size();
        SetWindowTextW(status_, message.str().c_str());
    }
}

} // namespace lanspeak::gui
