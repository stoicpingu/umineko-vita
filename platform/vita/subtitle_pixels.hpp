#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace VitaSubtitles {
struct Bounds { int x{}, y{}, w{}, h{}; };

// libass bitmaps may overlap, extend outside the frame, and have no padding
// after their last row. Build only the visible region in premultiplied RGBA.
template<class Image>
Bounds render(Image *images, int width, int height, std::vector<uint8_t> &pixels) {
    int left = width, top = height, right = 0, bottom = 0;
    auto clip = [=](Image *i) {
        Bounds r;
        if (!i->bitmap || i->w <= 0 || i->h <= 0 || i->stride < i->w) return r;
        r.x = std::max(0, i->dst_x); r.y = std::max(0, i->dst_y);
        r.w = int(std::min<int64_t>(width, int64_t(i->dst_x) + i->w)) - r.x;
        r.h = int(std::min<int64_t>(height, int64_t(i->dst_y) + i->h)) - r.y;
        return r;
    };
    for (auto i = images; i; i = i->next) {
        auto r = clip(i);
        if (r.w <= 0 || r.h <= 0 || (i->color & 255) == 255) continue;
        left = std::min(left, r.x); top = std::min(top, r.y);
        right = std::max(right, r.x + r.w); bottom = std::max(bottom, r.y + r.h);
    }
    if (right <= left || bottom <= top) { pixels.clear(); return {}; }
    Bounds result{left, top, right - left, bottom - top};
    pixels.assign(size_t(result.w) * result.h * 4, 0);
    for (auto i = images; i; i = i->next) {
        auto r = clip(i);
        if (r.w <= 0 || r.h <= 0 || (i->color & 255) == 255) continue;
        const unsigned opacity = 255 - (i->color & 255);
        const unsigned color[]{i->color >> 24, (i->color >> 16) & 255, (i->color >> 8) & 255};
        for (int y = r.y; y < r.y + r.h; ++y) {
            const auto src = i->bitmap + size_t(y - i->dst_y) * i->stride;
            auto dst = pixels.data() + (size_t(y - top) * result.w + r.x - left) * 4;
            for (int x = r.x; x < r.x + r.w; ++x, dst += 4) {
                unsigned alpha = src[x - i->dst_x] * opacity / 255;
                for (int c = 0; c < 3; ++c)
                    dst[c] = (color[c] * alpha + dst[c] * (255 - alpha)) / 255;
                dst[3] = alpha + dst[3] * (255 - alpha) / 255;
            }
        }
    }
    return result;
}
}
