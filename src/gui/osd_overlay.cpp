#include "gui/osd_overlay.h"

#include "gui/ui_icons.h"
#include "gui/vu_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lanspeak::gui {
namespace {

constexpr int kIconSize = 48;
constexpr int kGap = 14;
constexpr int kPaddingX = 20;
constexpr int kPaddingY = 14;
constexpr int kRowGap = 8;
constexpr int kScreenMargin = 36;
constexpr int kOutlineRadius = 2;
constexpr int kTargetIconSize = 30;
constexpr int kTargetGap = 8;
constexpr int kMeterHeight = 7;
constexpr int kMeterGap = 5;
constexpr int kStreamMinimumPixels = 10;
constexpr wchar_t kWindowClass[] = L"LanSpeakOsdWindow";

void make_nonzero_pixels_opaque(void* pixels, int width, int height) {
    auto* data = static_cast<std::uint32_t*>(pixels);
    const std::size_t count = static_cast<std::size_t>(width) * height;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint32_t pixel = data[index];
        if ((pixel & 0xff000000u) == 0 && (pixel & 0x00ffffffu) != 0) {
            data[index] = pixel | 0xff000000u;
        }
    }
}

void draw_outlined_text(HDC dc, const std::wstring& text, RECT rect) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(5, 18, 26));
    for (int dy = -kOutlineRadius; dy <= kOutlineRadius; ++dy) {
        for (int dx = -kOutlineRadius; dx <= kOutlineRadius; ++dx) {
            if (dx == 0 && dy == 0) continue;
            RECT outline = rect;
            OffsetRect(&outline, dx, dy);
            DrawTextW(
                dc, text.c_str(), static_cast<int>(text.size()), &outline,
                DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        }
    }
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(
        dc, text.c_str(), static_cast<int>(text.size()), &rect,
        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

void draw_outlined_group_icon(HDC dc, const RECT& rect, GdiObjectCache& objects) {
    for (int dy = -kOutlineRadius; dy <= kOutlineRadius; ++dy) {
        for (int dx = -kOutlineRadius; dx <= kOutlineRadius; ++dx) {
            if (dx == 0 && dy == 0) continue;
            RECT outline = rect;
            OffsetRect(&outline, dx, dy);
            draw_group_icon(dc, outline, RGB(5, 18, 26), false, objects);
        }
    }
    draw_group_icon(dc, rect, RGB(255, 255, 255), false, objects);
}

void fill_dib_rect(void* pixels, int width, int height, RECT rect, COLORREF color) {
    rect.left = std::clamp<LONG>(rect.left, 0, width);
    rect.right = std::clamp<LONG>(rect.right, 0, width);
    rect.top = std::clamp<LONG>(rect.top, 0, height);
    rect.bottom = std::clamp<LONG>(rect.bottom, 0, height);
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    auto* data = static_cast<std::uint32_t*>(pixels);
    const std::uint32_t argb = 0xff000000u | static_cast<std::uint32_t>(color);
    for (LONG y = rect.top; y < rect.bottom; ++y) {
        std::fill(
            data + static_cast<std::size_t>(y) * width + rect.left,
            data + static_cast<std::size_t>(y) * width + rect.right,
            argb);
    }
}

void draw_meter_track(void* pixels, int width, int height, const RECT& rect) {
    fill_dib_rect(pixels, width, height, rect, RGB(5, 18, 26));
    RECT inner = rect;
    InflateRect(&inner, -1, -1);
    fill_dib_rect(pixels, width, height, inner, RGB(91, 103, 116));
}

void draw_meter(void* pixels, int width, int height, const RECT& rect, const OsdRow& row) {
    RECT inner = rect;
    InflateRect(&inner, -1, -1);
    const LONG track_width = inner.right - inner.left;
    if (track_width <= 0 || inner.bottom <= inner.top) return;
    LONG fill_width = static_cast<LONG>(std::lround(track_width * vu_fill_ratio(row.level_db)));
    if (row.stream_active) {
        fill_width = std::max<LONG>(fill_width, std::min<LONG>(track_width, kStreamMinimumPixels));
    }
    fill_width = std::clamp<LONG>(fill_width, 0, track_width);
    if (fill_width <= 0) return;
    RECT fill = inner;
    fill.right = fill.left + fill_width;
    fill_dib_rect(
        pixels, width, height, fill,
        row.voice_active ? RGB(42, 190, 94) : RGB(190, 201, 213));
}

} // namespace

OsdOverlay::~OsdOverlay() {
    reset();
}

LRESULT CALLBACK OsdOverlay::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wparam, lparam);
}

bool OsdOverlay::ensure_window(HINSTANCE instance) {
    if (window_ && IsWindow(window_)) return true;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass, L"", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, instance, nullptr);
    return window_ != nullptr;
}

