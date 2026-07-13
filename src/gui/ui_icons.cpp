#include "gui/ui_icons.h"

#include <iterator>

namespace lanspeak::gui {

void draw_group_icon(
    HDC dc,
    const RECT& bounds,
    COLORREF color,
    bool crossed,
    GdiObjectCache& objects) {
    const int center_x = bounds.left + (bounds.right - bounds.left) / 2;
    const int center_y = bounds.top + (bounds.bottom - bounds.top) / 2;
    HBRUSH icon_brush = objects.brush(color);
    HPEN icon_pen = objects.pen(color, 2);
    HGDIOBJ old_brush = SelectObject(dc, icon_brush);
    HGDIOBJ old_pen = SelectObject(dc, icon_pen);

    Ellipse(dc, center_x - 3, center_y - 8, center_x + 4, center_y - 1);
    Ellipse(dc, center_x - 9, center_y - 6, center_x - 4, center_y - 1);
    Ellipse(dc, center_x + 5, center_y - 6, center_x + 10, center_y - 1);

    POINT center_body[] = {
        {center_x - 6, center_y + 7},
        {center_x - 5, center_y + 3},
        {center_x - 2, center_y},
        {center_x + 2, center_y},
        {center_x + 5, center_y + 3},
        {center_x + 6, center_y + 7}};
    Polyline(dc, center_body, static_cast<int>(std::size(center_body)));
    MoveToEx(dc, center_x - 10, center_y + 6, nullptr);
    LineTo(dc, center_x - 9, center_y + 2);
    LineTo(dc, center_x - 6, center_y);
    MoveToEx(dc, center_x + 10, center_y + 6, nullptr);
    LineTo(dc, center_x + 9, center_y + 2);
    LineTo(dc, center_x + 6, center_y);

    if (crossed) {
        MoveToEx(dc, bounds.left + 6, bounds.top + 5, nullptr);
        LineTo(dc, bounds.right - 5, bounds.bottom - 5);
    }

    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
}

} // namespace lanspeak::gui
