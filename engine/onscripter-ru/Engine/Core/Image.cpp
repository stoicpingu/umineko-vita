/**
 *  Image.cpp
 *  ONScripter-RU
 *
 *  Code for image loading and processing.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Core/ONScripter.hpp"
#include "Resources/Support/Resources.hpp"
#include "Engine/Components/Async.hpp"
#include "Engine/Components/Window.hpp"
#include "Engine/Readers/Base.hpp"
#include "Engine/Graphics/PNG.hpp"
#include "Engine/Graphics/Pool.hpp"

#include <SDL2/SDL_thread.h>
#if defined(PIVAS)
#include <GLES2/gl2.h>
#endif

#include <new>
#include <string>
#include <map>
#include <cstdio>

#ifndef PIVAS_INSTANT_REDUCE_MOTION_EFFECTS
#define PIVAS_INSTANT_REDUCE_MOTION_EFFECTS 1
#endif

#if defined(PIVAS)
extern bool pivas_final_window_seen_live_dialogue; // ONScripter.cpp
extern uint32_t pivas_final_window_hold_until;     // ONScripter.cpp
#endif

#ifndef PIVAS_RENDER_TRACE
#define PIVAS_RENDER_TRACE 0
#endif

#if defined(PIVAS) && PIVAS_RENDER_TRACE
static int pivas_hud_refresh_trace_count = 0;
static int pivas_hud_branch_trace_count  = 0;

#define PIVAS_TRACE_REDUCED_DRAW(target)                                                  \
	do {                                                                                  \
		if (vita_effect_scene_dst && (target) == vita_effect_scene_dst->target)           \
			vita_render_trace_scene_draw_count++;                                         \
		else if (vita_effect_hud_dst && (target) == vita_effect_hud_dst->target)          \
			vita_render_trace_hud_draw_count++;                                           \
	} while (false)
#else
#define PIVAS_TRACE_REDUCED_DRAW(target) \
	do {                                 \
	} while (false)
#endif

#if defined(PIVAS) && PIVAS_HUD_OUTPUT_PROBE
static int pivas_hud_output_probe_count = 0;
static GPU_Image *pivas_hud_probe_quadrants = nullptr;
static GPU_Image *pivas_hud_probe_atlas = nullptr;
#endif

#if defined(PIVAS) && PIVAS_TEXT_CACHE_TRACE
static int pivas_atomic_hud_trace_count = 0;
#endif

#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_TRACE
static int pivas_dynamic_hud_lifecycle_trace_count = 0;
static int pivas_dynamic_hud_gl_trace_count        = 0;

static const char *pivasDynamicHudGlErrorName(GLenum err) {
	switch (err) {
		case GL_NO_ERROR: return "GL_NO_ERROR";
		case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
		case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
		case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
		case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
		default: return "GL_UNKNOWN";
	}
}

static void pivasDrainDynamicHudGlErrors(const char *stage) {
	GPU_FlushBlitBuffer();
	for (int i = 0; i < 8; i++) {
		GLenum err = glGetError();
		if (err == GL_NO_ERROR)
			break;
		if (pivas_dynamic_hud_gl_trace_count++ < 48) {
			sendToLog(LogLevel::Warn,
			          "[dyn-window-hud-gl] stale before %s err=0x%04x %s\n",
			          stage ? stage : "(null)", err, pivasDynamicHudGlErrorName(err));
		}
	}
}

static void pivasCheckDynamicHudGlError(const char *stage) {
	GPU_FlushBlitBuffer();
	GLenum err = glGetError();
	if (err != GL_NO_ERROR && pivas_dynamic_hud_gl_trace_count++ < 48) {
		sendToLog(LogLevel::Error,
		          "[dyn-window-hud-gl] after %s err=0x%04x %s\n",
		          stage ? stage : "(null)", err, pivasDynamicHudGlErrorName(err));
	}
}
#endif

void ONScripter::loadImageIntoCache(int id, const std::string &filename_str, bool allow_rgb) {
	bool has_alpha;
	SDL_Surface *surface = loadImage(filename_str.c_str(), &has_alpha, allow_rgb);
	{
		Lock lock(&imageCache);
		auto ptr = imageCache.get(filename_str);
		if (ptr) {
			//sendToLog(LogLevel::Error, "Tried to double-add a surface refs %d (ptr: %p, this: %p)\n", surface->refcount, ptr->surface, surface);
			if (ptr->surface != surface) {
				sendToLog(LogLevel::Error, "INSANE: different surfaces in loadImageIntoCache\n");
			}
			SDL_FreeSurface(surface);
			return;
		}
		imageCache.add(id, filename_str, std::make_shared<Wrapped_SDL_Surface>(surface, has_alpha));
	}
}

void ONScripter::dropCache(int *id, const std::string &filename_str) {
	// Pass nullptr to drop string from all caches
	{
		Lock lock(&imageCache);
		if (!id) {
			imageCache.removeAll(filename_str);
		} else {
			imageCache.remove(*id, filename_str);
		}
	}
}

GPU_Image *ONScripter::loadGpuImage(const char *file_name, bool allow_rgb) {
	//This function is unable to handle archives

	if (!file_name) {
		sendToLog(LogLevel::Error, "loadGpuImage: Incorrect file_name was passed!\n");
		return nullptr;
	}

	SDL_Surface *input_surface = loadImage(file_name, nullptr, allow_rgb);

	if (!input_surface) {
		sendToLog(LogLevel::Error, "loadGpuImage: File %s cannot be opened!\n", file_name);
		return nullptr;
	}

	GPU_Image *img = gpu.copyImageFromSurface(input_surface);

	if (!img) {
		sendToLog(LogLevel::Error, "loadGpuImage: File %s cannot be opened!\n", file_name);
		SDL_FreeSurface(input_surface);
		return nullptr;
	}

	gpu.multiplyAlpha(img);
	SDL_FreeSurface(input_surface);

	return img;
}

SDL_Surface *ONScripter::loadImage(const char *filename, bool *has_alpha, bool allow_rgb) {
	// This function assumes we never load the same image with a different AnimationInfo::trans_mode

	//sendToLog(LogLevel::Info, "loadImage (%s)\n", filename);

	if (!filename)
		return nullptr;

	{
		Lock lock(&imageCache);
		std::shared_ptr<Wrapped_SDL_Surface> r = imageCache.get(filename);
		if (r && r->surface) {
			if (has_alpha)
				*has_alpha = r->has_alpha;
			if (!allow_rgb && r->surface->format->BitsPerPixel == 24) {
				SDL_Surface *ret = SDL_ConvertSurfaceFormat(r->surface, pixel_format_enum_32bpp, SDL_SWSURFACE);
				// Allow the 24-bit r->surface to be freed by the wrapped surface destruction
				return ret;
			}
			r->surface->refcount++;
			return r->surface;
		}
	}

	SDL_Surface *tmp = nullptr;

	if (filename[0] == '>')
		tmp = createRectangleSurface(filename);
	else if (filename[0] != '*') // layers begin with *
		tmp = createSurfaceFromFile(filename);
	if (tmp == nullptr) {
		//sendToLog(LogLevel::Info, "returning from loadImage [2]\n");
		return nullptr;
	}

	bool has_colorkey = false;
	uint32_t colorkey = 0;

	if (has_alpha) {
		*has_alpha = (tmp->format->Amask != 0);
		if (!(*has_alpha) && !SDL_GetColorKey(tmp, &colorkey)) {
			has_colorkey = true;

			if (tmp->format->palette) {
				//palette will be converted to RGBA, so don't do colorkey check
				has_colorkey = false;
			}
			*has_alpha = true;
		}
	}

	uint32_t format  = SDL_MasksToPixelFormatEnum(tmp->format->BitsPerPixel, tmp->format->Rmask, tmp->format->Gmask, tmp->format->Bmask, tmp->format->Amask);
	SDL_Surface *ret = tmp;

	bool conversionRequired = !(
	    // no conversion to 32-bit is required if:
	    (format == pixel_format_enum_32bpp) ||             // the image is already in 32-bit format;
	    (allow_rgb && (format == pixel_format_enum_24bpp)) // or the image is already in 24-bit format and we're allowing images without alpha
	);

	if (conversionRequired) {
		ret = SDL_ConvertSurfaceFormat(tmp, pixel_format_enum_32bpp, SDL_SWSURFACE);
		SDL_FreeSurface(tmp);
	}

	//  A PNG image may contain an alpha channel, which complicates
	// handling loaded images when the ":a" alphablend tag is used,
	// since the standard method was to assume the right half of the image
	// contains an alpha data mask for the left half.
	//  The current default behavior is to use the PNG image's alpha
	// channel if available, and only process for an old-style mask
	// when no alpha channel was provided.
	// However, this could cause problems running older NScr games
	// which have PNG images containing old-style masks but also an
	// opaque alpha channel.
	//  Therefore, we provide a hack, set with the --detect-png-nscmask
	// command-line option, to auto-detect if a PNG image is likely to
	// have an old-style mask.  We assume that an old-style mask is intended
	// if the image either has no alpha channel, or the alpha channel it has
	// is completely opaque.  (Note that this used to be the default
	// behavior for onscripter-en.)
	//  Note that using the --force-png-nscmask option will always assume
	// old-style masks, while --force-png-alpha will produce the current
	// default behavior.
	if ((png_mask_type != PNG_MASK_USE_ALPHA) &&
	    has_alpha && *has_alpha) {
		if (png_mask_type == PNG_MASK_USE_NSCRIPTER)
			*has_alpha = false;
		else if (png_mask_type == PNG_MASK_AUTODETECT) {
			const uint32_t aval = *static_cast<uint32_t *>(ret->pixels) & ret->format->Amask;
			if (aval == ret->format->Amask) {
				*has_alpha = false;
				for (int y = 0; y < ret->h; ++y) {
					uint32_t *pixbuf = reinterpret_cast<uint32_t *>(static_cast<char *>(ret->pixels) + y * ret->pitch);
					for (int x = ret->w; x > 0; --x, ++pixbuf) {
						// Resolving ambiguity per Tatu's patch, 20081118.
						// I note that this technically changes the meaning of the
						// code, since != is higher-precedence than &, but this
						// version is obviously what I intended when I wrote this.
						// Has this been broken all along?  :/  -- Haeleth
						if ((*pixbuf & ret->format->Amask) != aval) {
							*has_alpha = true;
							return ret;
						}
					}
				}
			}

			if (!*has_alpha && has_colorkey) {
				// has a colorkey, so run a match against rgb values
				const uint32_t aval = colorkey & ~(ret->format->Amask);
				if (aval == (*static_cast<uint32_t *>(ret->pixels) & ~(ret->format->Amask)))
					return ret;
				*has_alpha = false;
				for (int y = 0; y < ret->h; ++y) {
					uint32_t *pixbuf = reinterpret_cast<uint32_t *>(static_cast<char *>(ret->pixels) + y * ret->pitch);
					for (int x = ret->w; x > 0; --x, ++pixbuf) {
						if ((*pixbuf & ~(ret->format->Amask)) == aval) {
							*has_alpha = true;
							return ret;
						}
					}
				}
			}
		}
	}

	//sendToLog(LogLevel::Info, "returning from loadImage [3]\n");

	//printClock("loadImage ends");
	return ret;
}

SDL_Surface *ONScripter::createRectangleSurface(const char *filename) {
	int c = 1, w = 0, h = 0;
	while (filename[c] != 0x0a && filename[c] != 0x00) {
		if (filename[c] >= '0' && filename[c] <= '9')
			w = w * 10 + filename[c] - '0';
		if (filename[c] == ',') {
			c++;
			break;
		}
		c++;
	}

	while (filename[c] != 0x0a && filename[c] != 0x00) {
		if (filename[c] >= '0' && filename[c] <= '9')
			h = h * 10 + filename[c] - '0';
		if (filename[c] == ',') {
			c++;
			break;
		}
		c++;
	}

	while (filename[c] == ' ' || filename[c] == '\t') c++;
	int n = 0, c2 = c;
	while (filename[c] == '#') {
		uchar3 col;
		readColor(&col, filename + c);
		n++;
		c += 7;
		while (filename[c] == ' ' || filename[c] == '\t') c++;
	}

	SDL_Surface *tmp = SDL_CreateRGBSurface(SDL_SWSURFACE, w, h,
	                                        32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);

	c = c2;
	for (int i = 0; i < n; i++) {
		uchar3 col;
		readColor(&col, filename + c);
		c += 7;
		while (filename[c] == ' ' || filename[c] == '\t') c++;

		SDL_Rect rect;
		rect.x = w * i / n;
		rect.y = 0;
		rect.w = w * (i + 1) / n - rect.x;
		if (i == n - 1)
			rect.w = w - rect.x;
		rect.h = h;
		SDL_FillRect(tmp, &rect, SDL_MapRGBA(tmp->format, col.x, col.y, col.z, 0xff));
	}

	return tmp;
}

SDL_Surface *ONScripter::createSurfaceFromFile(const char *filename) {

	static int surfaceCreationLockVar = 0;

	size_t length{0};
	uint8_t *buffer{nullptr};

	if (filename[0]) {
		Lock lock(&surfaceCreationLockVar);
		script_h.reader->getFile(filename, length, &buffer);
	}

	if (length == 0) {
		//don't complain about missing cursors
		if (!equalstr(filename, "uoncur.bmp") &&
		    !equalstr(filename, "uoffcur.bmp") &&
		    !equalstr(filename, "doncur.bmp") &&
		    !equalstr(filename, "doffcur.bmp") &&
		    !equalstr(filename, "cursor0.bmp") &&
		    !equalstr(filename, "cursor1.bmp")) {
			std::snprintf(script_h.errbuf, MAX_ERRBUF_LEN,
			              "can't find file [%s]", filename);
			errorAndCont(script_h.errbuf, nullptr, "I/O Issue");
		}
		return nullptr;
	}

	if (filelog_flag)
		script_h.findAndAddLog(script_h.log_info[ScriptHandler::FILE_LOG], filename, true);

	const char *ext  = std::strrchr(filename, '.');
	SDL_RWops *src   = SDL_RWFromMem(buffer, static_cast<int>(length));
	SDL_Surface *tmp = nullptr;

	if (ext && (equalstr(ext + 1, "PNG") || equalstr(ext + 1, "png"))) {
		PNGLoader *loader = pngImageLoaderPool.getLoader();
		tmp               = loader->loadPng(src);
		if (!tmp)
			sendToLog(LogLevel::Error, "Failed to use internal PNGLoader on %s\n", filename);
		pngImageLoaderPool.giveLoader(loader);
	}

	if (!tmp) {
		Lock lock(&surfaceCreationLockVar);
		tmp = IMG_Load_RW(src, 0);
		if (!tmp && ext && (equalstr(ext + 1, "JPG") || equalstr(ext + 1, "jpg"))) {
			sendToLog(LogLevel::Warn, " *** force-loading a JPG image [%s]\n", filename);
			tmp = IMG_LoadJPG_RW(src);
		}
		if (!tmp)
			sendToLog(LogLevel::Error, " *** can't load file [%s] with purported length %zu bytes: %s ***\n", filename, length, IMG_GetError());
	}

	SDL_RWclose(src);

	freearr(&buffer);

	return tmp;
}

void ONScripter::effectBlendToCombinedImage(GPU_Image *mask_gpu, int trans_mode, uint32_t mask_value, GPU_Image *image) {
	if (image == pre_screen_gpu)
		pre_screen_render = true;

	int refresh_mode_src = effect_refresh_mode_src;
	int refresh_mode_dst = effect_refresh_mode_dst;
	if (refresh_mode_src == -1)
		refresh_mode_src = refreshMode() | REFRESH_BEFORESCENE_MODE;
	if (refresh_mode_dst == -1)
		refresh_mode_dst = refreshMode();

	bool srcb4         = refresh_mode_src & REFRESH_BEFORESCENE_MODE;
	bool dstb4         = refresh_mode_dst & REFRESH_BEFORESCENE_MODE;
	auto &src_dr_scene = (srcb4 ? before_dirty_rect_scene : dirty_rect_scene);
	auto &src_dr_hud   = (srcb4 ? before_dirty_rect_hud : dirty_rect_hud);
	auto &dst_dr_scene = (dstb4 ? before_dirty_rect_scene : dirty_rect_scene);
	auto &dst_dr_hud   = (dstb4 ? before_dirty_rect_hud : dirty_rect_hud);

	if (camera.has_moved || !src_dr_scene.isEmpty() || !src_dr_hud.isEmpty())
		mergeForEffect(combined_effect_src_gpu, &src_dr_scene.bounding_box_script, &src_dr_hud.bounding_box_script, refresh_mode_src | CONSTANT_REFRESH_MODE);
	if (camera.has_moved || !dst_dr_scene.isEmpty() || !dst_dr_hud.isEmpty())
		mergeForEffect(combined_effect_dst_gpu, &dst_dr_scene.bounding_box_script, &dst_dr_hud.bounding_box_script, refresh_mode_dst | CONSTANT_REFRESH_MODE);
	// v   note: we pass nullptr to effectblendgpu -- the whole src and dst are blitted onto prescreen

	/*sendToLog(LogLevel::Info, "src dr scene %f x %f ; hud %f x %f ; dst dr scene %f x %f ; hud %f x %f ; rfmodesrc %d ; dst %d\n",
		src_dr_scene.bounding_box_script.w,
		src_dr_scene.bounding_box_script.h,
		src_dr_hud.bounding_box_script.w,
		src_dr_hud.bounding_box_script.h,
		dst_dr_scene.bounding_box_script.w,
		dst_dr_scene.bounding_box_script.h,
		dst_dr_hud.bounding_box_script.w,
		dst_dr_hud.bounding_box_script.h,
		refresh_mode_src,
		refresh_mode_dst
	);*/

	effectBlendGPU(mask_gpu, trans_mode, mask_value, nullptr, combined_effect_src_gpu, combined_effect_dst_gpu, image);
}

