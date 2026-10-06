/**
 * @file  gxm_wrap.c
 * @brief Crash-proofing for vitaGL's unchecked GXM default-uniform reserves.
 *
 * vitaGL (update_scissor_test, glClear paths) calls
 * sceGxmReserve{Vertex,Fragment}DefaultUniformBuffer without checking the
 * return value and with an uninitialized out-pointer. When the reserve
 * fails (sceGxmBeginScene failed on a fresh FBO -> not-within-scene, or
 * ring exhaustion), sceGxmSetUniformDataF then writes NDC floats through
 * stale stack garbage, which can smash a saved LR.
 *
 * These --wrap shims (see CMakeLists target_link_options) convert every
 * failure into a harmless write into a static sink and log the GXM error
 * code.
 */

#include <stdint.h>
#include <psp2/kernel/clib.h>
#include <psp2/gxm.h>

#include <vitaGL.h>

#include "utils/logger.h"

int __real_sceGxmReserveVertexDefaultUniformBuffer(void *ctx, void **buf);
int __real_sceGxmReserveFragmentDefaultUniformBuffer(void *ctx, void **buf);

/*
 * The engine's GL context is created by the native SDL2 Vita video driver,
 * which initializes vitaGL through vglInitExtended. That entry passes
 * phycont_threshold=0, so vitaGL's init-time pool takes all PHYCONT memory,
 * while SceAvPlayer's codec engine needs physically contiguous memory. The
 * loader's own gl_init() thresholds do not apply here: SDL2 initializes GL
 * before the engine's EGL shim path is reached.
 *
 * Wrap the SDL2-driven entry and route it through the thresholded init,
 * reserving PHYCONT for the hardware video decoder. SceAvPlayer requests
 * one 11,534,336-byte contiguous block for its frame pool on a 960x544
 * H.264 stream (reimpl/video.c allocateTexture); 14MB covers it with margin.
 */
#define PIVAS_PHYCONT_RESERVE_FOR_AVPLAYER (14 * 1024 * 1024)

/* vitaGL internal (source/utils/mem_utils.h) — the cdlg_threshold value
 * vglInitExtended itself passes; not exported in vitaGL.h. */
#define SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE 0x8C6000

GLboolean __wrap_vglInitExtended(int pool_size, int width, int height,
                                 int ram_threshold, SceGxmMultisampleMode msaa) {
    l_info("[gxm] vglInitExtended wrapped: pool=%d %dx%d ram_threshold=%d, "
           "reserving %d PHYCONT for SceAvPlayer",
           pool_size, width, height, ram_threshold,
           PIVAS_PHYCONT_RESERVE_FOR_AVPLAYER);
    return vglInitWithCustomThreshold(pool_size, width, height,
                                      ram_threshold,
                                      0,
                                      PIVAS_PHYCONT_RESERVE_FOR_AVPLAYER,
                                      SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE,
                                      msaa);
}

static __attribute__((aligned(64))) uint8_t s_gxm_reserve_sink[4096];
static unsigned int s_gxm_reserve_fail_logs;

#define GXM_RESERVE_FAIL_LOG_CAP 32

int __wrap_sceGxmReserveVertexDefaultUniformBuffer(void *ctx, void **buf) {
    int r = __real_sceGxmReserveVertexDefaultUniformBuffer(ctx, buf);
    if (r != 0) {
        *buf = s_gxm_reserve_sink;
        if (s_gxm_reserve_fail_logs++ < GXM_RESERVE_FAIL_LOG_CAP)
            l_warn("[gxm] ReserveVertexDefaultUniformBuffer failed 0x%08X",
                   r);
    }
    return r;
}

int __wrap_sceGxmReserveFragmentDefaultUniformBuffer(void *ctx, void **buf) {
    int r = __real_sceGxmReserveFragmentDefaultUniformBuffer(ctx, buf);
    if (r != 0) {
        *buf = s_gxm_reserve_sink;
        if (s_gxm_reserve_fail_logs++ < GXM_RESERVE_FAIL_LOG_CAP)
            l_warn("[gxm] ReserveFragmentDefaultUniformBuffer failed 0x%08X",
                   r);
    }
    return r;
}
