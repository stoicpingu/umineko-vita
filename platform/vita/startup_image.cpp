#include "platform/vita/startup_image.hpp"
#include "platform/vita/platform.hpp"
#include <SDL_gpu.h>
#include <png.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace VitaPlatform {
static SceUID block = -1;
static void *pixels;
static bool shown;
static bool handedOff;

bool startupImageShown() { return shown; }
bool startupImagePending() { return shown && block >= 0 && !handedOff; }

void showStartupImage() {
    if (shown || block >= 0) return;
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    const std::string path = std::string(gameRoot()) + "graphics/locale_en/project_logo.png";
    if (!png_image_begin_read_from_file(&image, path.c_str())) {
        png_image_free(&image);
        return;
    }
    image.format = PNG_FORMAT_RGBA;
    if (!image.width || !image.height || image.width > 1920 || image.height > 1080) {
        png_image_free(&image);
        return;
    }
    auto source = static_cast<uint32_t *>(std::malloc(PNG_IMAGE_SIZE(image)));
    if (!source || !png_image_finish_read(&image, nullptr, source, 0, nullptr)) {
        std::free(source);
        png_image_free(&image);
        return;
    }
    block = sceKernelAllocMemBlock("umineko-startup", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 0x200000, nullptr);
    if (block >= 0 && sceKernelGetMemBlockBase(block, &pixels) >= 0) {
        auto destination = static_cast<uint32_t *>(pixels);
        std::fill(destination, destination + 960 * 544, 0xff000000u);
        const double scale = std::min(960.0 / image.width, 544.0 / image.height);
        const int width = int(image.width * scale), height = int(image.height * scale);
        const int left = (960 - width) / 2, top = (544 - height) / 2;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                destination[(top + y) * 960 + left + x] =
                    source[(size_t(y) * image.height / height) * image.width + size_t(x) * image.width / width] | 0xff000000u;
        SceDisplayFrameBuf frame{};
        frame.size = sizeof(frame); frame.base = pixels; frame.pitch = 960;
        frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
        frame.width = 960; frame.height = 544;
        shown = sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME) >= 0;
        if (shown) sceDisplayWaitVblankStart();
    }
    if (!shown && block >= 0) {
        sceKernelFreeMemBlock(block);
        block = -1;
        pixels = nullptr;
    }
    std::free(source);
    png_image_free(&image);
}

void releaseStartupImage() {
    if (block < 0) return;
    SceDisplayFrameBuf current{}, next{};
    current.size = sizeof(current); next.size = sizeof(next);
    if (sceDisplayGetFrameBuf(&current, SCE_DISPLAY_SETBUF_IMMEDIATE) < 0 ||
        sceDisplayGetFrameBuf(&next, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0 ||
        current.base == pixels || next.base == pixels) return;
    sceKernelFreeMemBlock(block);
    block = -1;
    pixels = nullptr;
}

void handoffStartupImage(GPU_Target *target) {
    if (!shown || !pixels || !target) return;
    SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(pixels, 960, 544, 32, 960 * 4,
                                                   0xff, 0xff00, 0xff0000, 0xff000000);
    if (!surface) return;
    GPU_Image *image = GPU_CopyImageFromSurface(surface);
    SDL_FreeSurface(surface);
    if (!image) return;
    GPU_SetBlending(image, false);
    GPU_BlitScale(image, nullptr, target, target->w / 2.0f, target->h / 2.0f,
                  target->w / 960.0f, target->h / 544.0f);
    GPU_Flip(target);
    handedOff = true;
    GPU_FreeImage(image);
    GPU_ClearRGBA(target, 0, 0, 0, 255);
    releaseStartupImage();
}
}
