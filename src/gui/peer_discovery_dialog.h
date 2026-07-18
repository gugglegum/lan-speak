#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "gui/telemetry_protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace lanspeak::gui {

struct PeerDiscoveryDialogText {
    const wchar_t* title = L"";
    const wchar_t* computer_name = L"";
    const wchar_t* ip_address = L"";
    const wchar_t* port = L"";
    const wchar_t* state = L"";
    const wchar_t* refresh = L"";
    const wchar_t* add = L"";
    const wchar_t* close = L"";
    const wchar_t* searching = L"";
    const wchar_t* found = L"";
    const wchar_t* nothing_found = L"";
    const wchar_t* available = L"";
    const wchar_t* already_in_contacts = L"";
    const wchar_t* error = L"";
};

struct DiscoveredPeer {
    std::uint64_t request_id = 0;
    std::uint64_t session_id = 0;
    std::wstring computer_name;
    std::wstring ip_address;
    std::uint16_t voice_port = 0;
    bool already_contact = false;
};

class PeerDiscoveryDialog {
public:
    using RefreshCallback = std::function<bool(std::uint64_t)>;
    using AddCallback = std::function<bool(const DiscoveredPeer&)>;

    bool show(
        HINSTANCE instance,
        HWND owner,
        HICON large_icon,
        HICON small_icon,
        PeerDiscoveryDialogText text,
        RefreshCallback refresh,
        AddCallback add);
    void apply_telemetry(const TelemetrySnapshot& snapshot);
    [[nodiscard]] bool open() const { return window_ != nullptr; }

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    void create_controls();
    void layout_controls(int width, int height);
    void begin_search();
    void rebuild_list();
    void update_add_button();
    void add_selected();
    void finish_search_status();

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    HWND list_ = nullptr;
    HWND status_ = nullptr;
    HWND refresh_button_ = nullptr;
    HWND add_button_ = nullptr;
    HWND close_button_ = nullptr;
    HICON large_icon_ = nullptr;
    HICON small_icon_ = nullptr;
    PeerDiscoveryDialogText text_;
    RefreshCallback refresh_;
    AddCallback add_;
    std::vector<DiscoveredPeer> peers_;
    std::vector<std::pair<std::wstring, std::uint16_t>> added_endpoints_;
    std::uint64_t request_id_ = 0;
    bool added_any_ = false;
    bool search_finished_ = false;
    int error_code_ = 0;
};

} // namespace lanspeak::gui