void ONScripter::effectBlendGPU(GPU_Image *mask_gpu, int trans_mode,
                                uint32_t mask_value, GPU_Rect *clip,
                                GPU_Image *src1, GPU_Image *src2, GPU_Image *dst) {
	if (!src1 || !src2 || !dst) {
		sendToLog(LogLevel::Error, "Invalid effectBlendGPU arguments\n");
		return;
	}

	GPU_Rect fullclip{0, 0, static_cast<float>(window.script_width), static_cast<float>(window.script_height)};
	if (!clip)
		clip = &fullclip;

	//sendToLog(LogLevel::Info, "effectBlendGPU clip %d %d %d %d\n", clip->x, clip->y, clip->w, clip->h);

	if (trans_mode == ALPHA_BLEND_CONST) {
		gpu.setShaderProgram("blendByMask.frag");
		gpu.bindImageToSlot(src1, 0);
		gpu.bindImageToSlot(src2, 1);
		gpu.bindImageToSlot(src2, 2); // hack for now, to avoid declared but unused param. need to make shader work without supplying it
		gpu.setShaderVar("mask_value", static_cast<int>(mask_value) * 2);
		gpu.setShaderVar("constant_mask", 1);
		gpu.setShaderVar("crossfade", 1);
		gpu.copyGPUImage(src1, clip, clip, dst->target, clip->x, clip->y);
		gpu.unsetShaderProgram();
	} else if ((trans_mode == ALPHA_BLEND_FADE_MASK ||
	            trans_mode == ALPHA_BLEND_CROSSFADE_MASK) &&
	           mask_gpu) {
		gpu.setShaderProgram("blendByMask.frag");
		gpu.bindImageToSlot(src2, 1);
		gpu.bindImageToSlot(mask_gpu, 2);
		gpu.setShaderVar("constant_mask", 0);
		gpu.setShaderVar("mask_value", static_cast<int>(mask_value));
		gpu.setShaderVar("crossfade", trans_mode == ALPHA_BLEND_CROSSFADE_MASK);
		gpu.copyGPUImage(src1, clip, clip, dst->target, clip->x, clip->y);
		gpu.unsetShaderProgram();
	} else {
		gpu.clearWholeTarget(dst->target, 255, 0, 0, 255);
	}
}

