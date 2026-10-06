/**
 *  Media.cpp
 *  ONScripter-RU
 *
 *  Video playback layer based on ffmpeg and MediaEngine.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Layers/Media.hpp"
#include "Engine/Components/Async.hpp"
#include "Engine/Graphics/GPU.hpp"
#include "Engine/Core/ONScripter.hpp"
#include "Support/FileDefs.hpp"

#if defined(PIVAS)
#include <cstdio>
#include <cstring>

// Capped diagnostics for the video pipeline: each trace point logs only its
// first few occurrences.
#define PIVAS_MEDIA_TRACE 0

#if PIVAS_MEDIA_TRACE
#define MEDIA_TRACE_CAPPED(counter, cap, ...)        \
	do {                                             \
		static int counter = 0;                      \
		if (counter++ < (cap))                       \
			sendToLog(LogLevel::Info, __VA_ARGS__); \
	} while (0)
#else
#define MEDIA_TRACE_CAPPED(counter, cap, ...)
#endif

// Replaces the mergeAlpha.frag render-to-texture pass for alpha-masked
// videos when frames arrive sws-converted (hwconvert=off). The RGB24
// surface holds colour in the top half and the greyscale mask in the
// bottom half; compose them into one premultiplied RGBA frame on the CPU
// and upload directly (GPU_UpdateImage, not gpu.updateImage, because the
// pixels are already premultiplied here).
static bool composeMaskedVideoFrameCPU(GPU_Image *frame, SDL_Surface *src, int w, int h) {
	if (!frame || !src || src->format->BytesPerPixel != 3 || src->w < w || src->h < h * 2)
		return false;

	static SDL_Surface *compose = nullptr;
	if (compose && (compose->w != w || compose->h != h)) {
		SDL_FreeSurface(compose);
		compose = nullptr;
	}
	if (!compose) {
		compose = SDL_CreateRGBSurface(SDL_SWSURFACE, w, h, 32,
		                               0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
		if (!compose)
			return false;
	}

	for (int y = 0; y < h; y++) {
		auto *colour = static_cast<uint8_t *>(src->pixels) + y * src->pitch;
		auto *mask   = static_cast<uint8_t *>(src->pixels) + (y + h) * src->pitch;
		auto *dst    = static_cast<uint8_t *>(compose->pixels) + y * compose->pitch;
		for (int x = 0; x < w; x++) {
			uint8_t a = mask[x * 3]; // greyscale mask, take one channel
			dst[x * 4 + 0] = static_cast<uint8_t>((static_cast<uint16_t>(colour[x * 3 + 0]) * a + 127) / 255);
			dst[x * 4 + 1] = static_cast<uint8_t>((static_cast<uint16_t>(colour[x * 3 + 1]) * a + 127) / 255);
			dst[x * 4 + 2] = static_cast<uint8_t>((static_cast<uint16_t>(colour[x * 3 + 2]) * a + 127) / 255);
			dst[x * 4 + 3] = a;
		}
	}

	GPU_UpdateImage(frame, nullptr, compose, nullptr);
	return true;
}

// ---- Native SceAvPlayer path -------------------------------------------
// The loader exports a minimal bridge around the Vita's hardware decoder
// (soloader source/reimpl/video.c, resolved like every other import).
// .mp4 clips play through it instead of the ffmpeg demux/decode thread
// pipeline; the MediaLayer lifecycle (videoState, frame_gpu, commit/
// refresh, waitvideo/skip/deadline) is unchanged.

// Must stay layout-identical to the loader's PivasVideoFrame
// (source/reimpl/video.h). SceAvPlayer frames are NV12: a Y plane and one
// interleaved CbCr plane, both with a 16-aligned stride. After poll only
// width/height/timestamp are set; pivas_video_frame_to_cpu fills the plane
// pointers (bridge staging buffer, valid until the next poll/close).
struct PivasVideoFrame {
	const uint8_t *planes[3]; // Y, interleaved UV, unused
	int linesize[3];
	int width;
	int height;
	uint64_t timestamp_ms;
};

// Weak imports: the .so links with --no-undefined, and these have no NDK
// stub library — the loader's dynlib table provides them at load time.
extern "C" {
__attribute__((weak)) int pivas_video_open(const char *path, int loop);
__attribute__((weak)) int pivas_video_dimensions(int *width, int *height, uint64_t *duration_ms);
__attribute__((weak)) int pivas_video_poll(PivasVideoFrame *out);
__attribute__((weak)) int pivas_video_frame_to_texture(unsigned gl_texture);
__attribute__((weak)) void pivas_video_texture_release(void);
__attribute__((weak)) int pivas_video_frame_to_cpu(PivasVideoFrame *out);
__attribute__((weak)) void pivas_video_set_looping(int loop);
__attribute__((weak)) void pivas_video_close(void);
}

// Kill-switches in pivas_flags.txt (cwd), read once:
//   native_video=off  every clip plays through the ffmpeg pipeline
//   video_gpu=off     native clips converted on the CPU (sws) instead of
//                     sampled in place by the GPU
static void pivasReadVideoFlags(bool &native, bool &gpu) {
	static int cachedNative = -1, cachedGpu = -1;
	if (cachedNative < 0) {
		cachedNative = 1;
		cachedGpu    = 1;
		FILE *f = std::fopen("pivas_flags.txt", "r");
		if (f) {
			char line[128];
			while (std::fgets(line, sizeof(line), f)) {
				if (std::strncmp(line, "native_video=off", 16) == 0)
					cachedNative = 0;
				if (std::strncmp(line, "video_gpu=off", 13) == 0)
					cachedGpu = 0;
			}
			std::fclose(f);
		}
		sendToLog(LogLevel::Info, "[media] native video: %s, gpu path: %s\n",
		          cachedNative ? "enabled" : "disabled (pivas_flags.txt)",
		          cachedGpu ? "enabled" : "disabled (pivas_flags.txt)");
	}
	native = cachedNative != 0;
	gpu    = cachedGpu != 0;
}

static bool pivasNativeVideoEnabled() {
	bool native, gpu;
	pivasReadVideoFlags(native, gpu);
	return native;
}

static bool pivasNativeGpuEnabled() {
	bool native, gpu;
	pivasReadVideoFlags(native, gpu);
	return gpu && pivas_video_frame_to_texture && pivas_video_frame_to_cpu;
}

// SceAvPlayer only demuxes MP4 (the masked animated-BG loops are MPEG-2
// and must stay on the ffmpeg path).
static bool pivasPathIsMp4(const char *path) {
	size_t len = std::strlen(path);
	if (len < 5)
		return false;
	auto lower    = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
	const char *e = path + len - 4;
	return e[0] == '.' && lower(e[1]) == 'm' && lower(e[2]) == 'p' && e[3] == '4';
}

// Mirrors MediaProcController::frameSize with the native clip's dimensions
// (plus defined behavior for the tall-masked-container corner upstream
// leaves unset).
void MediaLayer::pivasNativeFrameSize(int videoWidth, int videoHeight, int &frameWidth, int &frameHeight, bool alphaMasked) {
	if (videoWidth < scaleRect.w) {
		frameWidth = videoWidth;
		wFactor    = scaleRect.w / static_cast<float>(videoWidth);
	} else {
		frameWidth = scaleRect.w;
		wFactor    = 1;
	}

	if (videoHeight < scaleRect.h) {
		if (!alphaMasked) {
			frameHeight = videoHeight;
			hFactor     = scaleRect.h / static_cast<float>(videoHeight);
		} else if (videoHeight / 2 < scaleRect.h) {
			frameHeight = videoHeight / 2;
			hFactor     = 2.0f * scaleRect.h / videoHeight;
		} else {
			frameHeight = scaleRect.h;
			hFactor     = 1;
		}
	} else {
		frameHeight = scaleRect.h;
		hFactor     = 1;
	}
}

// A new decoded frame is current in the bridge: get it into the display
// frame image. Preferred: the GPU samples the decoder's NV12 buffer in
// place through pivasYuvImage (a 16x16 placeholder whose GXM texture the
// loader re-points each frame; GXM converts YUV->RGB while sampling) with
// one blit into the frame's render target, so no CPU copy or conversion is
// needed and the frame image stays an ordinary RGBA texture. Masked clips
// and any failure use the CPU path for the rest of the clip.
void MediaLayer::pivasPresentNativeFrame(GPU_Image *frame, PivasVideoFrame &nf) {
	if (pivasNativeGpu && pivasYuvImage && !mask_gpu) {
		GPU_TextureHandle handle = GPU_GetTextureHandle(pivasYuvImage);
		int res = handle ? pivas_video_frame_to_texture(static_cast<unsigned>(handle)) : -100;
		if (res == 0) {
			MEDIA_TRACE_CAPPED(native_gpu_trace, 8, "[media] native frame %dx%d ts=%llu sampled by GPU\n",
			                   nf.width, nf.height, static_cast<unsigned long long>(nf.timestamp_ms));
			GPU_GetTarget(frame);
			if (!frame->target)
				return;
			GPU_SetBlending(pivasYuvImage, false);
			gpu.copyGPUImage(pivasYuvImage, nullptr, nullptr, frame->target,
			                 frame->w / 2.0f, frame->h / 2.0f,
			                 frame->w / static_cast<float>(pivasYuvImage->w),
			                 frame->h / static_cast<float>(pivasYuvImage->h), 0, true);
			GPU_FlushBlitBuffer();
			return;
		}
		sendToLog(LogLevel::Warn, "[media] native GPU frame path failed (%d), converting on the CPU from now on\n", res);
		pivasNativeGpu = false;
	}

	if (pivas_video_frame_to_cpu(&nf) == 0)
		pivasUploadNativeFrame(frame, nf);
}

// Ends the native session: the last blit may still be queued or running on
// the GPU while it samples decoder memory that pivas_video_close frees, so
// drain the GPU first. Safe to call repeatedly.
void MediaLayer::pivasReleaseNative() {
	if (pivasYuvImage) // the GPU path was set up: a blit may have been issued
		gpu.finishRenderingForReadback();
	pivas_video_close(); // also restores pivasYuvImage's own texture words
	if (pivasYuvImage) {
		GPU_FreeImage(pivasYuvImage);
		pivasYuvImage = nullptr;
	}
	pivasNativeGpu = false;
}

// CPU path: sws converts the NV12 frame straight to RGBA (BT.709) into a
// persistent surface, uploaded through the standard surface path every
// sprite/BG uses. Masked clips compose the stacked colour/mask halves on
// the CPU like the ffmpeg sws path does.
void MediaLayer::pivasUploadNativeFrame(GPU_Image *frame, const PivasVideoFrame &nf) {
	const bool masked = mask_gpu != nullptr;
	if (!nf.planes[0] || !nf.planes[1])
		return;

	MEDIA_TRACE_CAPPED(native_sws_trace, 8, "[media] native frame %dx%d ts=%llu via sws surface masked=%d\n",
	                   nf.width, nf.height, static_cast<unsigned long long>(nf.timestamp_ms), masked);

	const int dstW = videoRect.w;
	const int dstH = masked ? videoRect.h * 2 : videoRect.h;

	static SwsContext *swsCtx = nullptr;
	static SDL_Surface *conv  = nullptr;
	static int cachedSrcW, cachedSrcH, cachedDstW, cachedDstH;
	if (swsCtx && (cachedSrcW != nf.width || cachedSrcH != nf.height || cachedDstW != dstW || cachedDstH != dstH)) {
		sws_freeContext(swsCtx);
		swsCtx = nullptr;
		SDL_FreeSurface(conv);
		conv = nullptr;
	}
	if (!conv) {
		// RGBA straight from sws: the frame image is 4-byte on the native
		// path, so the upload passes through the loader untouched instead
		// of paying an RGB24->RGBA expansion (malloc + full-frame pass)
		// per video frame.
		conv = SDL_CreateRGBSurface(SDL_SWSURFACE, dstW, dstH, 32,
		                            0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
		if (!conv)
			return;
	}
	if (!swsCtx) {
		swsCtx = sws_getContext(nf.width, nf.height, AV_PIX_FMT_NV12,
		                        dstW, dstH, AV_PIX_FMT_RGBA, SWS_BICUBIC,
		                        nullptr, nullptr, nullptr);
		if (!swsCtx)
			return;
		// Same BT.709 source-matrix enforcement as VideoDecoder::initSwsContext.
		int *inv_table, *table, srcRange, dstRange, brightness, contrast, saturation;
		if (!sws_getColorspaceDetails(swsCtx, &inv_table, &srcRange, &table, &dstRange,
		                              &brightness, &contrast, &saturation)) {
			sws_setColorspaceDetails(swsCtx,
			                         sws_getCoefficients(SWS_CS_ITU709), 0,
			                         sws_getCoefficients(SWS_CS_ITU709), 0,
			                         brightness, contrast, saturation);
		}
		cachedSrcW = nf.width;
		cachedSrcH = nf.height;
		cachedDstW = dstW;
		cachedDstH = dstH;
	}

	const uint8_t *srcPlanes[4]{nf.planes[0], nf.planes[1], nullptr, nullptr};
	int srcStrides[4]{nf.linesize[0], nf.linesize[1], 0, 0};
	uint8_t *dstPlanes[4]{static_cast<uint8_t *>(conv->pixels), nullptr, nullptr, nullptr};
	int dstStrides[4]{conv->pitch, 0, 0, 0};

	// Per-frame cost breakdown, logged a limited number of times: sws
	// conversion vs GPU upload vs everything else in the loop.
	static uint32_t perf_frames = 0, perf_logs = 0;
	static uint64_t perf_sws_us = 0, perf_upload_us = 0, perf_last_end_us = 0, perf_between_us = 0;
	uint64_t t0 = SDL_GetPerformanceCounter();
	if (perf_last_end_us)
		perf_between_us += t0 - perf_last_end_us;

	sws_scale(swsCtx, srcPlanes, srcStrides, 0, nf.height, dstPlanes, dstStrides);
	uint64_t t1 = SDL_GetPerformanceCounter();

	if (masked) {
		if (!composeMaskedVideoFrameCPU(frame, conv, videoRect.w, videoRect.h)) {
			MEDIA_TRACE_CAPPED(native_mask_fail_trace, 4, "[media] native CPU mask compose failed, using mergeAlpha shader\n");
			GPU_Rect maskRect = videoRect;
			maskRect.y += maskRect.h;
			gpu.mergeAlpha(frame, &videoRect, mask_gpu, &maskRect, conv);
		}
	} else {
		// Straight upload into the display frame. Video frames are opaque
		// (sws fills alpha=255), so gpu.updateImage's premultiply staging
		// pass would be an identity transform costing a full-frame CPU
		// pass.
		GPU_UpdateImage(frame, nullptr, conv, nullptr);
	}

	uint64_t t2 = SDL_GetPerformanceCounter();
	uint64_t pf = SDL_GetPerformanceFrequency();
	perf_sws_us += (t1 - t0) * 1000000 / pf;
	perf_upload_us += (t2 - t1) * 1000000 / pf;
	perf_last_end_us = t2;
	perf_frames++;
	if (perf_frames % 30 == 0 && perf_logs < 16) {
		perf_logs++;
		sendToLog(LogLevel::Info,
		          "[media] frame cost avg over %u: sws %ums, upload %ums, between-frames %ums\n",
		          perf_frames,
		          (unsigned)(perf_sws_us / perf_frames / 1000),
		          (unsigned)(perf_upload_us / perf_frames / 1000),
		          (unsigned)(perf_between_us * 1000000 / pf / perf_frames / 1000));
	}
}
#endif

MediaLayer::MediaLayer(int w, int h, BaseReader **br) {
	reader = br;
	width  = w;
	height = h;
}

MediaLayer::~MediaLayer() {
	while (!stopPlayback()) {
		// This should not happen in a proper script
		sendToLog(LogLevel::Error, "You forgot to stop video playback before exiting\n");
	}
}

bool MediaLayer::loadVideo(std::string &filename, unsigned audioStream, unsigned subtitleStream) {

	// If we arrived here we are guaranteed to be not playing anything
	// However, we may still display some frame of the previous video
	//videoState &= ~VS_END_OF_FILE;
	videoState = VS_OFFLINE;

	//Secondly, try to open the video
	std::unique_ptr<char[]> video_file((*reader)->completePath(filename.c_str(), FileType::File));
#if defined(PIVAS)
	MEDIA_TRACE_CAPPED(load_trace, 16, "[media] loadVideo %s audio=%u sub=%u\n",
	                   video_file ? video_file.get() : filename.c_str(), audioStream, subtitleStream);

	pivasNativeActive = false;
	if (video_file && pivasNativeVideoEnabled() && pivasPathIsMp4(video_file.get())) {
		// The loop flag only arrives at loadPresentation; open unlooped and
		// let the native loadPresentation branch update it.
		if (pivas_video_open(video_file.get(), 0) == 0) {
			pivasNativeActive = true;
			MEDIA_TRACE_CAPPED(native_open_trace, 16, "[media] native SceAvPlayer path engaged for %s\n", video_file.get());
			return true;
		}
		// No fallback to the ffmpeg pipeline: on Vita it hangs on these clips
		// (black screen, media.finish never completes, the waitvideo deadline
		// cannot recover it). Failing here makes movieCommand skip the clip
		// and the game continue. "native_video=off" in pivas_flags.txt still
		// forces mp4s onto ffmpeg.
		sendToLog(LogLevel::Warn, "[media] native video open failed for %s — skipping clip (ffmpeg fallback disabled)\n",
		          video_file.get());
		return false;
	}
#endif
	return media.loadVideo(video_file.get(), audioStream, subtitleStream);
}

bool MediaLayer::stopPlayback(FinishMode mode) {
	// Are we done?
	if (!(videoState & VS_PLAYING) && !frame_gpu[DefFrame] && !frame_gpu[NewFrame])
		return true;

	audioBridge.reset();

#if defined(PIVAS)
	if (pivasNativeActive) {
		// The ffmpeg controller was never started for this clip — close the
		// native session instead (idempotent; stopPlayback legitimately runs
		// twice: EOF LeaveCurrent, then _csp/shutdown Normal). frame_gpu
		// holds a real RGBA copy of the last frame, so it stays displayable
		// after the decoder's memory is gone.
		pivasReleaseNative();
		videoState &= ~VS_PLAYING;

		if (mode == FinishMode::Normal) {
			for (auto &img : {frame_gpu[DefFrame], frame_gpu[NewFrame], mask_gpu})
				if (img)
					gpu.freeImage(img);
			frame_gpu[DefFrame] = frame_gpu[NewFrame] = mask_gpu = nullptr;
		}

		return true;
	}
#endif

	if (media.finish(true)) {

		// Depending on the flags reset frame_gpu/mask_gpu
		// return false is the last frame still needs to be grabbed

		media.resetState();
		videoState &= ~VS_PLAYING;

		if (mode == FinishMode::Normal) {
			for (auto &img : {frame_gpu[DefFrame], frame_gpu[NewFrame], mask_gpu})
				if (img)
					gpu.freeImage(img);
			frame_gpu[DefFrame] = frame_gpu[NewFrame] = mask_gpu = nullptr;
		}

		return true;
	}

	return false;
}

bool MediaLayer::loadPresentation(bool alphaMasked, bool loop, std::string &sub_file) {
	/* Determine video dimensions */

	scaleRect = {0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height)};

	int frameWidth, frameHeight, channels = alphaMasked ? 4 : 3;
