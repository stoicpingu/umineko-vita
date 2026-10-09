#include "media.hpp"
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/dmac.h>
#include <vitaGL.h>
#include <SDL2/SDL_gpu.h>
#include <cstdio>
#include <cstring>
#include "platform.hpp"
extern "C" {
#include "gpu_render_targets.h"
}
#include <malloc.h>
#include <cstdlib>
extern "C" {
#include <libswscale/swscale.h>
}

void *VitaMediaPlayer::allocate(void *, uint32_t alignment, uint32_t size) {
    return memalign(alignment < 16 ? 16 : alignment, size);
}
void VitaMediaPlayer::release(void *, void *ptr) { free(ptr); }
void *VitaMediaPlayer::allocateFrame(void *opaque, uint32_t, uint32_t size) {
    auto &self = *static_cast<VitaMediaPlayer *>(opaque);
    const uint32_t rounded = (size + 0xfffff) & ~0xfffffu;
    int uid = sceKernelAllocMemBlock("ons_video", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, rounded, nullptr);
    if (uid < 0) return nullptr;
    void *base = nullptr;
    if (sceKernelGetMemBlockBase(uid, &base) < 0) { sceKernelFreeMemBlock(uid); return nullptr; }
    bool mapped = sceGxmMapMemory(base, rounded, static_cast<SceGxmMemoryAttribFlags>(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)) >= 0;
    SDL_LockMutex(self.memoryMutex);
    for (auto &block : self.blocks) {
        if (!block.base) {
            block.base = base;
            block.uid = uid;
            block.size = rounded;
            block.mapped = mapped;
            SDL_UnlockMutex(self.memoryMutex);
            return base;
        }
    }
    SDL_UnlockMutex(self.memoryMutex);
    if (mapped) sceGxmUnmapMemory(base);
    sceKernelFreeMemBlock(uid);
    return nullptr;
}
void VitaMediaPlayer::releaseFrame(void *opaque, void *ptr) {
    auto &self = *static_cast<VitaMediaPlayer *>(opaque);
    SDL_LockMutex(self.memoryMutex);
    for (auto &block : self.blocks) {
        if (block.base == ptr && ptr) {
            if (block.mapped) sceGxmUnmapMemory(block.base);
            sceKernelFreeMemBlock(block.uid);
            block = {};
            break;
        }
    }
    SDL_UnlockMutex(self.memoryMutex);
}
bool VitaMediaPlayer::open(const char *path, uint64_t durationMs) {
    close();
    char flagsPath[512];
    std::snprintf(flagsPath, sizeof(flagsPath), "%spivas_flags.txt", VitaPlatform::gameRoot());
    if (FILE *flags = std::fopen(flagsPath, "r")) {
        char line[128];
        while (std::fgets(line, sizeof(line), flags))
            if (!std::strncmp(line, "video_swap_uv=1", 15)) swapUV = true;
        std::fclose(flags);
    }
    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_AVPLAYER) != SCE_SYSMODULE_LOADED &&
        sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER) < 0) return false;
    memoryMutex = SDL_CreateMutex();
    if (!memoryMutex) return false;
    SceAvPlayerInitData init{};
    init.memoryReplacement.objectPointer = this;
    init.memoryReplacement.allocate = allocate;
    init.memoryReplacement.deallocate = release;
    init.memoryReplacement.allocateTexture = allocateFrame;
    init.memoryReplacement.deallocateTexture = releaseFrame;
    init.basePriority = 0xa0;
    init.numOutputVideoFrameBuffers = 3;
    // Both working Vita players use AvPlayer's asynchronous auto-start.
    // AddSource is deferred until start(), after GPU/subtitle setup, so its
    // playback clock cannot run while the engine is still preparing the movie.
    init.autoStart = true;
    handle = sceAvPlayerInit(&init);
    // A valid AvPlayer handle can be a pointer with its sign bit set.
    if (!handle || handle == -1 || (static_cast<uint32_t>(handle) & 0xffff0000u) == 0x806a0000u) {
        handle = 0; close(); return false;
    }
    source = path;
    expectedDuration = durationMs;
    return true;
}
bool VitaMediaPlayer::start(bool loop) {
    if (!handle) return false;
    if (started) return true;
    began = lastProgress = SDL_GetTicks();
    started = sceAvPlayerAddSource(handle, source.c_str()) >= 0;
    if (!started) failure = "AvPlayer could not open the video source";
    else sceAvPlayerSetLooping(handle, loop);
    return started;
}
bool VitaMediaPlayer::readFrame(Frame &frame, bool &eof) {
    finishFrameUse();
    eof = false;
    // loadPresentation may poll an uncommitted layer before startProcessing.
    // That is not an instruction to start (or finish) the decoder.
    if (!started) { eof = failure != nullptr; return false; }
    SceAvPlayerFrameInfo info{};
    // FFmpeg decodes the selected audio stream into the engine mixer. Drain
    // AvPlayer's own PCM to avoid blocking its internal demultiplexer.
    for (int i = 0; i < 16 && sceAvPlayerGetAudioData(handle, &info); ++i) {}
    bool fresh = false;
    // AvPlayer owns pacing. If rendering fell behind, discard already-due
    // pictures rather than playing a growing queue in slow motion.
    for (unsigned i = 0; i < 8 && sceAvPlayerGetVideoData(handle, &info); ++i) {
        const unsigned width = info.details.video.width, height = info.details.video.height;
        const size_t bytes = ((width + 15u) & ~15u) * size_t(height) * 3 / 2;
        if (!width || !height || width > 1920 || height > 1088 || (width & 1) || (height & 1) ||
            !frameInBlock(info.pData, bytes, false)) {
            failure = "AvPlayer returned an invalid video frame"; eof = true; return false;
        }
        current = info;
        fresh = true;
    }
    if (fresh) {
        frame = {int(current.details.video.width), int(current.details.video.height), current.timeStamp};
        receivedFrame = true;
        lastProgress = SDL_GetTicks();
        return true;
    }
    uint32_t now = SDL_GetTicks();
    // Inactive during asynchronous preparation/buffering is not EOF. In
    // particular, neither one second of wall time nor one decoded frame means
    // an opening has ended. Check the known stream duration before accepting
    // an inactive decoder as a normal end; retain the bounded stall failure.
    const bool atEnd = receivedFrame && (!expectedDuration || current.timeStamp + 250 >= expectedDuration);
    eof = atEnd && now - lastProgress >= 100 && !sceAvPlayerIsActive(handle);
    if (!eof && now - lastProgress > 5000) {
        failure = receivedFrame ? "AvPlayer stopped progressing before the end of the movie"
                                : "AvPlayer startup timed out without a decoded frame";
        eof = true;
    }
    return false;
}
bool VitaMediaPlayer::frameInBlock(const void *pixels, size_t bytes, bool requireMapped) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(pixels);
    bool valid = false;
    SDL_LockMutex(memoryMutex);
    for (const auto &block : blocks) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(block.base);
        if (base && address >= base && address - base <= block.size &&
            bytes <= block.size - (address - base) && (!requireMapped || block.mapped)) {
            valid = true; break;
        }
    }
    SDL_UnlockMutex(memoryMutex);
    return valid;
}