#ifndef PIVAS_CPU_GLYPH_COLOR
#define PIVAS_CPU_GLYPH_COLOR 1
#endif

#if defined(PIVAS) && PIVAS_CPU_GLYPH_COLOR
static uint8_t pivasClampColor(float value) {
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return static_cast<uint8_t>(value + 0.5f);
}

static SDL_Surface *createColouredGlyphSurface(SDL_Surface *alpha_surface, SDL_Color color,
                                               bool gradient, const GlyphValues *glyph) {
	if (!alpha_surface)
		return nullptr;

	SDL_Surface *surface = SDL_CreateRGBSurface(SDL_SWSURFACE, alpha_surface->w, alpha_surface->h,
	                                            32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
	if (!surface)
		return nullptr;

	if (SDL_MUSTLOCK(alpha_surface) && SDL_LockSurface(alpha_surface) != 0) {
		SDL_FreeSurface(surface);
		return nullptr;
	}
	if (SDL_MUSTLOCK(surface) && SDL_LockSurface(surface) != 0) {
		if (SDL_MUSTLOCK(alpha_surface))
			SDL_UnlockSurface(alpha_surface);
		SDL_FreeSurface(surface);
		return nullptr;
	}

	for (int y = 0; y < alpha_surface->h; y++) {
		auto *src_row = static_cast<uint8_t *>(alpha_surface->pixels) + y * alpha_surface->pitch;
		auto *dst_row = static_cast<uint8_t *>(surface->pixels) + y * surface->pitch;
		float lightening = 0.0f;
		if (gradient && glyph && glyph->faceAscender > 0) {
			float current_y                = static_cast<float>(alpha_surface->h) * (1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(alpha_surface->h));
			float current_y_above_baseline = current_y - (static_cast<float>(alpha_surface->h) - static_cast<float>(glyph->maxy));
			float percent_above_baseline   = current_y_above_baseline > 0 ? current_y_above_baseline / static_cast<float>(glyph->faceAscender) : 0.0f;
			bool is_white                  = color.r == 255 && color.g == 255 && color.b == 255;
			lightening                     = 127.5f * (is_white ? (percent_above_baseline - 0.6f) : (0.65f - percent_above_baseline));
		}
		for (int x = 0; x < alpha_surface->w; x++) {
			uint8_t a    = static_cast<uint8_t>((static_cast<uint16_t>(src_row[x]) * color.a + 127) / 255);
			uint8_t *dst = dst_row + x * 4;
			uint8_t r    = pivasClampColor(static_cast<float>(color.r) + lightening);
			uint8_t g    = pivasClampColor(static_cast<float>(color.g) + lightening);
			uint8_t b    = pivasClampColor(static_cast<float>(color.b) + lightening);
			dst[0]       = static_cast<uint8_t>((static_cast<uint16_t>(r) * a + 127) / 255);
			dst[1]       = static_cast<uint8_t>((static_cast<uint16_t>(g) * a + 127) / 255);
			dst[2]       = static_cast<uint8_t>((static_cast<uint16_t>(b) * a + 127) / 255);
			dst[3]       = a;
		}
	}

	if (SDL_MUSTLOCK(surface))
		SDL_UnlockSurface(surface);
	if (SDL_MUSTLOCK(alpha_surface))
		SDL_UnlockSurface(alpha_surface);

	return surface;
}
#endif

bool ONScripter::colorGlyph(const GlyphParams *key, GlyphValues *glyph, SDL_Color *color, bool border, GlyphAtlasController *atlas) {
#if defined(PIVAS) && PIVAS_CPU_GLYPH_COLOR
	// Nothing to colour for a glyph with no pixels (space etc.); the GPU path
	// below returns true for these too (src_img == nullptr). Uploading the
	// empty bitmap would create a zero-size texture: vitaGL stores
	// (height - 1) in its control word, so the texture reads back as
	// 4096x4096 with a bogus format, and the next upload into the recycled
	// image copy-on-writes 64MB out of a 0-byte buffer.
	SDL_Surface *alpha_bitmap = border ? glyph->border_bitmap : glyph->bitmap;
	if (!alpha_bitmap || alpha_bitmap->w == 0 || alpha_bitmap->h == 0)
		return true;

	SDL_Surface *surface = createColouredGlyphSurface(border ? glyph->border_bitmap : glyph->bitmap,
	                                                  *color,
	                                                  key->is_gradient && !border,
	                                                  glyph);
	if (!surface)
		return false;

	GPU_Image *image = gpu.copyImageFromSurface(surface);
	SDL_FreeSurface(surface);
	if (!image)
		return false;

	if (atlas) {
		GPU_Rect rect;
		if (!atlas->add(image->w + 2, image->h + 2, rect)) {
			sendToLog(LogLevel::Error, "ONScripter@colorGlyph: PIVAS texture atlas addition failed!\n");
			gpu.freeImage(image);
			return false;
		}

		GPU_SetBlending(image, false);
		gpu.copyGPUImage(image, nullptr, nullptr, atlas->atlas->target, rect.x + 1, rect.y + 1);
		GPU_SetBlending(image, true);
		gpu.simulateRead(atlas->atlas);

		if (border)
			glyph->border_pos.set(rect);
		else
			glyph->glyph_pos.set(rect);
		gpu.freeImage(image);
	} else {
		GPU_Image *&slot = border ? glyph->border_gpu : glyph->glyph_gpu;
		if (slot)
			gpu.freeImage(slot);
		slot = image;
		if (border)
			glyph->border_pos.unset();
		else
			glyph->glyph_pos.unset();
	}
	return true;
#endif

	// 1. atlas -> atlas (src_atlas && atlas) -> via temp image
	// 2. image -> image (!src_atlas && !atlas) -> overwrite self
	// 3. image -> atlas (!src_atlas && atlas) -> coords & colour
	// 4. atlas -> image (src_atlas && !atlas) -> overwrite image

	// is atlas a place we are blitting from
	bool src_atlas     = ((!border && glyph->glyph_pos.has()) || (border && glyph->border_pos.has()));
	GPU_Image *src_img = src_atlas ? glyphAtlas.atlas : (border ? glyph->border_gpu : glyph->glyph_gpu);
	GPU_Rect *src_rect = !src_atlas ? nullptr : (border ? &glyph->border_pos.get() : &glyph->glyph_pos.get());
	std::unique_ptr<GPU_Rect> dst_rect;

	if (src_img == nullptr || color == nullptr) {
		return true;
	}

	GPU_GetTarget(src_img);
	GPU_Target *target = src_img->target; // case 2

	float x = src_img->w / 2.0, y = src_img->h / 2.0; // cases 2 & 4
	GPU_Image *tmp{nullptr};
	bool atlas_ok{true};

	if (src_atlas && atlas) { // case 1
		tmp = gpu.createImage(src_rect->w, src_rect->h, 4);
		GPU_GetTarget(tmp);
		target   = tmp->target;
		dst_rect = std::make_unique<GPU_Rect>();
		if (atlas->add(tmp->w + 2, tmp->h + 2, *dst_rect)) {
			x = dst_rect->x + tmp->w / 2.0;
			y = dst_rect->y + tmp->h / 2.0;
		} else {
			sendToLog(LogLevel::Error, "ONScripter@colorGlyph: Texture atlas addition failed (case #1)!\n");
			atlas_ok = false;
		}
	} else if (!src_rect && atlas) { // case 3
		dst_rect = std::make_unique<GPU_Rect>();
		if (atlas->add(src_img->w + 2, src_img->h + 2, *dst_rect)) {
			x = dst_rect->x + dst_rect->w / 2.0;
			y = dst_rect->y + dst_rect->h / 2.0;
		} else {
			sendToLog(LogLevel::Error, "ONScripter@colorGlyph: Texture atlas addition failed (case #3)!\n");
			atlas_ok = false;
		}
	} else if (src_rect && !atlas) { // case 4
		GPU_Image *image = (border ? glyph->border_gpu : glyph->glyph_gpu);
		GPU_GetTarget(image);
		target = image->target;
	}

	if (!atlas_ok) {
		if (tmp)
			gpu.freeImage(tmp);
		return false;
	}

	bool src_needs_copy   = src_img->target == target && !gpu.render_to_self;
	GPU_Image *actual_src = src_img;

	if (key->is_gradient && !border) {
		if (src_needs_copy) {
			actual_src = gpu.copyImage(actual_src);
			GPU_GetTarget(actual_src);
		}

		// Add a gradient instead!
		gpu.setShaderProgram("glyphGradient.frag");
		gpu.bindImageToSlot(actual_src, 0);
		gpu.setShaderVar("color", *color);
		// Make sure we call the right overloaded version of setShaderVar(!!)
		gpu.setShaderVar("faceAscender", static_cast<int>(glyph->faceAscender));
		gpu.setShaderVar("maxy", static_cast<int>(glyph->maxy + (src_rect ? (src_rect->y) : 0)));
		gpu.setShaderVar("height", static_cast<int>(/*src_rect ? src_rect->h :*/ src_img->h));
		GPU_SetBlending(actual_src, false);
		if (tmp)
			gpu.copyGPUImage(actual_src, src_rect, nullptr, target);
		else
			gpu.copyGPUImage(actual_src, src_rect, dst_rect.get(), target, x, y, 1, 1, 0, true);

		gpu.unsetShaderProgram();
		GPU_SetBlending(actual_src, true);
		if (atlas && target == atlas->atlas->target)
			gpu.simulateRead(atlas->atlas);
	} else {
		//Don't take alpha into account
		if (color->r == 0 && color->b == 0 && color->g == 0) {
			if (!tmp) {
				gpu.multiplyAlpha(src_img, dst_rect.get());
				return true;
			}
			throw std::runtime_error("Temporary glyph texture should not have been allocated for a black glyph");
		}

		if (src_needs_copy) {
			actual_src = gpu.copyImage(actual_src);
			GPU_GetTarget(actual_src);
		}

		SDL_Color srcColor{0, 0, 0, 0};

		gpu.setShaderProgram("colorModification.frag");

		gpu.bindImageToSlot(actual_src, 0);
		GPU_SetBlending(actual_src, false);
		gpu.setShaderVar("modificationType", 7);
		gpu.setShaderVar("replaceSrcColor", srcColor);
		gpu.setShaderVar("replaceDstColor", *color);
		gpu.setShaderVar("multiplyAlpha", 1);
		if (tmp)
			gpu.copyGPUImage(actual_src, src_rect, nullptr, target);
		else
			gpu.copyGPUImage(actual_src, src_rect, dst_rect.get(), target, x, y, 1, 1, 0, true);

		gpu.unsetShaderProgram();
		GPU_SetBlending(actual_src, true);
		if (atlas && target == atlas->atlas->target)
			gpu.simulateRead(atlas->atlas);
	}

	if (tmp) {
		gpu.copyGPUImage(tmp, nullptr, dst_rect.get(), atlas->atlas->target, x, y, 1, 1, 0, true);
		gpu.freeImage(tmp);
		gpu.simulateRead(atlas->atlas);
	}

	if (dst_rect.get())
		(border ? glyph->border_pos : glyph->glyph_pos).set(*dst_rect);

	if (src_needs_copy)
		gpu.freeImage(actual_src);

	return true;
}

void ONScripter::makeNegaTarget(GPU_Target *target, GPU_Rect clip) {
	if (target == nullptr || target->image == nullptr) {
		sendToLog(LogLevel::Error, "makeNegaTarget@Target has no image\n");
		return;
	}

	gpu.setShaderProgram("colorModification.frag");
	gpu.bindImageToSlot(target->image, 0);

	gpu.setShaderVar("modificationType", 5);

	//Switch to canvas coordinate system
	clip.x += camera.center_pos.x;
	clip.y += camera.center_pos.y;

	gpu.copyGPUImage(target->image, nullptr, &clip, target);
	gpu.unsetShaderProgram();
}

void ONScripter::makeMonochromeTarget(GPU_Target *target, GPU_Rect clip, bool before_scene) {
	if (target == nullptr || target->image == nullptr) {
		sendToLog(LogLevel::Error, "makeMonochromeTarget@Target has no image\n");
		return;
	}

	gpu.setShaderProgram("colorModification.frag");
	gpu.bindImageToSlot(target->image, 0);

	gpu.setShaderVar("modificationType", 4);
	gpu.setShaderVar("greyscaleHue", monocro_color[before_scene]);

	//Switch to canvas coordinate system
	clip.x += camera.center_pos.x;
	clip.y += camera.center_pos.y;

	gpu.copyGPUImage(target->image, nullptr, &clip, target);
	gpu.unsetShaderProgram();
}

void ONScripter::makeBlurTarget(GPU_Target *target, GPU_Rect clip, bool before_scene) {
	if (target == nullptr || target->image == nullptr) {
		sendToLog(LogLevel::Error, "makeBlurTarget@Target has no image\n");
		return;
	}

	//Switch to canvas coordinate system
	clip.x += camera.center_pos.x;
	clip.y += camera.center_pos.y;

	GPUTransformableCanvasImage tmp(target->image);
	PooledGPUImage toDraw = gpu.getBlurredImage(tmp, blur_mode[before_scene]);
	gpu.copyGPUImage(toDraw.image, nullptr, &clip, target);
}

void ONScripter::makeWarpedTarget(GPU_Target *target, GPU_Rect clip, bool /*before_scene*/) {
	if (target == nullptr || target->image == nullptr) {
		sendToLog(LogLevel::Error, "makeWarpedTarget@Target has no image\n");
		return;
	}

	//Switch to canvas coordinate system
	clip.x += camera.center_pos.x;
	clip.y += camera.center_pos.y;

	GPUTransformableCanvasImage tmp(target->image);
	float secs            = warpClock.time() / 1000.0;
	PooledGPUImage toDraw = gpu.getWarpedImage(tmp, secs, warpAmplitude, warpWaveLength, warpSpeed);
	gpu.copyGPUImage(toDraw.image, nullptr, &clip, target);
}

//Adds to correct dirty rect by z level
void ONScripter::dirtyRectForZLevel(int num, GPU_Rect &rect) {
	DirtyRect *dirty = (num <= z_order_hud) ? &before_dirty_rect_hud : &before_dirty_rect_scene;
	dirty->add(rect);
	dirty = (num <= z_order_hud) ? &dirty_rect_hud : &dirty_rect_scene;
	dirty->add(rect);
}

void ONScripter::dirtySpriteRect(AnimationInfo *ai, bool before) {
	dirtySpriteRect(ai->id, ai->type == SPRITE_LSP2, before);
}

void ONScripter::dirtySpriteRect(int num, bool lsp2, bool before) {
	if (num == -1)
		return;

	DirtyRect *dirty   = (num <= z_order_hud) ? (before ? &before_dirty_rect_hud : &dirty_rect_hud) : (before ? &before_dirty_rect_scene : &dirty_rect_scene);
	AnimationInfo *spr = lsp2 ? &sprite2_info[num] : &sprite_info[num];
	spr                = (before && spr->old_ai) ? spr->old_ai : spr;
	GPU_Rect toAdd{0, 0, 0, 0};

	if (spr->parentImage.no != -1) {
		bool parentIsLsp2        = spr->parentImage.lsp2;
		AnimationInfo *parentSpr = parentIsLsp2 ? (before ? (sprite2_info[spr->parentImage.no].old_ai ? sprite2_info[spr->parentImage.no].old_ai : &sprite2_info[spr->parentImage.no]) : &sprite2_info[spr->parentImage.no]) :
		                                          (before ? (sprite_info[spr->parentImage.no].old_ai ? sprite_info[spr->parentImage.no].old_ai : &sprite_info[spr->parentImage.no]) : &sprite_info[spr->parentImage.no]);
		toAdd = parentIsLsp2 ? parentSpr->bounding_rect : parentSpr->pos;
		toAdd.x += parentSpr->camera.pos.x;
		toAdd.y += parentSpr->camera.pos.y;
		if (parentSpr->scrollable.h > 0) {
			toAdd.h = parentSpr->scrollable.h;
		}
		if (parentSpr->scrollable.w > 0) {
			toAdd.w = parentSpr->scrollable.w;
		}
	} else {
		toAdd = lsp2 ? spr->bounding_rect : spr->pos;
		toAdd.x += spr->camera.pos.x;
		toAdd.y += spr->camera.pos.y;
		if (spr->scrollable.h > 0) {
			toAdd.h = spr->scrollable.h;
		}
		if (spr->scrollable.w > 0) {
			toAdd.w = spr->scrollable.w;
		}
	}

	if (spr->spriteTransforms.breakupFactor > 0 || spr->spriteTransforms.blurFactor > 0 || std::fabs(spr->spriteTransforms.warpAmplitude) > 0) {
		dirty->fill(window.canvas_width, window.canvas_height);
	}

	dirty->add(toAdd);

	if (num > z_order_hud) {
		SpritesetInfo *cleanSet{nullptr};
		// Sets 1+
		if (z_order_spritesets.count(1) && num <= z_order_spritesets[1]) {
			//Belongs to a spriteset, we need to tell that spriteset about this
			int spriteset = 1;
			while (z_order_spritesets.count(spriteset + 1) && num <= z_order_spritesets[spriteset + 1]) {
				spriteset++;
			}
			cleanSet = &spritesets[spriteset];
		}
		// Set 0
		else if (num < z_order_ld) {
			cleanSet = &spritesets[0];
		}

		if (cleanSet) {
			//TODO: If the spriteset isNullTransform, or has pos and nothing else that might change the dirty rect like blur,
			// then simply adjust the dirty rect we add above instead of updating the whole screen
			if (before)
				before_dirty_rect_scene.fill(window.canvas_width, window.canvas_height);
			else
				dirty_rect_scene.fill(window.canvas_width, window.canvas_height);
			cleanSpritesetCache(cleanSet, before);
		}
	}

	if (!before && !spr->old_ai) {
		dirtySpriteRect(num, lsp2, true); // This sprite is on both the beforescene and afterscene -- call ourselves again to update the beforescene rects
	}
}

int ONScripter::getAIno(AnimationInfo *info, bool /*unused*/, bool &lsp2) {
	if (info == nullptr)
		return -1;

	lsp2 = false;
	if (info->type == SPRITE_LSP2)
		lsp2 = true;
	else if (info->type == SPRITE_LSP)
		lsp2 = false;
	else
		return -1;

	return info->id;
}

bool ONScripter::isHudAI(AnimationInfo *info, bool /*unused*/) {
	if (!info)
		return false;
	if (info->type == SPRITE_CURSOR || info->type == SPRITE_SENTENCE_FONT)
		return true;
	if (info->type != SPRITE_LSP && info->type != SPRITE_LSP2)
		return false;
	return info->id <= z_order_hud;
}

void ONScripter::fillCanvas(bool after, bool before) {
	if (after) {
		dirty_rect_scene.fill(window.canvas_width, window.canvas_height);
		dirty_rect_hud.fill(window.canvas_width, window.canvas_height);
	}
	if (before) {
		before_dirty_rect_scene.fill(window.canvas_width, window.canvas_height);
		before_dirty_rect_hud.fill(window.canvas_width, window.canvas_height);
	}
}

void ONScripter::resetSpritesets() {
	for (auto &id_set : spritesets) {
		cleanSpritesetCache(&id_set.second, true);
		cleanSpritesetCache(&id_set.second, false);
	}
	spritesets.clear();
}

void ONScripter::cleanSpritesetCache(SpritesetInfo *spriteset, bool before) {
	if (!spriteset)
		return;
	auto &ssim = before ? spriteset->im : spriteset->imAfterscene;
	if (!ssim.image)
		return;
	gpu.giveCanvasImage(ssim.image);
	ssim.clearImage();
}

void ONScripter::setupZLevels(int refresh_mode) {
	spriteZLevels.clear();
	for (auto ai : sprites(SPRITE_LSP | SPRITE_LSP2, true)) {
		auto spr = ai->oldNew(refresh_mode);
		if (spr->exists)
			spriteZLevels[spr->has_z_order_override ? spr->z_order_override : spr->id].insert(spr);
	}
}

// Helper function for refreshSceneTo & refreshHudTo.
void ONScripter::drawSpritesBetween(int upper_inclusive, int lower_exclusive, GPU_Target *target, GPU_Rect *clip_dst, int refresh_mode) {
	for (int i = upper_inclusive; i > lower_exclusive; i--) {
		if (refresh_mode & REFRESH_SAYA_MODE && i <= 9)
			return;

		auto z = spriteZLevels.find(i);
		if (z == spriteZLevels.end())
			continue;

		for (AnimationInfo *spr : z->second) {
			// Will iterate through sprites in correct z order using cmpById
			// Don't display:
			// LSP sprites if those are hidden
			if (spr->type == SPRITE_LSP && all_sprite_hide_flag)
				continue;
			// LSP2 sprites if those are hidden
			if (spr->type == SPRITE_LSP2 && all_sprite2_hide_flag)
				continue;
			// Sprites that have no image and don't have the excuse that they're layers
			if (!spr->exists)
				continue;
			// Invisible sprites
			if (!spr->visible)
				continue;
			// Draw it!
			drawToGPUTarget(target, spr, refresh_mode, clip_dst, spr->type == SPRITE_LSP2);
			PIVAS_TRACE_REDUCED_DRAW(target);
		}
	}
}

// This function rebuilds the game screen and blits it to the target.
void ONScripter::refreshSceneTo(GPU_Target *target, GPU_Rect *passed_script_clip_dst, int refresh_mode) {

	int &rm = refresh_mode; // We'll be passing this around a lot, let's make it short

	if (!(rm & CONSTANT_REFRESH_MODE)) {
		rm &= ~REFRESH_BEFORESCENE_MODE;
		constant_refresh_mode |= rm;
		return;
	}

	if (target == nullptr) {
		sendToLog(LogLevel::Error, "refreshSceneTo: Null target was passed\n");
		return;
	}

	if (!(rm & REFRESH_SOMETHING))
		return;

	/* Some basic variable sanity checks, which should probably be done somewhere other than here, like after define. */
	if (!(MAX_SPRITE_NUM > z_order_ld &&
	      z_order_ld > z_order_hud &&
	      z_order_hud > z_order_window &&
	      z_order_window > z_order_text &&
	      z_order_window > 0)) {
		errorAndExit("z_orders are somehow wrong. Make sure max > humanz > spriteset(1) > spriteset(2) > ... > hudz > windowz > 0.");
	}

	setupZLevels(rm);

	GPU_Rect script_clip_dst = full_script_clip;
	if (passed_script_clip_dst)
		if (doClipping(&script_clip_dst, passed_script_clip_dst))
			return;

#if defined(PIVAS) && PIVAS_RENDER_TRACE
	if (vita_effect_scene_dst && target == vita_effect_scene_dst->target)
		vita_render_trace_scene_draw_count = 0;
#endif

	AnimationInfo *bg = bg_info.oldNew(rm);
	if (bg->exists) {
		drawToGPUTarget(target, bg, rm, &script_clip_dst);
		PIVAS_TRACE_REDUCED_DRAW(target);
	}

	drawSpritesBetween(MAX_SPRITE_NUM - 1, z_order_ld, target, &script_clip_dst, rm);

	for (int i = 0; i < 3; i++) {
		AnimationInfo *tc = tachi_info[human_order[2 - i]].oldNew(rm);
		if (tc->exists) {
			drawToGPUTarget(target, tc, rm, &script_clip_dst);
			PIVAS_TRACE_REDUCED_DRAW(target);
		}
	}

	//Spritesets
	for (int spritesetNo = 0;; spritesetNo++) {
		int startZ = spritesetNo == 0 ? z_order_ld : z_order_spritesets[spritesetNo];
		int endZ   = (z_order_spritesets.count(spritesetNo + 1) == 1) ? z_order_spritesets[spritesetNo + 1] : z_order_hud;
		//sendToLog(LogLevel::Info, "(in refresh:) spritesets[%i].enable=%i\n", spritesetNo, spritesets[spritesetNo].enable);
		if (spritesetNo == 0 || spritesets[spritesetNo].isEnabled(rm & REFRESH_BEFORESCENE_MODE)) { // spriteset 0 is always active (?)
			if (spritesets[spritesetNo].isNullTransform()) {
				if (spritesetNo != 0)
					gpu.clearWholeTarget(target, 0, 0, 0, 255);
				// If the spriteset's properties are all default, just blit all the elements individually straight onto the target for efficiency
				drawSpritesBetween(startZ, endZ, target, &script_clip_dst, rm);
			} else {
				// Make the spriteset's image, if it doesn't exist
				// (if it does exist, it will be up-to-date, because the image is deleted by cleanSpritesetCache if spriteset or any of its elements change)
				auto &ssim = rm & REFRESH_BEFORESCENE_MODE ? spritesets[spritesetNo].im : spritesets[spritesetNo].imAfterscene;
				if (!ssim.image) {
					GPU_Image *spritesetImage = gpu.getCanvasImage();
					if (spritesetNo != 0)
						gpu.clearWholeTarget(spritesetImage->target, 0, 0, 0, 255); // give black bg to all spritesets except 0 (0 would just be illogical, it would prevent bg and ld entirely)
					GPU_Rect full_rect = full_script_clip;
					drawSpritesBetween(startZ, endZ, spritesetImage->target, &full_rect, rm);
					(rm & REFRESH_BEFORESCENE_MODE ? spritesets[spritesetNo].im : spritesets[spritesetNo].imAfterscene) = GPUTransformableCanvasImage(spritesetImage);
				}
				// draw the spriteset to target
				drawSpritesetToGPUTarget(target, &spritesets[spritesetNo], &script_clip_dst, rm);
				PIVAS_TRACE_REDUCED_DRAW(target);
			}
		}
		if (endZ == z_order_hud)
			break;
	}

	//Apply nega in the end of normal rebuild
	bool before = rm & REFRESH_BEFORESCENE_MODE;

	if (nega_mode[before] == 1)
		makeNegaTarget(target, script_clip_dst);
	if (monocro_flag[before])
		makeMonochromeTarget(target, script_clip_dst, before);
	if (nega_mode[before] == 2)
		makeNegaTarget(target, script_clip_dst);
	if (blur_mode[before] > 0)
		makeBlurTarget(target, script_clip_dst, before);
	if (std::fabs(warpAmplitude) > 0)
		makeWarpedTarget(target, script_clip_dst, before);

	//sendToLog(LogLevel::Info, "enddraw\n");
}

void ONScripter::refreshHudTo(GPU_Target *target, GPU_Rect *passed_script_clip_dst, int refresh_mode) {

	/* a) make sure textwindow renders properly and according to its position. 
Including leaveTextMode and enterTextMode (all the sprites that should be 
above textwindow are indeed above textwindow while it transitions or text renders)

b) text is always rendered on the top of any sprites excluding buttons, 
this can lead to a possible glitch, but dammit, who is the mad man to 
use buttons & text this way */

	int &rm = refresh_mode; // We'll be passing this around a lot, let's make it short

	if (!(rm & CONSTANT_REFRESH_MODE)) {
		rm &= ~REFRESH_BEFORESCENE_MODE;
		constant_refresh_mode |= rm;
		return;
	}

	if (target == nullptr) {
		sendToLog(LogLevel::Error, "refreshHudTo: Null target was passed\n");
		return;
	}

	if (target->w != window.canvas_width || target->h != window.canvas_height) {
		sendToLog(LogLevel::Error, "refreshHudTo: not canvas dst\n");
		return;
	}

	if (!(rm & REFRESH_SOMETHING))
		return;

#if defined(PIVAS)
#if PIVAS_DYNAMIC_WINDOW_TRACE
	auto pivasHudTargetName = [&]() -> const char * {
		if (hud_gpu && target == hud_gpu->target)
			return "hud_gpu";
		if (text_gpu && target == text_gpu->target)
			return "text_gpu";
		if (window_gpu && target == window_gpu->target)
			return "window_gpu";
		if (vita_effect_hud_dst && target == vita_effect_hud_dst->target)
			return "vita_effect_hud_dst";
		if (screen_target && target == screen_target)
			return "screen";
		return "other";
	};
#endif
	// pivasScriptWindowHidden: an intended script hide (textoff + input
	// wait) must not be escalated back to WINDOW|TEXT via the sticky click
	// terms — see combineWithCamera's matching suppression.
	bool pivas_text_hud_live =
	    ((display_mode & DISPLAY_MODE_TEXT) ||
	     page_enter_status != 0 ||
	     dlgCtrl.dialogueProcessingState.active ||
	     dlgCtrl.dialogueIsRendering ||
	     clickstr_state != CLICK_NONE ||
	     textgosub_clickstr_state != CLICK_NONE) &&
	    !pivasScriptWindowHidden();
	bool pivas_atomic_text_hud =
	    pivas_text_hud_live &&
	    (rm & REFRESH_NORMAL_MODE) &&
	    ((rm & (REFRESH_WINDOW_MODE | REFRESH_TEXT_MODE | REFRESH_CURSOR_MODE)) ||
	     // Post-textoff constant refreshes carry bare REFRESH_NORMAL_MODE
	     // (display_mode is NORMAL so refreshMode() drops WINDOW|TEXT), yet
	     // combineWithCamera still draws the dynamic box art via the sticky
	     // eligibility terms. Invariant: wherever the box art persists, the
	     // committed text cache (re-validated by the END commit) must
	     // persist with it. The hold timer is the box-persistence signal:
	     // re-armed through advance gaps and set_name_window/setwindowd
	     // re-setups, zeroed by a genuine textoff, so the hud recompose
	     // after a textoff erases the text instead of redrawing it.
	     (wndCtrl.usingDynamicTextWindow && pivas_final_window_seen_live_dialogue &&
	      SDL_GetTicks() < pivas_final_window_hold_until));
#if PIVAS_TEXT_CACHE_TRACE
	int pivas_rm_before_atomic = rm;
#endif
	if (pivas_atomic_text_hud) {
		rm |= REFRESH_WINDOW_MODE | REFRESH_TEXT_MODE;
		if (draw_cursor_flag || clickstr_state != CLICK_NONE || textgosub_clickstr_state != CLICK_NONE)
			rm |= REFRESH_CURSOR_MODE;
		passed_script_clip_dst = nullptr;
	}
#endif

#if defined(PIVAS) && PIVAS_RENDER_TRACE
	if (pivas_hud_refresh_trace_count++ < 512) {
		const char *targetName = "other";
		if (hud_gpu && target == hud_gpu->target)
			targetName = "hud_gpu";
		else if (vita_effect_hud_dst && target == vita_effect_hud_dst->target)
			targetName = "vita_effect_hud_dst";
		sendToLog(LogLevel::Info,
		          "[hud] refresh target=%s rm=0x%x rwtm=0x%x display=0x%x page_enter=%d draw_cursor=%d click=%d textgosub_click=%d window=%d text=%d cursor=%d dynamic=%d dlg_active=%d dlg_rendering=%d seg=%d dlg_bounds=%g,%g,%g,%g text_gpu=%dx%d window_gpu=%dx%d\n",
		          targetName, rm,
		          refresh_window_text_mode,
		          display_mode,
		          page_enter_status,
		          draw_cursor_flag ? 1 : 0,
		          clickstr_state,
		          textgosub_clickstr_state,
		          (rm & REFRESH_WINDOW_MODE) ? 1 : 0,
		          (rm & REFRESH_TEXT_MODE) ? 1 : 0,
		          (rm & REFRESH_CURSOR_MODE) ? 1 : 0,
		          wndCtrl.usingDynamicTextWindow ? 1 : 0,
		          dlgCtrl.dialogueProcessingState.active ? 1 : 0,
		          dlgCtrl.dialogueIsRendering ? 1 : 0,
		          dlgCtrl.dialogueRenderState.segmentIndex,
		          dlgCtrl.dialogueRenderState.bounds.x,
		          dlgCtrl.dialogueRenderState.bounds.y,
		          dlgCtrl.dialogueRenderState.bounds.w,
		          dlgCtrl.dialogueRenderState.bounds.h,
		          text_gpu ? text_gpu->w : 0,
		          text_gpu ? text_gpu->h : 0,
		          window_gpu ? window_gpu->w : 0,
		          window_gpu ? window_gpu->h : 0);
	}
	if (vita_effect_hud_dst && target == vita_effect_hud_dst->target)
		vita_render_trace_hud_draw_count = 0;
#endif

	if (display_draw) {
		gpu.copyGPUImage(draw_screen_gpu, nullptr, nullptr, target, (target->w - draw_screen_gpu->w) / 2.0, (target->h - draw_screen_gpu->h) / 2.0);
		PIVAS_TRACE_REDUCED_DRAW(target);
		return;
	}

	GPU_Rect script_clip_dst = full_script_clip;
	if (passed_script_clip_dst)
		if (doClipping(&script_clip_dst, passed_script_clip_dst))
			return;

	GPU_Rect canvas_clip_dst = script_clip_dst;
	canvas_clip_dst.x += camera.center_pos.x;
	canvas_clip_dst.y += camera.center_pos.y;

#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_TRACE
	if (wndCtrl.usingDynamicTextWindow &&
	    pivas_dynamic_hud_lifecycle_trace_count++ < 220) {
		sendToLog(LogLevel::Info,
		          "[dyn-window-hud] pre-clear target=%s rm=0x%x atomic_live=%d display=0x%x page_enter=%d active=%d rendering=%d page_cleared=%d script_clip=%g,%g,%g,%g canvas_clip=%g,%g,%g,%g window=%d text=%d cursor=%d text_valid=%d gen=%u cached_gen=%u seg=%d cached_seg=%d\n",
		          pivasHudTargetName(),
		          rm,
		          pivas_text_hud_live ? 1 : 0,
		          display_mode,
		          page_enter_status,
		          dlgCtrl.dialogueProcessingState.active ? 1 : 0,
		          dlgCtrl.dialogueIsRendering ? 1 : 0,
		          dlgCtrl.vitaPageClearedByDialogue ? 1 : 0,
		          script_clip_dst.x,
		          script_clip_dst.y,
		          script_clip_dst.w,
		          script_clip_dst.h,
		          canvas_clip_dst.x,
		          canvas_clip_dst.y,
		          canvas_clip_dst.w,
		          canvas_clip_dst.h,
		          (rm & REFRESH_WINDOW_MODE) ? 1 : 0,
		          (rm & REFRESH_TEXT_MODE) ? 1 : 0,
		          (rm & REFRESH_CURSOR_MODE) ? 1 : 0,
		          vita_text_gpu_valid ? 1 : 0,
		          vita_text_gpu_dialogue_generation,
		          vita_text_gpu_cached_generation,
		          dlgCtrl.dialogueRenderState.segmentIndex,
		          vita_text_gpu_last_segment);
	}
#endif

#if defined(PIVAS) && PIVAS_TEXT_CACHE_TRACE
	if (pivas_atomic_text_hud && dlgCtrl.vitaOpeningDialogueLabel &&
	    pivas_atomic_hud_trace_count++ < 192) {
		sendToLog(LogLevel::Info,
		          "[hud-atomic] d%d gen=%u rm=0x%x->0x%x target=%dx%d script_clip=%g,%g,%g,%g canvas_clip=%g,%g,%g,%g display=0x%x page_enter=%d active=%d rendering=%d seg=%d bounds=%g,%g,%g,%g click=%d textgosub_click=%d cursor=%d\n",
		          dlgCtrl.vitaOpeningDialogueLabel,
		          vita_text_gpu_dialogue_generation,
		          pivas_rm_before_atomic,
		          rm,
		          target->w,
		          target->h,
		          script_clip_dst.x,
		          script_clip_dst.y,
		          script_clip_dst.w,
		          script_clip_dst.h,
		          canvas_clip_dst.x,
		          canvas_clip_dst.y,
		          canvas_clip_dst.w,
		          canvas_clip_dst.h,
		          display_mode,
		          page_enter_status,
		          dlgCtrl.dialogueProcessingState.active ? 1 : 0,
		          dlgCtrl.dialogueIsRendering ? 1 : 0,
		          dlgCtrl.dialogueRenderState.segmentIndex,
		          dlgCtrl.dialogueRenderState.bounds.x,
		          dlgCtrl.dialogueRenderState.bounds.y,
		          dlgCtrl.dialogueRenderState.bounds.w,
		          dlgCtrl.dialogueRenderState.bounds.h,
		          clickstr_state,
		          textgosub_clickstr_state,
		          draw_cursor_flag ? 1 : 0);
	}
#endif

	//hud has no background, so we have to set a clip rect and clear it before we can draw onto it.
	gpu.setClipRect(target, canvas_clip_dst);
#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_TRACE
	if (wndCtrl.usingDynamicTextWindow)
		pivasDrainDynamicHudGlErrors("hud-clear");
#endif
	gpu.clear(target);
#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_TRACE
	if (wndCtrl.usingDynamicTextWindow)
		pivasCheckDynamicHudGlError("hud-clear");
#endif
	GPU_UnsetClip(target);
#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_TRACE
	if (wndCtrl.usingDynamicTextWindow &&
	    pivas_dynamic_hud_lifecycle_trace_count++ < 220) {
		sendToLog(LogLevel::Info,
		          "[dyn-window-hud] cleared target=%s canvas_clip=%g,%g,%g,%g rm=0x%x\n",
		          pivasHudTargetName(),
		          canvas_clip_dst.x,
		          canvas_clip_dst.y,
		          canvas_clip_dst.w,
		          canvas_clip_dst.h,
		          rm);
	}
#endif

	//canvas_clip_dst is used for text only which doesn't occupy the whole canvas
	GPU_Rect middle_of_canvas{camera.center_pos.x, camera.center_pos.y, static_cast<float>(window.script_width), static_cast<float>(window.script_height)};
	doClipping(&canvas_clip_dst, &middle_of_canvas);

#if defined(PIVAS) && PIVAS_HUD_OUTPUT_PROBE
	bool pivas_hud_probe_drawn = false;
	auto pivasDrawHudOutputProbe = [&](const char *stage) {
		if (pivas_hud_probe_drawn)
			return;
		if (!(rm & (REFRESH_WINDOW_MODE | REFRESH_TEXT_MODE)))
			return;

		bool text_hud_live =
		    (display_mode & DISPLAY_MODE_TEXT) ||
		    page_enter_status != 0 ||
		    dlgCtrl.dialogueProcessingState.active ||
		    dlgCtrl.dialogueIsRendering ||
		    clickstr_state != CLICK_NONE ||
		    textgosub_clickstr_state != CLICK_NONE;
		if (!text_hud_live)
			return;

		GPU_Rect probe{};
		const GPU_Rect &bounds = dlgCtrl.dialogueRenderState.bounds;
		if (bounds.w > 0 && bounds.h > 0) {
			probe.x = bounds.x + camera.center_pos.x;
			probe.y = bounds.y + camera.center_pos.y;
			probe.w = bounds.w;
			probe.h = bounds.h < 32 ? 32 : bounds.h;
		} else {
			float margin = window.script_width * 0.08f;
			probe.x      = camera.center_pos.x + margin;
			probe.y      = camera.center_pos.y + window.script_height - 170.0f;
			probe.w      = window.script_width - margin * 2.0f;
			probe.h      = 96.0f;
		}

		if (probe.w < 64 || probe.h < 16)
			return;
		if (doClipping(&probe, &canvas_clip_dst))
			return;

		const float gap = 4.0f;
		if (probe.h < 116.0f)
			probe.h = 116.0f;
		if (doClipping(&probe, &canvas_clip_dst))
			return;

		float tile_h = (probe.h - gap) / 2.0f;
		if (tile_h > 52.0f)
			tile_h = 52.0f;
		float tile_w = (probe.w - gap * 4.0f) / 5.0f;
		if (tile_w < 20.0f || tile_h < 20.0f)
			return;

		GPU_UnsetClip(target);
		auto prepareProbeTexture = [&](GPU_Image *&image) {
			if (image)
				return image;

			SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, 128, 128, 32, SDL_PIXELFORMAT_RGBA32);
			if (!surface)
				return image;

			auto fillRect = [&](int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
				SDL_Rect rect{x, y, w, h};
				SDL_FillRect(surface, &rect, SDL_MapRGBA(surface->format, r, g, b, 255));
			};

			fillRect(0, 0, 64, 64, 255, 0, 0);
			fillRect(64, 0, 64, 64, 0, 255, 0);
			fillRect(0, 64, 64, 64, 0, 0, 255);
			fillRect(64, 64, 64, 64, 255, 255, 0);

			image = gpu.copyImageFromSurface(surface);
			SDL_FreeSurface(surface);
			if (!image)
				return image;

			GPU_SetBlending(image, false);
			GPU_SetImageFilter(image, GPU_FILTER_NEAREST);
			return image;
		};

		auto prepareAtlasTexture = [&](GPU_Image *&atlas, GPU_Image *source) {
			if (atlas || !source)
				return atlas;

			atlas = gpu.createImage(192, 48, 4);
			if (!atlas)
				return atlas;
			GPU_GetTarget(atlas);
			gpu.clearWholeTarget(atlas->target, 0, 0, 0, 255);
			GPU_SetBlending(atlas, false);
			GPU_SetImageFilter(atlas, GPU_FILTER_NEAREST);

			GPU_bool old_blending       = source->use_blending;
			GPU_BlendMode old_blendmode = source->blend_mode;
			GPU_SetBlending(source, false);

			GPU_Rect src_rects[] = {
			    {0, 0, 64, 64},
			    {64, 0, 64, 64},
			    {0, 64, 64, 64},
			    {64, 64, 64, 64},
			};
			for (int i = 0; i < 4; i++) {
				GPU_Rect src_copy = src_rects[i];
				float dst_x       = 8.0f + i * 44.0f + 16.0f;
				float dst_y       = 24.0f;
				gpu.copyGPUImage(source, &src_copy, nullptr, atlas->target,
				                 dst_x, dst_y, 0.5f, 0.5f, 0, true);
			}

			source->blend_mode = old_blendmode;
			GPU_SetBlending(source, old_blending);
			return atlas;
		};

		auto drawProbeTexture = [&](GPU_Image *image, const GPU_Rect &src, const GPU_Rect &dst) {
			if (!image)
				return;

			GPU_bool old_blending       = image->use_blending;
			GPU_BlendMode old_blendmode = image->blend_mode;
			GPU_SetBlending(image, false);

			GPU_Rect src_copy = src;
			float scale_x     = dst.w / src_copy.w;
			float scale_y     = dst.h / src_copy.h;
			gpu.copyGPUImage(image, &src_copy, &canvas_clip_dst, target,
			                 dst.x + dst.w / 2.0f,
			                 dst.y + dst.h / 2.0f,
			                 scale_x,
			                 scale_y,
			                 0,
			                 true);

			image->blend_mode = old_blendmode;
			GPU_SetBlending(image, old_blending);
		};

		GPU_Image *probe_texture = prepareProbeTexture(pivas_hud_probe_quadrants);
		GPU_Image *atlas_texture = prepareAtlasTexture(pivas_hud_probe_atlas, probe_texture);
		GPU_Rect src_full{0, 0, 128, 128};
		GPU_Rect src_tl{0, 0, 64, 64};
		GPU_Rect src_tr{64, 0, 64, 64};
		GPU_Rect src_bl{0, 64, 64, 64};
		GPU_Rect src_br{64, 64, 64, 64};
		GPU_Rect atlas_tl{8, 8, 32, 32};
		GPU_Rect atlas_tr{52, 8, 32, 32};
		GPU_Rect atlas_bl{96, 8, 32, 32};
		GPU_Rect atlas_br{140, 8, 32, 32};
		GPU_Rect dst0{probe.x, probe.y, tile_w, tile_h};
		GPU_Rect dst1{probe.x + (tile_w + gap), probe.y, tile_w, tile_h};
		GPU_Rect dst2{probe.x + (tile_w + gap) * 2.0f, probe.y, tile_w, tile_h};
		GPU_Rect dst3{probe.x + (tile_w + gap) * 3.0f, probe.y, tile_w, tile_h};
		GPU_Rect dst4{probe.x + (tile_w + gap) * 4.0f, probe.y, tile_w, tile_h};
		float row2_y = probe.y + tile_h + gap;
		GPU_Rect dst5{probe.x, row2_y, tile_w, tile_h};
		GPU_Rect dst6{probe.x + (tile_w + gap), row2_y, tile_w, tile_h};
		GPU_Rect dst7{probe.x + (tile_w + gap) * 2.0f, row2_y, tile_w, tile_h};
		GPU_Rect dst8{probe.x + (tile_w + gap) * 3.0f, row2_y, tile_w, tile_h};
		GPU_Rect dst9{probe.x + (tile_w + gap) * 4.0f, row2_y, tile_w, tile_h};
		drawProbeTexture(probe_texture, src_full, dst0);
		drawProbeTexture(probe_texture, src_tl, dst1);
		drawProbeTexture(probe_texture, src_tr, dst2);
		drawProbeTexture(probe_texture, src_bl, dst3);
		drawProbeTexture(probe_texture, src_br, dst4);
		drawProbeTexture(atlas_texture, src_full, dst5);
		drawProbeTexture(atlas_texture, atlas_tl, dst6);
		drawProbeTexture(atlas_texture, atlas_tr, dst7);
		drawProbeTexture(atlas_texture, atlas_bl, dst8);
		drawProbeTexture(atlas_texture, atlas_br, dst9);

		if (pivas_hud_output_probe_count++ < 16) {
			sendToLog(LogLevel::Info,
			          "[hud-probe] atlas-roundtrip stage=%s rm=0x%x target=%dx%d probe=%g,%g,%g,%g tile=%gx%g page_enter=%d active=%d rendering=%d source=%p atlas=%p expected=row1 full,red,green,blue,yellow row2 atlas-full,red,green,blue,yellow\n",
			          stage ? stage : "(null)",
			          rm,
			          target->w,
			          target->h,
			          probe.x,
			          probe.y,
			          probe.w,
			          probe.h,
			          tile_w,
			          tile_h,
			          page_enter_status,
			          dlgCtrl.dialogueProcessingState.active ? 1 : 0,
			          dlgCtrl.dialogueIsRendering ? 1 : 0,
			          static_cast<void *>(probe_texture),
			          static_cast<void *>(atlas_texture));
		}
		pivas_hud_probe_drawn = true;
	};
#endif

	drawSpritesBetween(z_order_hud, z_order_window, target, &script_clip_dst, rm);

	if (refresh_mode & REFRESH_WINDOW_MODE) {
#if defined(PIVAS) && PIVAS_RENDER_TRACE
		if (pivas_hud_branch_trace_count++ < 160) {
			sendToLog(LogLevel::Info,
			          "[hud] window branch dynamic=%d dlg_active=%d rm=0x%x target=%dx%d clip=%g,%g,%g,%g canvas_clip=%g,%g,%g,%g\n",
			          wndCtrl.usingDynamicTextWindow ? 1 : 0,
			          dlgCtrl.dialogueProcessingState.active ? 1 : 0,
			          refresh_mode,
			          target->w,
			          target->h,
			          script_clip_dst.x,
			          script_clip_dst.y,
			          script_clip_dst.w,
			          script_clip_dst.h,
			          canvas_clip_dst.x,
			          canvas_clip_dst.y,
			          canvas_clip_dst.w,
			          canvas_clip_dst.h);
		}
#endif
		if (wndCtrl.usingDynamicTextWindow) {
			if (!dlgCtrl.dialogueProcessingState.active) {
#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_TRACE
				if (pivas_dynamic_hud_lifecycle_trace_count++ < 220) {
					sendToLog(LogLevel::Info,
					          "[dyn-window-hud] window-branch inactive-copy target=%s rm=0x%x canvas_clip=%g,%g,%g,%g text_valid=%d gen=%u cached_gen=%u\n",
					          pivasHudTargetName(),
					          refresh_mode,
					          canvas_clip_dst.x,
					          canvas_clip_dst.y,
					          canvas_clip_dst.w,
					          canvas_clip_dst.h,
					          vita_text_gpu_valid ? 1 : 0,
					          vita_text_gpu_dialogue_generation,
					          vita_text_gpu_cached_generation);
				}
#endif
#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_TRACE
				pivasDrainDynamicHudGlErrors("inactive-window-copy");
#endif
				gpu.copyGPUImage(window_gpu, nullptr, &canvas_clip_dst, target, camera.center_pos.x, camera.center_pos.y);
#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_TRACE
				pivasCheckDynamicHudGlError("inactive-window-copy");
#endif
				PIVAS_TRACE_REDUCED_DRAW(target);
			} else {
#if defined(PIVAS)
				if (dlgCtrl.vitaPageClearedByDialogue) {
#if defined(PIVAS_DYNAMIC_WINDOW_TRACE) && PIVAS_DYNAMIC_WINDOW_TRACE
					if (pivas_dynamic_hud_lifecycle_trace_count++ < 220) {
						sendToLog(LogLevel::Info,
						          "[dyn-window-hud] window-branch active-suppressed-page-cleared target=%s rm=0x%x\n",
						          pivasHudTargetName(),
						          refresh_mode);
					}
#endif
#if defined(PIVAS) && PIVAS_HUD_OUTPUT_PROBE
					if (!(refresh_mode & REFRESH_TEXT_MODE))
						pivasDrawHudOutputProbe("window-cleared");
#endif
				} else
#endif
				{
					wndCtrl.updateTextboxExtension(true);
#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_TRACE
					if (pivas_dynamic_hud_lifecycle_trace_count++ < 220) {
						sendToLog(LogLevel::Info,
						          "[dyn-window-hud] window-branch active-render target=%s rm=0x%x canvas_clip=%g,%g,%g,%g extension=%d\n",
						          pivasHudTargetName(),
						          refresh_mode,
						          canvas_clip_dst.x,
						          canvas_clip_dst.y,
						          canvas_clip_dst.w,
						          canvas_clip_dst.h,
					          wndCtrl.extension);
					}
#endif
#if !defined(PIVAS) || !PIVAS_DYNAMIC_WINDOW_FINAL_DIRECT_ART || !PIVAS_DYNAMIC_WINDOW_SKIP_OFFSCREEN_WINDOW_ART
					renderDynamicTextWindow(target, &canvas_clip_dst, rm);
					PIVAS_TRACE_REDUCED_DRAW(target);
#endif
#if defined(PIVAS) && PIVAS_HUD_OUTPUT_PROBE
					if (!(refresh_mode & REFRESH_TEXT_MODE))
						pivasDrawHudOutputProbe("window-dynamic");
#endif
				}
			}
		} else {
			AnimationInfo *si = sentence_font_info.oldNew(rm);
			if (si->exists) {
				drawToGPUTarget(target, si, rm, &script_clip_dst);
				PIVAS_TRACE_REDUCED_DRAW(target);
#if defined(PIVAS) && PIVAS_HUD_OUTPUT_PROBE
				if (!(refresh_mode & REFRESH_TEXT_MODE))
					pivasDrawHudOutputProbe("window-static");
#endif
			}
		}
	}

	drawSpritesBetween(z_order_window, z_order_text, target, &script_clip_dst, rm);

	AnimationInfo *spr;
	if (!(refresh_mode & REFRESH_SAYA_MODE)) {
		for (auto &i : bar_info)
			if (i && (spr = i->oldNew(rm))) {
				drawToGPUTarget(target, spr, rm, &script_clip_dst);
				PIVAS_TRACE_REDUCED_DRAW(target);
			}
		for (auto &i : prnum_info)
			if (i && (spr = i->oldNew(rm))) {
				drawToGPUTarget(target, spr, rm, &script_clip_dst);
				PIVAS_TRACE_REDUCED_DRAW(target);
			}
	}

	if (refresh_mode & REFRESH_TEXT_MODE) {
#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_TRACE
		if (wndCtrl.usingDynamicTextWindow &&
		    pivas_dynamic_hud_lifecycle_trace_count++ < 220) {
			sendToLog(LogLevel::Info,
			          "[dyn-window-hud] text-branch-after-window target=%s rm=0x%x active=%d rendering=%d text_valid=%d seg=%d canvas_clip=%g,%g,%g,%g\n",
			          pivasHudTargetName(),
			          refresh_mode,
			          dlgCtrl.dialogueProcessingState.active ? 1 : 0,
			          dlgCtrl.dialogueIsRendering ? 1 : 0,
			          vita_text_gpu_valid ? 1 : 0,
			          dlgCtrl.dialogueRenderState.segmentIndex,
			          canvas_clip_dst.x,
			          canvas_clip_dst.y,
			          canvas_clip_dst.w,
			          canvas_clip_dst.h);
		}
#endif
#if defined(PIVAS) && PIVAS_RENDER_TRACE
		if (pivas_hud_branch_trace_count++ < 160) {
			sendToLog(LogLevel::Info,
			          "[hud] text branch active=%d rendering=%d rm=0x%x target=%dx%d canvas_clip=%g,%g,%g,%g\n",
			          dlgCtrl.dialogueProcessingState.active ? 1 : 0,
			          dlgCtrl.dialogueIsRendering ? 1 : 0,
			          refresh_mode,
			          target->w,
			          target->h,
			          canvas_clip_dst.x,
			          canvas_clip_dst.y,
			          canvas_clip_dst.w,
			          canvas_clip_dst.h);
		}
#endif
		dlgCtrl.renderDialogueToTarget(target, &canvas_clip_dst, rm);
		PIVAS_TRACE_REDUCED_DRAW(target);
#if defined(PIVAS) && PIVAS_HUD_OUTPUT_PROBE
		pivasDrawHudOutputProbe("text");
#endif
	}

	if (refresh_mode & REFRESH_CURSOR_MODE && !textgosub_label && !enable_custom_cursors) {
		if (clickstr_state == CLICK_WAIT) {
			drawToGPUTarget(target, cursor_info[CURSOR_WAIT_NO].oldNew(rm), rm, &script_clip_dst);
			PIVAS_TRACE_REDUCED_DRAW(target);
		} else if (clickstr_state == CLICK_NEWPAGE) {
#if defined(PIVAS) && PIVAS_SUPPRESS_NEWPAGE_CURSOR
			if (!wndCtrl.usingDynamicTextWindow)
#endif
			{
				drawToGPUTarget(target, cursor_info[CURSOR_NEWPAGE_NO].oldNew(rm), rm, &script_clip_dst);
				PIVAS_TRACE_REDUCED_DRAW(target);
			}
		}
	}

	drawSpritesBetween(z_order_text, -1, target, &script_clip_dst, rm);

	ButtonLink *p_button_link = root_button_link.next;
	while (p_button_link) {
		ButtonLink *cur_button_link = p_button_link;
		while (cur_button_link) {
			if (cur_button_link->show_flag) {
				drawToGPUTarget(target, cur_button_link->anim->oldNew(rm), rm, &script_clip_dst);
				PIVAS_TRACE_REDUCED_DRAW(target);
			}
			cur_button_link = cur_button_link->same;
		}
		p_button_link = p_button_link->next;
	}
}

