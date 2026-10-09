#pragma once
#include <algorithm>
#include <cmath>
#include <string>

namespace VitaFocus {
struct Rect { float x, y, w, h; };

inline bool pictureButton(const char *filename) {
    if (!filename) return false;
    std::string path(filename);
    std::replace(path.begin(), path.end(), '\\', '/');
    return path.find("graphics/chars/") != std::string::npos ||
           path.find("graphics/thumb/") != std::string::npos;
}

// Draw inward, so the engine's existing button dirty rectangles erase the old
// focus completely when selection moves. Dark edging stays visible on light CGs.
template <typename Draw>
void outline(Rect bounds, Rect clip, Draw draw) {
    for (float f : {bounds.x, bounds.y, bounds.w, bounds.h, clip.x, clip.y, clip.w, clip.h})
        if (!std::isfinite(f)) return;
    if (bounds.w < 16 || bounds.h < 16 || clip.w <= 0 || clip.h <= 0) return;
    auto paint = [&](Rect r, bool gold) {
        float right = std::min(r.x + r.w, clip.x + clip.w);
        float bottom = std::min(r.y + r.h, clip.y + clip.h);
        r.x = std::max(r.x, clip.x); r.y = std::max(r.y, clip.y);
        r.w = right - r.x; r.h = bottom - r.y;
        if (r.w > 0 && r.h > 0) draw(r, gold);
    };
    for (bool gold : {false, true}) {
        float inset = gold ? 2.f : 0.f;
        float width = gold ? 4.f : 8.f;
        Rect r{bounds.x + inset, bounds.y + inset, bounds.w - inset * 2, bounds.h - inset * 2};
        paint({r.x, r.y, r.w, width}, gold);
        paint({r.x, r.y + r.h - width, r.w, width}, gold);
        paint({r.x, r.y + width, width, r.h - width * 2}, gold);
        paint({r.x + r.w - width, r.y + width, width, r.h - width * 2}, gold);
    }
}
} // namespace VitaFocus