#if defined(PIVAS)
	if (pivasNativeActive) {
		int nativeW = 0, nativeH = 0;
		if (pivas_video_dimensions(&nativeW, &nativeH, nullptr) < 0 || nativeW <= 0 || nativeH <= 0) {
			pivas_video_close();
			pivasNativeActive = false;
			return false;
		}
		pivasNativeFrameSize(nativeW, nativeH, frameWidth, frameHeight, alphaMasked);
	} else
#endif
		media.frameSize(scaleRect, frameWidth, wFactor, frameHeight, hFactor, alphaMasked);

	videoRect.w = frameWidth;
	videoRect.h = frameHeight;

	/* Prepare GPU_Images */

	if (frame_gpu[NewFrame]) {
		sendToLog(LogLevel::Error, "Discovered uncommitted video frame, this is not allowed, attempting to recover\n");
		gpu.freeImage(frame_gpu[NewFrame]);
		frame_gpu[NewFrame] = nullptr;
	}

	if (frame_gpu[DefFrame] && (frame_gpu[DefFrame]->w != frameWidth ||
	                            frame_gpu[DefFrame]->h != frameHeight ||
	                            frame_gpu[DefFrame]->bytes_per_pixel !=
	                                static_cast<int>(channels * sizeof(uint8_t)))) {
		sendToLog(LogLevel::Error, "Transitioning from a different video type is not allowed\n");
		gpu.freeImage(frame_gpu[DefFrame]);
		frame_gpu[DefFrame] = nullptr;
	}

	// If there is an existing DefFrame, load to NewFrame
	// Otherwise load to DefFrame
	auto &frame = frame_gpu[DefFrame] ? frame_gpu[NewFrame] : frame_gpu[DefFrame];
