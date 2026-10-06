/*
 * SceAvPlayer bridge for the engine's PIVAS native video path.
 *
 * The Vita's codec engine demuxes and decodes the .mp4 internally (own
 * SceKernel threads, none of our pthread shims involved); we poll frames
 * from the engine main thread. Decoded frames are NV12 (Y plane, then one
 * interleaved CbCr plane, both with a 16-aligned stride) in physically
 * contiguous *uncached* memory that is also mapped for the GPU, so the
 * preferred display path (pivas_video_frame_to_texture) lets GXM sample a
 * frame in place as a YUV texture: no CPU copy, no CPU colour conversion.
 * The fallback CPU path (pivas_video_frame_to_cpu) copies a frame once via
 * sceDmacMemcpy into cached memory for sws conversion.
 */
#include "video.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <psp2/avplayer.h>
#include <psp2/gxm.h>
#include <psp2/kernel/dmac.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>

#include <vitaGL.h>

#include "utils/logger.h"

/* Open blocks only until the demuxer has parsed the stream header, not
 * until the first decoded frame, so the engine can black out the scene
 * right away. Frames arrive via poll. */
#define AVP_STREAMINFO_TIMEOUT_US (1500 * 1000)
/* IsActive can lag AddSource with autoStart; don't trust "inactive" as
 * "failed" until the player had time to spin up. */
#define AVP_ACTIVE_GRACE_US (1000 * 1000)
#define AVP_PROBE_STEP_US 2000
/* EOF backstop in case IsActive stays true past the stream end: no new
 * frames and a frozen playback clock for this long also count as "ended",
 * so the game always resumes. */
#define AVP_STALL_TIMEOUT_US (2500 * 1000)
/* Frames due at once that one poll may pull (keeping the newest). */
#define AVP_MAX_DRAIN 8

/*
 * Decoded-frame ring. Each extra buffer lets the decoder run one more frame
 * ahead (so one late poll can skip ahead instead of falling behind for the
 * rest of the clip) and keeps a buffer the GPU may still be sampling from
 * being overwritten. With 2 buffers the decoder asks for one 11,534,336-byte
 * contiguous pool on a 960x544 H.264 stream; each further buffer costs one
 * more 960x544 NV12 frame (783,360 bytes) before the 1MiB rounding, so 3
 * fits the 14MB PHYCONT reserve in gxm_wrap.c. If the pool allocation fails
 * anyway, the clip is reopened once with 2.
 */
#define AVP_OUT_BUFFERS 3
#define AVP_OUT_BUFFERS_FALLBACK 2

#define ALIGN16(x) (((x) + 15u) & ~15u)

static SceAvPlayerHandle s_handle = -1;
static int s_open;
static int s_out_buffers;
static int s_loop;
static char s_path[512];

/* Playback-pace telemetry: media-time vs wall-time deltas and poll cadence,
 * logged once per ~30 delivered frames, capped. */
static uint32_t s_stat_frames, s_stat_polls, s_stat_logs, s_stat_skipped;
static uint64_t s_stat_wall0_us;
static uint64_t s_stat_media0_ms;
static int s_ended;
static int s_module_loaded;
static int s_swap_uv;
static uint64_t s_last_progress_us;
static uint64_t s_last_player_time_ms;

/* Newest frame handed out by the decoder; its pData stays valid until the
 * decoder recycles that ring slot (AVP_OUT_BUFFERS - 1 frames later). */
static SceAvPlayerFrameInfo s_cur;
static int s_cur_valid;
static int s_unconsumed; /* a polled frame not yet reported to the engine */

/* CPU path staging (cached memory). */
static uint8_t *s_staging;
static size_t s_staging_size;

/* GPU path: the vitaGL texture whose GXM control words we overwrite. */
static SceGxmTexture *s_gxm_tex;
static SceGxmTexture s_gxm_saved;
static unsigned s_gl_tex;
static int s_tex_attached;

static int s_video_w, s_video_h;
static uint64_t s_duration_ms;

/* Decoder frame pool allocations: PHYCONT memblocks mapped for the GPU.
 * Tracked explicitly so the free callback unmaps exactly what we mapped. */
typedef struct {
    void *base;
    SceUID uid;
    uint32_t size;
} AvpTexBlock;
static AvpTexBlock s_tex_blocks[8];
static volatile int s_tex_alloc_failed;
static int s_reopened;

/*
 * General allocations: demux buffers, internal queues — regular heap.
 */
static void *avp_alloc(void *arg, uint32_t alignment, uint32_t size) {
    (void)arg;
    if (alignment < 16)
        alignment = 16;
    return memalign(alignment, size);
}