void ONScripter::refreshSprite(int sprite_no, bool active_flag,
                               int cell_no, GPU_Rect *check_src_rect,
                               GPU_Rect *check_dst_rect) {
	if ((sprite_info[sprite_no].image_name ||
	     ((sprite_info[sprite_no].trans_mode == AnimationInfo::TRANS_STRING) &&
	      sprite_info[sprite_no].file_name)) &&
	    ((sprite_info[sprite_no].visible != active_flag) ||
	     ((cell_no >= 0) && (sprite_info[sprite_no].current_cell != cell_no)) ||
	     (doClipping(check_src_rect, &sprite_info[sprite_no].pos) == 0) ||
	     (doClipping(check_dst_rect, &sprite_info[sprite_no].pos) == 0))) {
		if (cell_no >= 0)
			sprite_info[sprite_no].setCell(cell_no);

		sprite_info[sprite_no].visible = active_flag;

		dirtySpriteRect(sprite_no, false);
	}
}

void ONScripter::createBackground() {
	bg_info.type = SPRITE_BG;
	// Default bg should have 1 cell, black colour, and COPY
	// bg_info.trans_mode = AnimationInfo::TRANS_COPY; // set by constructor / previous bg
	bg_info.num_of_cells = 1;
	bg_info.color        = {};
	bg_info.deleteImage();

	if (equalstr(bg_info.file_name, "white")) {
		bg_info.color = {0xff, 0xff, 0xff};
	} else if (bg_info.file_name[0] == '#') {
		readColor(&bg_info.color, bg_info.file_name);
	} else if (!equalstr(bg_info.file_name, "black") != 0) {
		script_h.setStr(&bg_info.image_name, bg_info.file_name);
		parseTaggedString(&bg_info);
		// Enforce cell number and trans_mode after parsing
		bg_info.trans_mode   = AnimationInfo::TRANS_COPY;
		bg_info.num_of_cells = 1;
		setupAnimationInfo(&bg_info);

		if (bg_info.image_surface) {
			SDL_FreeSurface(bg_info.image_surface);
			bg_info.image_surface = nullptr;
		}

		if (bg_info.gpu_image) {
			bg_info.pos.x = static_cast<float>((window.script_width - bg_info.gpu_image->w) / 2);
			bg_info.pos.y = static_cast<float>((window.script_height - bg_info.gpu_image->h) / 2);
			bg_info.pos.w = bg_info.gpu_image->w;
			bg_info.pos.h = bg_info.gpu_image->h;
		}
	}

	if (!bg_info.gpu_image) {
		// Wrapping will stretch it automatically
		bg_info.gpu_image = gpu.createImage(1, 1, 3);
		GPU_GetTarget(bg_info.gpu_image);
		gpu.clearWholeTarget(bg_info.gpu_image->target, bg_info.color.x, bg_info.color.y, bg_info.color.z, 0xff);
		bg_info.pos = full_script_clip;
	}

	bg_info.exists = true;
}