#if defined(PIVAS)
	// Native path uploads RGBA directly (GXM has no 24bpp format; a 3-byte
	// image forces the loader to expand RGB->RGBA with a malloc + full-frame
	// pass per video frame).
	frame = gpu.createImage(frameWidth, frameHeight, (alphaMasked || pivasNativeActive) ? 4 : 3);
#else
	frame       = gpu.createImage(frameWidth, frameHeight, alphaMasked ? 4 : 3);
#endif
	GPU_GetTarget(frame);
	gpu.clearWholeTarget(frame->target);

	videoState |= VS_AWAITS_COMMIT;

	if (mask_gpu)
		gpu.freeImage(mask_gpu);
	if (alphaMasked) {
		mask_gpu = gpu.createImage(frameWidth, frameHeight, 3);
		GPU_GetTarget(mask_gpu);
		gpu.clearWholeTarget(mask_gpu->target);
	} else {
		mask_gpu = nullptr;
	}

	/* Signal readiness */
	videoState |= VS_PLAYING;

	/* Prepare media presentation */

#if defined(PIVAS)
	if (pivasNativeActive) {
		// SceAvPlayer owns demux/decode/pacing internally; update() just
		// polls it once per engine frame. Subtitles are burned into the
		// re-encoded clips and pam audio is a parallel mixer ogg, so
		// neither engine subsystem loads.
		pivas_video_set_looping(loop);

		// Zero-copy display (see pivasPresentNativeFrame). The placeholder
		// is deliberately tiny and power-of-two: sdl-gpu then maps the
		// whole 0..1 texture range to it, which is exactly the decoder
		// frame once the loader has re-pointed the texture.
		pivasNativeGpu = false;
		if (pivasYuvImage) {
			GPU_FreeImage(pivasYuvImage);
			pivasYuvImage = nullptr;
		}
		if (!alphaMasked && pivasNativeGpuEnabled()) {
			pivasYuvImage = GPU_CreateImage(16, 16, GPU_FORMAT_RGBA);
			if (pivasYuvImage) {
				GPU_SetImageFilter(pivasYuvImage, GPU_FILTER_LINEAR);
				GPU_SetWrapMode(pivasYuvImage, GPU_WRAP_NONE, GPU_WRAP_NONE);
				GPU_SetSnapMode(pivasYuvImage, GPU_SNAP_NONE);
				GPU_SetBlending(pivasYuvImage, false);
				pivasNativeGpu = true;
			} else {
				sendToLog(LogLevel::Warn, "[media] no placeholder image for the GPU video path, using the CPU path\n");
			}
		}
		uint64_t durationMs = 0;
		pivas_video_dimensions(nullptr, nullptr, &durationMs);
		expectedDurationMs = durationMs;
		framesToAdvance    = 0;
		nanosPerFrame      = 0; // unused on the native path
		if (sub_file != "")
			sendToLog(LogLevel::Warn, "[media] native video ignores external subtitles %s\n", sub_file.c_str());
		return true;
	}
