#pragma once
#include <SDL2/SDL.h>
#include <cstdint>

namespace VitaPixels {
// The PoC prepares premultiplied RGBA before uploading. Do the same for the
// full engine's CPU images, preserving the straight-alpha surface/cache owner.
// Generated GPU effects still use their original shader implementation.
inline SDL_Surface *premultipliedCopy(SDL_Surface *source) {
    if (!source || source->w <= 0 || source->h <= 0 ||
        source->format->BytesPerPixel != 4 ||
        source->format->Rmask != 0x000000ff || source->format->Gmask != 0x0000ff00 ||
        source->format->Bmask != 0x00ff0000 || source->format->Amask != 0xff000000)
        return nullptr;
    SDL_Surface *result = SDL_ConvertSurface(source, source->format, 0);
    if (!result) return nullptr;
    if (SDL_MUSTLOCK(result) && SDL_LockSurface(result) != 0) {
        SDL_FreeSurface(result);
        return nullptr;
    }
    for (int y = 0; y < result->h; ++y) {
        auto *row = static_cast<uint8_t *>(result->pixels) + y * result->pitch;
        for (int x = 0; x < result->w; ++x) {
            auto *p = row + 4 * x;
            const unsigned alpha = p[3];
            p[0] = (p[0] * alpha + 127) / 255;
            p[1] = (p[1] * alpha + 127) / 255;
            p[2] = (p[2] * alpha + 127) / 255;
        }
    }
    if (SDL_MUSTLOCK(result)) SDL_UnlockSurface(result);
    return result;
}
}
