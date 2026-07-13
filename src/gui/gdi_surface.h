#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

namespace lanspeak::gui {

class CompatibleSurface {
public:
    CompatibleSurface() = default;
    ~CompatibleSurface();
    CompatibleSurface(const CompatibleSurface&) = delete;
    CompatibleSurface& operator=(const CompatibleSurface&) = delete;

    bool ensure(HDC reference, int width, int height);
    void reset();
    [[nodiscard]] HDC dc() const { return dc_; }
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }

private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_bitmap_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

class DibSurface {
public:
    DibSurface() = default;
    ~DibSurface();
    DibSurface(const DibSurface&) = delete;
    DibSurface& operator=(const DibSurface&) = delete;

    bool ensure(HDC reference, int width, int height);
    void reset();
    [[nodiscard]] HDC dc() const { return dc_; }
    [[nodiscard]] void* pixels() const { return pixels_; }
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }

private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_bitmap_ = nullptr;
    void* pixels_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

} // namespace lanspeak::gui