#endif

	bool ret = media.loadPresentation(videoRect, loop);
	// We should start from displaying the first frame
	// 0 here would make it need a 1/fps delay before displaying anything
	framesToAdvance = 1;
	nanosPerFrame   = media.getNanosPerFrame();
#if defined(PIVAS)
	expectedDurationMs = media.getDurationMs();
#endif

	if (ret) {
		/* Load subtitles */
		std::unique_ptr<char[]> subtitles(sub_file != "" ? (*reader)->completePath(sub_file.c_str(), FileType::File) : nullptr);
		media.addSubtitles(subtitles.get(), frameWidth, frameHeight);

		/* Load audio */
		if (media.hasStream(MediaProcController::AudioEntry)) {
			audioBridge = std::make_unique<AudioBridge>(MIX_VIDEO_CHANNEL,
			                                            !ons.volume_on_flag ? 0 : ons.video_volume * MIX_MAX_VOLUME / 100, [](size_t &sz) {
				                                            return media.advanceAudioChunks(sz);
			                                            });
			ret         = audioBridge->prepare();
		}
	}

	return ret;
}

void MediaLayer::startProcessing() {
#if defined(PIVAS)
	if (pivasNativeActive)
		return; // SceAvPlayer autostarted at open
#endif
	media.startProcessing();
}