void VitaMediaPlayer::finishFrameUse() {
    if (!gpuReadPending) return;
    // Flush/submit before the decoder can recycle a sampled frame, or close
    // can unmap it. glFinish alone does not submit vitaGL's open GXM scene.
    GPU_FlushBlitBuffer();
    ons_vita_finish_readback();
    gpuReadPending = false;
}

void VitaMediaPlayer::releaseTexture() {
    finishFrameUse();
    if (texture) *texture = savedTexture;
    texture = nullptr;
    textureId = 0;
}

bool VitaMediaPlayer::bindFrame(unsigned id) {
    if (!receivedFrame || !id) return false;
    const unsigned width = current.details.video.width, height = current.details.video.height;
    if (!frameInBlock(current.pData, ((width + 15u) & ~15u) * size_t(height) * 3 / 2, true)) return false;
    if (textureId != id) {
        releaseTexture();
        GPU_FlushBlitBuffer();
        GLint active = 0, previous = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
        glBindTexture(GL_TEXTURE_2D, id);
        texture = vglGetGxmTexture(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, previous);
        glActiveTexture(active);
        if (!texture) return false;
        savedTexture = *texture;
        textureId = id;
    }
    const auto format = swapUV ? SCE_GXM_TEXTURE_FORMAT_YUV420P2_CSC1 : SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1;
    if (sceGxmTextureInitLinear(texture, current.pData, format, width, height, 0) < 0) {
        releaseTexture(); return false;
    }
    sceGxmTextureSetMinFilter(texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMipFilter(texture, SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
    sceGxmTextureSetUAddrMode(texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
    gpuReadPending = true;
    return true;
}

bool VitaMediaPlayer::convertFrame(SDL_Surface *surface) {
    if (!receivedFrame || !surface) return false;
    const int width = current.details.video.width, height = current.details.video.height;
    const int stride = (width + 15) & ~15;
    staging.resize(size_t(stride) * height * 3 / 2);
    // CPU fallbacks must not repeatedly read PHYCONT uncached decoder memory.
    if (sceDmacMemcpy(staging.data(), current.pData, staging.size()) < 0)
        std::memcpy(staging.data(), current.pData, staging.size());
    if (swapUV)
        for (size_t i = size_t(stride) * height; i + 1 < staging.size(); i += 2)
            std::swap(staging[i], staging[i + 1]);
    scaler = sws_getCachedContext(scaler, width, height, AV_PIX_FMT_NV12,
                                 surface->w, surface->h, AV_PIX_FMT_RGB24,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!scaler) { failure = "Could not allocate native video color converter"; return false; }
    sws_setColorspaceDetails(scaler, sws_getCoefficients(SWS_CS_ITU709), 0,
                            sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);
    const uint8_t *planes[4] = {staging.data(), staging.data() + stride * height, nullptr, nullptr};
    int strides[4] = {stride, stride, 0, 0};
    uint8_t *output[4] = {static_cast<uint8_t *>(surface->pixels), nullptr, nullptr, nullptr};
    int outputStrides[4] = {surface->pitch, 0, 0, 0};
    sws_scale(scaler, planes, strides, 0, height, output, outputStrides);
    return true;
}

void VitaMediaPlayer::close() {
    releaseTexture();
    if (handle) { sceAvPlayerStop(handle); sceAvPlayerClose(handle); handle = 0; }
    for (auto &block : blocks) {
        if (block.base) {
            if (block.mapped) sceGxmUnmapMemory(block.base);
            sceKernelFreeMemBlock(block.uid); block = {};
        }
    }
    if (memoryMutex) { SDL_DestroyMutex(memoryMutex); memoryMutex = nullptr; }
    if (scaler) { sws_freeContext(scaler); scaler = nullptr; }
    started = receivedFrame = swapUV = false;
    current = {};
    expectedDuration = 0;
    source.clear();
    staging.clear();
    failure = nullptr;
}
