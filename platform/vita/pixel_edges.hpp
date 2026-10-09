#pragma once
#include <cmath>

namespace VitaPixels {
// Same shared-edge rounding as native-bench/textbox.c. Return logical units
// because the full engine retains its virtual canvas and camera coordinates.
template<class Rect> Rect alignEdges(Rect rect, float sx, float sy) {
    if (sx <= 0 || sy <= 0) return rect;
    const float right = std::round((rect.x + rect.w) * sx) / sx;
    const float bottom = std::round((rect.y + rect.h) * sy) / sy;
    rect.x = std::round(rect.x * sx) / sx;
    rect.y = std::round(rect.y * sy) / sy;
    rect.w = right - rect.x;
    rect.h = bottom - rect.y;
    return rect;
}
}