bool MediaLayer::ensurePlanesImgs(AVPixelFormat f, size_t n, float w, float h) {
	constexpr size_t num = sizeof(planes_gpu) / sizeof(*planes_gpu);
	if (n > num)
		return false;

	float widths[num]{w, w, w, w};
	float heights[num]{h, h, h, h};
	float formats[num]{1, 1, 1, 1};

	if (f == AV_PIX_FMT_NV12) {
		widths[1] /= 2;
		heights[1] /= 2;
		formats[1] = 2;
		assert(n == 2);
	} else if (f == AV_PIX_FMT_YUV420P) {
		widths[1] = widths[2] = widths[0] / 2;
		heights[1] = heights[2] = heights[0] / 2;
		assert(n == 3);
	}

	for (size_t i = 0; i < n; i++) {
		if (planes_gpu[i] == nullptr ||
		    planes_gpu[i]->w != widths[i] ||
		    planes_gpu[i]->h != heights[i] ||
		    planes_gpu[i]->bytes_per_pixel != formats[i]) {
			if (planes_gpu[i])
				gpu.freeImage(planes_gpu[i]);
			planes_gpu[i] = gpu.createImage(widths[i], heights[i], formats[i]);
		}
	}
	return true;
}

bool MediaLayer::update(bool old) {
	// Not much to do here, although I doubt this can happen
	if (!sprite)
		return true;
	auto sp = old ? sprite->oldNew(REFRESH_BEFORESCENE_MODE) : sprite;

	// Reset clock if:
	// - EOF reached;
	// - not playing;
	// - we are uncommitted
	// Also reset the clock to avoid going forward before staring playback
	if ((videoState & (VS_END_OF_FILE | VS_AWAITS_COMMIT)) || !(videoState & VS_PLAYING)) {
		sp->clock.reset();
		// Do not update unless we have not
		if (!(videoState & VS_AWAITS_COMMIT))
			return true;
	}

#if defined(PIVAS)
	if (pivasNativeActive) {
		PivasVideoFrame nativeFrame;
		int res = pivas_video_poll(&nativeFrame);
		if (res < 0) {
			videoState |= VS_END_OF_FILE;
			MEDIA_TRACE_CAPPED(native_eof_trace, 8, "[media] native video ended\n");
		} else if (res > 0) {
			auto frame = frame_gpu[NewFrame] ? frame_gpu[NewFrame] : frame_gpu[DefFrame];
			if (frame)
				pivasPresentNativeFrame(frame, nativeFrame);
		}
		return true;
	}
#endif

	//sendToLog(LogLevel::Info, "MediaLayer::update. Total: %i, Lap: %i, ", mediaClock.time(), mediaClock.lap());
	uint32_t toAdd{0};

	// Do not update until audio plays if it is enabled
	if (media.hasStream(MediaProcController::AudioEntry) && audioBridge && !audioBridge->update(toAdd))
		return true;

	if (toAdd != 0) {
		sp->clock.reset();
		//sendToLog(LogLevel::Info, "toAdd %d\n", toAdd);
	}

	auto objectClockLap = sp->clock.lapNanos();

	//sendToLog(LogLevel::Info, "mediaClock: %llu, yesobjectClock: %llu, objectClockLap: %llu\n", mediaClock.timeNanos(), object->clock.timeNanos(), objectClockLap);
	mediaClock.tickNanos(objectClockLap);
	if (toAdd != 0)
		mediaClock.tick(toAdd);
	if (!mediaClock.hasCountdown())
		mediaClock.addCountdownNanos(nanosPerFrame);
	while (mediaClock.expired()) {
		mediaClock.addCountdownNanos(nanosPerFrame);
		framesToAdvance++;
	}

	//sendToLog(LogLevel::Info, "framesToAdvance: %i\n", framesToAdvance);

	if (framesToAdvance > 0) {
		bool endOfFile      = false;
		auto thisVideoFrame = media.advanceVideoFrames(framesToAdvance, endOfFile);
		if (endOfFile)
			videoState |= VS_END_OF_FILE;

#if defined(PIVAS)
		if (!thisVideoFrame && !endOfFile)
			MEDIA_TRACE_CAPPED(starve_trace, 8, "[media] no frame available (decode starved), need=%d\n", framesToAdvance);
#endif

		if (thisVideoFrame) {
			// This is not a mistake, frame update logic does not depend on old
			// old is only relevant in sprite verification
			auto frame = frame_gpu[NewFrame] ? frame_gpu[NewFrame] : frame_gpu[DefFrame];
			//sendToLog(LogLevel::Info, "[Frame %lld] Fmt: %d(nv12<%d>,yuv420p<%d>), planes: %d<%d, %d, %d>, gpu out: %dx%d\n",
			//			thisVideoFrame->frameNumber, thisVideoFrame->srcFormat, AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P,
			//			thisVideoFrame->planesCnt, thisVideoFrame->linesize[0], thisVideoFrame->linesize[1], thisVideoFrame->linesize[2],
			//			frame->w, frame->h);
			if (thisVideoFrame->srcFormat == AV_PIX_FMT_NV12 || thisVideoFrame->srcFormat == AV_PIX_FMT_YUV420P) {
#if defined(PIVAS)
				MEDIA_TRACE_CAPPED(planes_trace, 8, "[media] frame %lld via GPU planes fmt=%d planes=%d %dx%d masked=%d\n",
				                   thisVideoFrame->frameNumber, thisVideoFrame->srcFormat,
				                   thisVideoFrame->planesCnt, videoRect.w, videoRect.h, mask_gpu != nullptr);
#endif
				ensurePlanesImgs(thisVideoFrame->srcFormat, thisVideoFrame->planesCnt, videoRect.w, mask_gpu ? videoRect.h * 2 : videoRect.h);
				if (thisVideoFrame->srcFormat == AV_PIX_FMT_NV12) {
					gpu.convertNV12ToRGB(frame, planes_gpu, videoRect, thisVideoFrame->planes, thisVideoFrame->linesize, mask_gpu);
				} else {
					gpu.convertYUVToRGB(frame, planes_gpu, videoRect, thisVideoFrame->planes, thisVideoFrame->linesize, mask_gpu);
				}
			} else /*if (thisVideoFrame->srcFormat == AV_PIX_FMT_NONE)*/ { /* Converted by sws earlier */
#if defined(PIVAS)
				MEDIA_TRACE_CAPPED(sws_trace, 8, "[media] frame %lld via sws surface %dx%d masked=%d\n",
				                   thisVideoFrame->frameNumber, videoRect.w, videoRect.h, mask_gpu != nullptr);
#endif
				if (mask_gpu) {
#if defined(PIVAS)
					if (!composeMaskedVideoFrameCPU(frame, thisVideoFrame->surface, videoRect.w, videoRect.h)) {
						MEDIA_TRACE_CAPPED(mask_fail_trace, 4, "[media] CPU mask compose failed, falling back to mergeAlpha shader\n");
						GPU_Rect maskRect = videoRect;
						maskRect.y += maskRect.h;
						gpu.mergeAlpha(frame, &videoRect, mask_gpu, &maskRect, thisVideoFrame->surface);
					}
#else
					GPU_Rect maskRect = videoRect;
					maskRect.y += maskRect.h;
					gpu.mergeAlpha(frame, &videoRect, mask_gpu, &maskRect, thisVideoFrame->surface);
#endif
				} else {
					gpu.updateImage(frame, nullptr, thisVideoFrame->surface, nullptr, false);
				}
			}

			//sendToLog(LogLevel::Info, "Updated frame number %d\n", thisFrame->frameNumber);

			// Now we are done; give back the surface for later use
			media.giveImageBack(thisVideoFrame->surface);
		}
	}
	return true;
}