static void avp_free(void *arg, void *ptr) {
    (void)arg;
    free(ptr);
}

/*
 * Frame/texture allocations: the codec engine DMAs decoded pictures into
 * these, so they must be physically contiguous. PHYCONT blocks want their
 * size in 1MiB multiples and come 1MiB-aligned, satisfying the 0x100000
 * alignment SceAvPlayer asks for. The block is also mapped into the GPU's
 * address space so a frame can be sampled in place as a YUV texture.
 */
static void *avp_alloc_texture(void *arg, uint32_t alignment, uint32_t size) {
    (void)arg;
    (void)alignment;
    uint32_t block_size = (size + 0xFFFFFu) & ~0xFFFFFu;
    SceUID uid = sceKernelAllocMemBlock("avp_frame",
                                        SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,
                                        block_size, NULL);
    if (uid < 0) {
        l_error("[avp] PHYCONT alloc failed: 0x%08X (%u bytes, %d output buffers)",
                uid, block_size, s_out_buffers);
        s_tex_alloc_failed = 1;
        return NULL;
    }
    void *base = NULL;
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
        sceKernelFreeMemBlock(uid);
        s_tex_alloc_failed = 1;
        return NULL;
    }

    int mapped = sceGxmMapMemory(base, block_size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    if (mapped < 0) {
        /* Frames stay decodable; only the zero-copy texture path is off
         * (frame_to_texture refuses unmapped memory and the engine falls
         * back to the CPU path). */
        l_error("[avp] sceGxmMapMemory failed: 0x%08X (%u bytes)", mapped, block_size);
    }

    for (size_t i = 0; i < sizeof(s_tex_blocks) / sizeof(*s_tex_blocks); i++) {
        if (!s_tex_blocks[i].base) {
            s_tex_blocks[i].base = base;
            s_tex_blocks[i].uid  = uid;
            s_tex_blocks[i].size = mapped < 0 ? 0 : block_size;
            break;
        }
    }
    return base;
}

static void avp_free_texture(void *arg, void *ptr) {
    (void)arg;
    if (!ptr)
        return;
    for (size_t i = 0; i < sizeof(s_tex_blocks) / sizeof(*s_tex_blocks); i++) {
        if (s_tex_blocks[i].base == ptr) {
            if (s_tex_blocks[i].size)
                sceGxmUnmapMemory(ptr);
            sceKernelFreeMemBlock(s_tex_blocks[i].uid);
            memset(&s_tex_blocks[i], 0, sizeof(s_tex_blocks[i]));
            return;
        }
    }
    /* Not tracked (should not happen): free by address. */
    SceUID uid = sceKernelFindMemBlockByAddr(ptr, 0);
    if (uid >= 0)
        sceKernelFreeMemBlock(uid);
}

static int avp_ptr_is_gpu_mapped(const void *ptr) {
    for (size_t i = 0; i < sizeof(s_tex_blocks) / sizeof(*s_tex_blocks); i++) {
        const uint8_t *b = s_tex_blocks[i].base;
        if (b && s_tex_blocks[i].size &&
            (const uint8_t *)ptr >= b && (const uint8_t *)ptr < b + s_tex_blocks[i].size)
            return 1;
    }
    return 0;
}

/*
 * The engine hands over whatever completePath produced: relative to the
 * loader's chdir(DATA_PATH) cwd, possibly with script backslashes.
 * SceAvPlayer opens with raw sceIo (no cwd concept) — absolutize here.
 */
static void avp_normalize_path(const char *in, char *out, size_t out_size) {
    if (strchr(in, ':')) {
        snprintf(out, out_size, "%s", in);
    } else {
        while (in[0] == '.' && (in[1] == '/' || in[1] == '\\'))
            in += 2;
        while (in[0] == '/' || in[0] == '\\')
            in++;
        snprintf(out, out_size, DATA_PATH "%s", in);
    }
    for (char *p = out; *p; p++)
        if (*p == '\\')
            *p = '/';
}

static void avp_read_flags(void) {
    s_swap_uv = 0;
    FILE *f = fopen(DATA_PATH "pivas_flags.txt", "r");
    if (!f)
        return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "video_swap_uv=1", 15) == 0)
            s_swap_uv = 1;
    }
    fclose(f);
}

static void avp_drain_audio(void) {
    SceAvPlayerFrameInfo info;
    int guard = 0;
    while (guard++ < 8) {
        memset(&info, 0, sizeof(info));
        if (!sceAvPlayerGetAudioData(s_handle, &info))
            break;
    }
}