void ONScripter::loadBreakupCellforms() {
#if defined(PIVAS) && PIVAS_INSTANT_REDUCE_MOTION_EFFECTS
	if (reduce_motion) {
		sendToLog(LogLevel::Info, "PIVAS: skipping breakup-cellforms resource under reduce-motion\n");
		return;
	}
#endif
	const InternalResource *breakup_cellforms_res = getResource("breakup-cellforms.png");
	if (breakup_cellforms_res) {
		SDL_RWops *rwcells               = SDL_RWFromConstMem(breakup_cellforms_res->buffer,
                                                static_cast<int>(breakup_cellforms_res->size));
		SDL_Surface *breakup_cellforms_s = IMG_Load_RW(rwcells, 0);
		breakup_cellforms_gpu            = gpu.copyImageFromSurface(breakup_cellforms_s);
		SDL_FreeSurface(breakup_cellforms_s);
	} else {
		sendToLog(LogLevel::Error, "breakup-cellforms.png not loaded resource compilation broken\n");
	}
}

void ONScripter::loadDrawImages() {
	if (!draw_gpu) {
		draw_gpu = gpu.createImage(window.script_width, window.script_height, 4);
		GPU_GetTarget(draw_gpu);
	}
	if (!draw_screen_gpu) {
		draw_screen_gpu = gpu.createImage(window.script_width, window.script_height, 4);
		GPU_GetTarget(draw_screen_gpu);
	}
}

void ONScripter::unloadDrawImages() {
	display_draw = false;

	if (draw_gpu) {
		gpu.freeImage(draw_gpu);
		draw_gpu = nullptr;
	}

	if (draw_screen_gpu) {
		gpu.freeImage(draw_screen_gpu);
		draw_screen_gpu = nullptr;
	}
}

void ONScripter::clearDrawImages(int r, int g, int b, bool clear_screen) {
	loadDrawImages();
	gpu.clearWholeTarget(draw_gpu->target, r, g, b, 0xff);
	if (clear_screen) {
		gpu.clearWholeTarget(draw_screen_gpu->target, r, g, b, 0xff);
		display_draw = true;
	}
}