void MediaLayer::refresh(GPU_Target *target, GPU_Rect &clip, float x, float y, bool centre_coordinates, int rm, float scalex, float scaley) {
	auto frame = (!(rm & REFRESH_BEFORESCENE_MODE) && frame_gpu[NewFrame]) ? frame_gpu[NewFrame] : frame_gpu[DefFrame];

	// I think this should never happen actually
	if (!frame || clip.w == 0 || clip.h == 0) {
		return;
	}

	if (!centre_coordinates) {
		x += (frame->w * wFactor) / 2.0;
		y += (frame->h * hFactor) / 2.0;
	}

	scalex = scalex != 0 ? wFactor * scalex : wFactor;
	scaley = scaley != 0 ? hFactor * scaley : hFactor;

	//sendToLog(LogLevel::Info, "frame_gpu->h %u h_factor %f y %f y+(f*h/2.0) %f\n", frame_gpu->h, h_factor, y, y+((frame_gpu->h*h_factor)/2.0));

	gpu.copyGPUImage(frame, nullptr, &clip, target, x, y, scalex, scaley, 0, true);

	if (audioBridge)
		audioBridge->startPlayback();

	if ((videoState & VS_END_OF_FILE) && (videoState & VS_PLAYING)) {
		while (!stopPlayback(FinishMode::LeaveCurrent)) {
			// This should not happen since the decoders are stopped by this time
			sendToLog(LogLevel::Error, "Failed to stop video playback at once, something is wrong\n");
		}
	}
}

void MediaLayer::commit() {
	if (frame_gpu[NewFrame]) {
		gpu.freeImage(frame_gpu[DefFrame]); // Guaranteed to be not null
		frame_gpu[DefFrame] = frame_gpu[NewFrame];
		frame_gpu[NewFrame] = nullptr;
	}
	videoState &= ~VS_AWAITS_COMMIT;
}

bool MediaLayer::isPlaying(bool checkStatic) {
	return (videoState & VS_PLAYING) || (checkStatic && frame_gpu[DefFrame]);
}