HFONT OsdOverlay::ensure_font(HDC dc) {
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (font_ && font_dpi_ == dpi) return font_;
    if (font_) DeleteObject(font_);
    font_dpi_ = dpi;
    static_signature_.clear();
    static_pixels_.clear();
    font_ = CreateFontW(
        -MulDiv(28, dpi, 72), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    return font_;
}

bool OsdOverlay::show(
    HINSTANCE instance,
    std::span<const OsdRow> rows,
    const RECT& monitor_rect,
    HICON speaking_icon) {
    if (rows.empty()) {
        hide();
        return true;
    }
    if (!ensure_window(instance)) return false;

    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) return false;
    HFONT font = ensure_font(screen_dc);
    if (!font) {
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    HGDIOBJ old_screen_font = SelectObject(screen_dc, font);
    int max_content_width = 0;
    int text_height = 0;
    for (const OsdRow& row : rows) {
        RECT measure{0, 0, 1, 1};
        DrawTextW(
            screen_dc, row.label.c_str(), static_cast<int>(row.label.size()), &measure,
            DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        int content_width = measure.right - measure.left;
        if (row.group_target) content_width += kTargetGap + kTargetIconSize;
        max_content_width = std::max(max_content_width, content_width);
        text_height = std::max(text_height, static_cast<int>(measure.bottom - measure.top));
    }
    if (old_screen_font) SelectObject(screen_dc, old_screen_font);

    const int row_height = std::max(
        kIconSize, text_height + kOutlineRadius * 2 + kMeterGap + kMeterHeight);
    const int width = std::max(
        240, kPaddingX * 2 + kIconSize + kGap + max_content_width + kOutlineRadius * 2);
    const int height = kPaddingY * 2 + static_cast<int>(rows.size()) * row_height +
        static_cast<int>(rows.size() - 1) * kRowGap;
    if (!surface_.ensure(screen_dc, width, height)) {
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    HDC memory_dc = surface_.dc();
    void* pixels = surface_.pixels();
    HGDIOBJ old_font = SelectObject(memory_dc, font);
    SetBkMode(memory_dc, TRANSPARENT);

    std::wstring signature = std::to_wstring(width) + L"x" + std::to_wstring(height);
    for (const OsdRow& row : rows) {
        signature.push_back(row.group_target ? L'G' : L'C');
        signature.append(row.label);
        signature.push_back(L'\0');
    }
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    if (signature != static_signature_ || static_pixels_.size() != pixel_count) {
        std::fill_n(static_cast<std::uint32_t*>(pixels), pixel_count, 0u);
        int y = kPaddingY;
        for (const OsdRow& row : rows) {
            const int icon_y = y + (row_height - kIconSize) / 2;
            DrawIconEx(
                memory_dc, kPaddingX, icon_y, speaking_icon,
                kIconSize, kIconSize, 0, nullptr, DI_NORMAL);
            const int text_left = kPaddingX + kIconSize + kGap;
            const int text_bottom = y + row_height - kMeterGap - kMeterHeight;
            RECT text_rect{text_left, y, width - kPaddingX, text_bottom};
            draw_outlined_text(memory_dc, row.label, text_rect);
            if (row.group_target) {
                SIZE label_size{};
                GetTextExtentPoint32W(
                    memory_dc, row.label.c_str(), static_cast<int>(row.label.size()), &label_size);
                const int icon_left = text_left + label_size.cx + kTargetGap;
                const int icon_center_y = y + (text_bottom - y) / 2;
                RECT group_rect{
                    icon_left, icon_center_y - kTargetIconSize / 2,
                    icon_left + kTargetIconSize,
                    icon_center_y + (kTargetIconSize + 1) / 2};
                draw_outlined_group_icon(memory_dc, group_rect, gdi_objects_);
            }
            RECT meter_rect{
                text_left, y + row_height - kMeterHeight,
                width - kPaddingX, y + row_height};
            draw_meter_track(pixels, width, height, meter_rect);
            y += row_height + kRowGap;
        }
        make_nonzero_pixels_opaque(pixels, width, height);
        static_pixels_.assign(
            static_cast<std::uint32_t*>(pixels),
            static_cast<std::uint32_t*>(pixels) + pixel_count);
        static_signature_ = std::move(signature);
    } else {
        std::copy(static_pixels_.begin(), static_pixels_.end(), static_cast<std::uint32_t*>(pixels));
    }

    int y = kPaddingY;
    const int meter_left = kPaddingX + kIconSize + kGap;
    for (const OsdRow& row : rows) {
        RECT meter_rect{
            meter_left, y + row_height - kMeterHeight,
            width - kPaddingX, y + row_height};
        draw_meter(pixels, width, height, meter_rect, row);
        y += row_height + kRowGap;
    }

    POINT position{
        monitor_rect.right - width - kScreenMargin,
        monitor_rect.bottom - height - kScreenMargin};
    SIZE window_size{width, height};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL updated = UpdateLayeredWindow(
        window_, screen_dc, &position, &window_size, memory_dc,
        &source, 0, &blend, ULW_ALPHA);
    if (old_font) SelectObject(memory_dc, old_font);
    ReleaseDC(nullptr, screen_dc);
    if (!updated) return false;

    ShowWindow(window_, SW_SHOWNOACTIVATE);
    SetWindowPos(
        window_, HWND_TOPMOST, position.x, position.y, width, height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

void OsdOverlay::hide() {
    if (window_ && IsWindow(window_)) ShowWindow(window_, SW_HIDE);
}

void OsdOverlay::reset() {
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
    window_ = nullptr;
    surface_.reset();
    gdi_objects_.reset();
    static_pixels_.clear();
    static_signature_.clear();
    if (font_) DeleteObject(font_);
    font_ = nullptr;
    font_dpi_ = 0;
}

} // namespace lanspeak::gui