static int avp_ensure_staging(size_t size) {
    if (s_staging && s_staging_size >= size)
        return 0;
    free(s_staging);
    s_staging = memalign(64, size);
    s_staging_size = s_staging ? size : 0;
    if (!s_staging) {
        l_error("[avp] staging alloc failed (%u bytes)", (unsigned)size);
        return -1;
    }
    return 0;
}

static int avp_frame_usable(const SceAvPlayerFrameInfo *info) {
    int w = (int)info->details.video.width;
    int h = (int)info->details.video.height;
    return info->pData && w > 0 && h > 0 && !(w & 1) && !(h & 1);
}

/* Poll GetStreamInfo until the demuxer reports a video stream with real
 * dimensions. Returns 1 and fills s_video_w/h + s_duration_ms. */
static int avp_wait_stream_info(uint32_t *waited_us) {
    *waited_us = 0;
    while (*waited_us < AVP_STREAMINFO_TIMEOUT_US) {
        for (uint32_t id = 0; id < 3; id++) {
            SceAvPlayerStreamInfo info;
            memset(&info, 0, sizeof(info));
            if (sceAvPlayerGetStreamInfo(s_handle, id, &info) < 0)
                continue;
            if (info.type == SCE_AVPLAYER_VIDEO &&
                info.details.video.width > 0 && info.details.video.height > 0) {
                s_video_w = (int)info.details.video.width;
                s_video_h = (int)info.details.video.height;
                s_duration_ms = info.duration;
                return 1;
            }
        }
        if (*waited_us > AVP_ACTIVE_GRACE_US && !sceAvPlayerIsActive(s_handle))
            return 0;
        sceKernelDelayThread(AVP_PROBE_STEP_US);
        *waited_us += AVP_PROBE_STEP_US;
    }
    return 0;
}

static void avp_shutdown_player(void) {
    if (s_open) {
        if (s_tex_attached)
            pivas_video_texture_release();
        /* Drain submitted GPU work before the decoder's (GPU-mapped) frame
         * memory goes away in sceAvPlayerClose. The engine already ended
         * the scene that last sampled it. */
        glFinish();
        sceAvPlayerStop(s_handle);
        sceAvPlayerClose(s_handle);
        l_info("[avp] closed");
    }
    s_open = 0;
    s_ended = 0;
    s_handle = -1;
    s_cur_valid = 0;
    s_unconsumed = 0;
    s_tex_alloc_failed = 0;
    /* s_staging is kept for the next clip; avp_ensure_staging regrows it */
}

static int avp_start(const char *full_path, int out_buffers) {
    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.allocate = avp_alloc;
    init.memoryReplacement.deallocate = avp_free;
    init.memoryReplacement.allocateTexture = avp_alloc_texture;
    init.memoryReplacement.deallocateTexture = avp_free_texture;
    init.basePriority = 0xA0; /* below the engine main thread */
    init.numOutputVideoFrameBuffers = out_buffers;
    init.autoStart = 1;
    s_out_buffers = out_buffers;

    s_handle = sceAvPlayerInit(&init);
    /*
     * The handle is the player context pointer cast to int (it comes out
     * of avp_alloc). Heap addresses can sit above 0x80000000, so a sign
     * check would misread a successful init as failure. Failure is NULL
     * (or the -1 sentinel), not negative.
     */
    if (s_handle == 0 || s_handle == -1) {
        l_error("[avp] sceAvPlayerInit failed: 0x%08X", s_handle);
        s_handle = -1;
        return -3;
    }
    s_open = 1;

    int ret = sceAvPlayerAddSource(s_handle, full_path);
    if (ret < 0) {
        l_error("[avp] AddSource(%s) failed: 0x%08X", full_path, ret);
        avp_shutdown_player();
        return -4;
    }

    sceAvPlayerSetLooping(s_handle, s_loop ? 1 : 0);

    /*
     * Wait only for the parsed stream header (dimensions for the GPU
     * image sizing + duration for the waitvideo deadline). Decoded frames
     * arrive later through pivas_video_poll; the engine blacks the scene
     * out immediately and the stall backstop skips the clip if decode
     * never delivers.
     */
    uint32_t waited = 0;
    if (!avp_wait_stream_info(&waited)) {
        l_error("[avp] no video stream info from %s after %u ms (active=%d)",
                full_path, waited / 1000, (int)sceAvPlayerIsActive(s_handle));
        avp_shutdown_player();
        return -5;
    }

    s_last_progress_us = sceKernelGetProcessTimeWide();
    s_last_player_time_ms = sceAvPlayerCurrentTime(s_handle);
    s_stat_frames = s_stat_polls = s_stat_logs = s_stat_skipped = 0;

    l_info("[avp] playing %s: %dx%d, duration %u ms, %d output buffers, stream info after %u ms%s",
           full_path, s_video_w, s_video_h, (unsigned)s_duration_ms, out_buffers,
           waited / 1000, s_swap_uv ? ", swap_uv" : "");
    return 0;
}

