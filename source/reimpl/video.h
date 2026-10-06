/*
 * Native hardware video playback bridge (SceAvPlayer).
 *
 * Exported to the engine through the dynlib table; the engine's PIVAS
 * MediaLayer branch (Engine/Layers/Media.cpp) drives it for .mp4 clips
 * instead of the ffmpeg demux/decode thread pipeline.
 *
 * Frame flow per engine frame:
 *   pivas_video_poll()             -> 1 when a new decoded frame is current
 *   pivas_video_frame_to_texture() -> GPU path: the GL texture samples the
 *                                     decoder's NV12 buffer directly (no copy,
 *                                     GXM does the YUV->RGB conversion)
 *   pivas_video_frame_to_cpu()     -> CPU path: NV12 planes in cached memory
 *                                     for sws conversion (fallback)
 */
#ifndef SOLOADER_VIDEO_H
#define SOLOADER_VIDEO_H

#include <stdint.h>

/*
 * One decoded frame as SceAvPlayer delivers it: NV12, i.e. a full-size Y
 * plane followed by one interleaved CbCr plane at half height. Both planes
 * share linesize (width rounded up to 16).
 *
 * After pivas_video_poll() only width/height/timestamp_ms are filled; the
 * plane pointers are set by pivas_video_frame_to_cpu() and then reference
 * the bridge's cached staging buffer (valid until the next poll or close).
 */
typedef struct PivasVideoFrame {
    const uint8_t *planes[3]; /* Y, interleaved UV, unused */
    int linesize[3];
    int width;
    int height;
    uint64_t timestamp_ms;
} PivasVideoFrame;

/*
 * Open `path` (engine-relative or absolute) and start playback. Blocks only
 * until the container's stream header is parsed (dimensions, duration).
 * Returns 0 on success, <0 on any failure.
 */
int pivas_video_open(const char *path, int loop);

/* Facts cached at open. Any out-pointer may be NULL. Returns 0, or <0 if
 * no video is open. duration_ms is 0 when the container did not say. */
int pivas_video_dimensions(int *width, int *height, uint64_t *duration_ms);

/* 1 = a new frame is current (see the frame_to_* calls); 0 = no new frame
 * yet (keep showing the previous one); -1 = playback ended (or nothing
 * open). Drains everything currently due and keeps only the newest frame,
 * so a late consumer skips frames instead of falling behind. Also drains
 * and discards any audio frames so an audio-bearing file cannot wedge the
 * decoder. */
int pivas_video_poll(PivasVideoFrame *out);

/*
 * GPU path. Re-points the GXM texture behind `gl_texture` (a vitaGL
 * texture the engine owns) at the current frame's decoder buffer as a
 * YUV420 two-plane texture; sampling it yields RGB (BT.709). The first
 * call saves the texture's original control words; they are restored by
 * pivas_video_texture_release() or pivas_video_close(), after which the
 * texture is an ordinary texture again and may be deleted normally.
 * Returns 0 on success, <0 on failure (fall back to the CPU path).
 */
int pivas_video_frame_to_texture(unsigned gl_texture);
void pivas_video_texture_release(void);

/* CPU path. Copies the current frame once (DMA) out of the decoder's
 * uncached memory into cached staging and fills *out with NV12 plane
 * pointers. Returns 0 on success, <0 on failure. */
int pivas_video_frame_to_cpu(PivasVideoFrame *out);

void pivas_video_set_looping(int loop);

/* Stop and tear down. Idempotent. The caller must make sure no GPU work
 * that samples the frame texture is still pending (end the scene and
 * glFinish) before this frees the decoder's memory. */
void pivas_video_close(void);

#endif
