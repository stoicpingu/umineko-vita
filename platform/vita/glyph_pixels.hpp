#pragma once
#include <SDL2/SDL.h>
#include <algorithm>
#include <cmath>

namespace VitaPixels {
// FreeType already supplies coverage. Prepare the final premultiplied pixels
// on the CPU, as the native benchmark does, without rendering into a glyph FBO.
// Keep ONScripter's gradient equation and ignore SDL_Color.a: the engine uses
// {0,0,0,0} for a visible black outline, with opacity supplied by coverage.
inline SDL_Surface *glyphSurface(SDL_Surface *coverage, SDL_Color color,
                                 bool gradient, float maxy, float ascender) {
    if (!coverage || coverage->w <= 0 || coverage->h <= 0 ||
        coverage->format->BytesPerPixel != 1) return nullptr;
    SDL_Surface *out = SDL_CreateRGBSurface(0, coverage->w + 2, coverage->h + 2,
        32, 0xff, 0xff00, 0xff0000, 0xff000000);
    if (!out) return nullptr;
    SDL_FillRect(out, nullptr, 0);
    const bool white = color.r == 255 && color.g == 255 && color.b == 255;
    for (int y = 0; y < coverage->h; ++y) {
        const auto *src = static_cast<const Uint8 *>(coverage->pixels) + y * coverage->pitch;
        auto *dst = static_cast<Uint8 *>(out->pixels) + (y + 1) * out->pitch + 4;
        const float above = ascender > 0 ? std::max(0.0f, (maxy - (y + .5f)) / ascender) : 0;
        const float light = gradient ? .5f * (white ? above - .6f : .65f - above) : 0;
        const Uint8 rgb[] = {color.r, color.g, color.b};
        for (int x = 0; x < coverage->w; ++x, dst += 4) {
            for (int c = 0; c < 3; ++c)
                dst[c] = static_cast<Uint8>(std::round(std::max(0.0f,
                    std::min(255.0f, (rgb[c] / 255.0f + light) * src[x]))));
            dst[3] = src[x];
        }
    }
    return out;
}
}