int pivas_video_open(const char *path, int loop) {
    pivas_video_close();

    if (!path || !*path)
        return -1;

    avp_read_flags();

    if (!s_module_loaded) {
        int ret = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
        if (ret != SCE_SYSMODULE_LOADED) {
            l_error("[avp] sceSysmoduleLoadModule failed: 0x%08X", ret);
            return -2;
        }
        s_module_loaded = 1;
    }

    avp_normalize_path(path, s_path, sizeof(s_path));
    s_loop = loop;
    s_reopened = 0;

    return avp_start(s_path, AVP_OUT_BUFFERS);
}

int pivas_video_dimensions(int *width, int *height, uint64_t *duration_ms) {
    if (!s_open)
        return -1;
    if (width)
        *width = s_video_w;
    if (height)
        *height = s_video_h;
    if (duration_ms)
        *duration_ms = s_duration_ms;
    return 0;
}

/* The decoder could not get its frame pool with the preferred ring size:
 * restart the clip once with the smaller fallback ring (same path and
 * dimensions, so the engine does not notice). */
static int avp_retry_smaller_pool(void) {
    if (s_reopened || s_out_buffers <= AVP_OUT_BUFFERS_FALLBACK)
        return 0;
    l_warn("[avp] frame pool allocation failed with %d output buffers, reopening with %d",
           s_out_buffers, AVP_OUT_BUFFERS_FALLBACK);
    int w = s_video_w, h = s_video_h;
    uint64_t d = s_duration_ms;
    avp_shutdown_player();
    s_reopened = 1;
    if (avp_start(s_path, AVP_OUT_BUFFERS_FALLBACK) < 0)
        return 0;
    if (s_video_w != w || s_video_h != h)
        l_warn("[avp] reopened clip reports %dx%d (was %dx%d)", s_video_w, s_video_h, w, h);
    s_duration_ms = d ? d : s_duration_ms;
    return 1;
}

int pivas_video_poll(PivasVideoFrame *out) {
    if (!s_open || s_ended)
        return -1;

    if (s_tex_alloc_failed && !s_reopened) {
        if (!avp_retry_smaller_pool()) {
            s_ended = 1;
            return -1;
        }
    }

    avp_drain_audio();
    s_stat_polls++;

    /* Drain everything currently due and keep only the newest — if the
     * engine loop lagged, stale frames are dropped instead of queued. */
    int guard = 0, fresh = 0;
    while (guard++ < AVP_MAX_DRAIN) {
        SceAvPlayerFrameInfo info;
        memset(&info, 0, sizeof(info));
        if (!sceAvPlayerGetVideoData(s_handle, &info))
            break;
        if (!avp_frame_usable(&info))
            continue;
        if (fresh)
            s_stat_skipped++;
        s_cur = info;
        s_cur_valid = 1;
        s_unconsumed = 1;
        fresh = 1;
    }

    uint64_t now = sceKernelGetProcessTimeWide();
    if (fresh) {
        if (s_stat_frames == 0) {
            s_stat_wall0_us  = now;
            s_stat_media0_ms = s_cur.timeStamp;
            s_stat_polls     = 0;
        }
        s_stat_frames++;
        if (s_stat_frames % 30 == 0 && s_stat_logs < 16) {
            s_stat_logs++;
            uint64_t wall_ms  = (now - s_stat_wall0_us) / 1000;
            uint64_t media_ms = s_cur.timeStamp - s_stat_media0_ms;
            l_info("[avp] pace: %u frames (%u skipped), media %ums in wall %ums (%d%% speed, lag %dms), %u polls",
                   s_stat_frames, s_stat_skipped, (unsigned)media_ms, (unsigned)wall_ms,
                   wall_ms ? (int)((media_ms * 100) / wall_ms) : 0,
                   (int)((int64_t)wall_ms - (int64_t)media_ms),
                   s_stat_polls);
        }
        s_last_progress_us = now;
    } else {
        uint64_t player_ms = sceAvPlayerCurrentTime(s_handle);
        if (player_ms != s_last_player_time_ms) {
            s_last_player_time_ms = player_ms;
            s_last_progress_us    = now;
        }
    }

    if (s_unconsumed) {
        memset(out, 0, sizeof(*out));
        out->width        = (int)s_cur.details.video.width;
        out->height       = (int)s_cur.details.video.height;
        out->timestamp_ms = s_cur.timeStamp;
        s_unconsumed = 0;
        return 1;
    }

    if (!sceAvPlayerIsActive(s_handle)) {
        s_ended = 1;
        l_info("[avp] playback ended (inactive) at %u ms after %u frames",
               (unsigned)s_last_player_time_ms, s_stat_frames);
        return -1;
    }

    if (now - s_last_progress_us > AVP_STALL_TIMEOUT_US) {
        s_ended = 1;
        l_warn("[avp] no playback progress for %u ms (player at %u ms), treating as ended",
               (unsigned)((now - s_last_progress_us) / 1000), (unsigned)s_last_player_time_ms);
        return -1;
    }

    return 0;
}

