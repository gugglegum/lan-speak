#include "gui/gdi_surface.h"

#include <algorithm>

namespace lanspeak::gui {

CompatibleSurface::~CompatibleSurface() {
    reset();
}

bool CompatibleSurface::ensure(HDC reference, int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (dc_ && bitmap_ && width_ == width && height_ == height) {
        return true;
    }
    reset();
    dc_ = CreateCompatibleDC(reference);
    bitmap_ = CreateCompatibleBitmap(reference, width, height);
    if (!dc_ || !bitmap_) {
        reset();
        return false;
    }
    previous_bitmap_ = SelectObject(dc_, bitmap_);
    width_ = width;
    height_ = height;
    return true;
}

void CompatibleSurface::reset() {
    if (dc_ && previous_bitmap_) SelectObject(dc_, previous_bitmap_);
    previous_bitmap_ = nullptr;
    if (bitmap_) DeleteObject(bitmap_);
    bitmap_ = nullptr;
    if (dc_) DeleteDC(dc_);
    dc_ = nullptr;
    width_ = 0;
    height_ = 0;
}

DibSurface::~DibSurface() {
    reset();
}

bool DibSurface::ensure(HDC reference, int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (dc_ && bitmap_ && pixels_ && width_ == width && height_ == height) {
        return true;
    }
    reset();

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    dc_ = CreateCompatibleDC(reference);
    bitmap_ = CreateDIBSection(reference, &info, DIB_RGB_COLORS, &pixels_, nullptr, 0);
    if (!dc_ || !bitmap_ || !pixels_) {
        reset();
        return false;
    }
    previous_bitmap_ = SelectObject(dc_, bitmap_);
    width_ = width;
    height_ = height;
    return true;
}

void DibSurface::reset() {
    if (dc_ && previous_bitmap_) SelectObject(dc_, previous_bitmap_);
    previous_bitmap_ = nullptr;
    if (bitmap_) DeleteObject(bitmap_);
    bitmap_ = nullptr;
    if (dc_) DeleteDC(dc_);
    dc_ = nullptr;
    pixels_ = nullptr;
    width_ = 0;
    height_ = 0;
}

} // namespace lanspeak::gui