int pivas_video_frame_to_texture(unsigned gl_texture) {
    if (!s_open || !s_cur_valid || !gl_texture)
        return -1;
    if (!avp_ptr_is_gpu_mapped(s_cur.pData))
        return -2;

    if (!s_tex_attached || s_gl_tex != gl_texture) {
        if (s_tex_attached)
            pivas_video_texture_release();
        /* vitaGL only exposes the GXM texture of the bound texture; keep
         * the binding the engine's renderer believes is current. */
        GLint prev = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glBindTexture(GL_TEXTURE_2D, (GLuint)gl_texture);
        SceGxmTexture *tex = vglGetGxmTexture(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
        if (!tex) {
            l_error("[avp] no GXM texture behind GL texture %u", gl_texture);
            return -3;
        }
        s_gxm_tex      = tex;
        s_gxm_saved    = *tex;
        s_gl_tex       = gl_texture;
        s_tex_attached = 1;
        l_info("[avp] GL texture %u now samples decoder frames directly", gl_texture);
    }

    /* Two-plane YUV420 with the BT.709 matrix (CSC1). video_swap_uv=1 in
     * pivas_flags.txt swaps the chroma order. */
    SceGxmTextureFormat fmt = s_swap_uv ? SCE_GXM_TEXTURE_FORMAT_YUV420P2_CSC1
                                        : SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1;
    int r = sceGxmTextureInitLinear(s_gxm_tex, s_cur.pData, fmt,
                                    s_cur.details.video.width, s_cur.details.video.height, 0);
    if (r < 0) {
        l_error("[avp] sceGxmTextureInitLinear failed: 0x%08X (%ux%u)", r,
                (unsigned)s_cur.details.video.width, (unsigned)s_cur.details.video.height);
        pivas_video_texture_release();
        return -4;
    }
    sceGxmTextureSetMinFilter(s_gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(s_gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMipFilter(s_gxm_tex, SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
    sceGxmTextureSetUAddrMode(s_gxm_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(s_gxm_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
    return 0;
}

void pivas_video_texture_release(void) {
    if (s_tex_attached && s_gxm_tex)
        *s_gxm_tex = s_gxm_saved;
    s_tex_attached = 0;
    s_gxm_tex = NULL;
    s_gl_tex = 0;
}

int pivas_video_frame_to_cpu(PivasVideoFrame *out) {
    if (!s_open || !s_cur_valid)
        return -1;

    uint32_t w = s_cur.details.video.width;
    uint32_t h = s_cur.details.video.height;
    uint32_t stride = ALIGN16(w);
    uint32_t rows   = ALIGN16(h);
    size_t y_size   = (size_t)stride * rows;
    size_t total    = y_size + y_size / 2;
    if (avp_ensure_staging(total) < 0)
        return -2;

    /* One copy out of uncached decoder memory; the DMA engine handles the
     * cache maintenance and does not stall the CPU like byte reads do. */
    if (sceDmacMemcpy(s_staging, s_cur.pData, total) < 0)
        memcpy(s_staging, s_cur.pData, total);

    memset(out, 0, sizeof(*out));
    out->planes[0]   = s_staging;
    out->planes[1]   = s_staging + y_size;
    out->planes[2]   = NULL;
    out->linesize[0] = (int)stride;
    out->linesize[1] = (int)stride;
    out->linesize[2] = 0;
    out->width       = (int)w;
    out->height      = (int)h;
    out->timestamp_ms = s_cur.timeStamp;
    return 0;
}

void pivas_video_set_looping(int loop) {
    s_loop = loop;
    if (s_open)
        sceAvPlayerSetLooping(s_handle, loop ? 1 : 0);
}

void pivas_video_close(void) {
    avp_shutdown_player();
}
