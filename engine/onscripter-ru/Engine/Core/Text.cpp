/**
 *  Text.cpp
 *  ONScripter-RU
 *
 *  Text parser and tag converter.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Core/ONScripter.hpp"
#include "Engine/Components/Async.hpp"
#include "Engine/Components/Fonts.hpp"
#include "Engine/Components/Window.hpp"
#include "Engine/Graphics/Common.hpp"
#include "Support/Unicode.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

#if defined(PIVAS)
#include <GLES2/gl2.h>

// 1 = CPU-composed window (see ONScripter.hpp note at
// PIVAS_DYNAMIC_WINDOW_FINAL_DIRECT_ART for why vanilla stays off).
#ifndef PIVAS_CPU_DYNAMIC_TEXT_WINDOW
#define PIVAS_CPU_DYNAMIC_TEXT_WINDOW 1
#endif

#ifndef PIVAS_DYNAMIC_WINDOW_GL_TRACE
#define PIVAS_DYNAMIC_WINDOW_GL_TRACE 0
#endif

#ifndef PIVAS_DYNAMIC_WINDOW_GEOMETRY_FALLBACK
#define PIVAS_DYNAMIC_WINDOW_GEOMETRY_FALLBACK 0
#endif

#ifndef PIVAS_DYNAMIC_WINDOW_SOLID_FALLBACK
#define PIVAS_DYNAMIC_WINDOW_SOLID_FALLBACK 0
#endif

#ifndef PIVAS_DYNAMIC_WINDOW_CLEAR_FALLBACK
#define PIVAS_DYNAMIC_WINDOW_CLEAR_FALLBACK 0
#endif

// Raises dark textbox pixels to a grey floor. Off: the textbox art is meant
// to be near-black and translucent.
#ifndef PIVAS_DYNAMIC_WINDOW_LUMA_LIFT
#define PIVAS_DYNAMIC_WINDOW_LUMA_LIFT 0
#endif
#endif

#if defined(PIVAS) && PIVAS_RENDER_TRACE
static int pivas_text_state_trace_count       = 0;
static int pivas_dynamic_window_trace_count   = 0;
static int pivas_dialogue_display_trace_count = 0;
static int pivas_text_command_trace_count     = 0;
#endif

#if defined(PIVAS)
static GPU_Rect pivasMapDynamicWindowSourceRect(const GPU_Rect &rect, float scale_x, float scale_y, float texture_w, float texture_h) {
	GPU_Rect mapped{
	    rect.x * scale_x,
	    rect.y * scale_y,
	    rect.w * scale_x,
	    rect.h * scale_y,
	};

	if (mapped.x < 0) {
		mapped.w += mapped.x;
		mapped.x = 0;
	}
	if (mapped.y < 0) {
		mapped.h += mapped.y;
		mapped.y = 0;
	}
	if (mapped.x + mapped.w > texture_w)
		mapped.w = texture_w - mapped.x;
	if (mapped.y + mapped.h > texture_h)
		mapped.h = texture_h - mapped.y;
	if (mapped.w < 0)
		mapped.w = 0;
	if (mapped.h < 0)
		mapped.h = 0;
	return mapped;
}

static bool pivasRectNearlyEqual(const GPU_Rect &a, const GPU_Rect &b) {
	return std::fabs(a.x - b.x) < 0.01f &&
	       std::fabs(a.y - b.y) < 0.01f &&
	       std::fabs(a.w - b.w) < 0.01f &&
	       std::fabs(a.h - b.h) < 0.01f;
}

static bool pivasBlitsNearlyEqual(const std::vector<BlitData> &a, const std::vector<BlitData> &b) {
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++) {
		if (!pivasRectNearlyEqual(a[i].src, b[i].src) ||
		    !pivasRectNearlyEqual(a[i].dst, b[i].dst))
			return false;
	}
	return true;
}

static GPU_Rect pivasBoundsForBlits(const std::vector<BlitData> &blits) {
	GPU_Rect bounds{0, 0, 0, 0};
	bool initialized = false;

	for (const auto &blit : blits) {
		if (blit.dst.w <= 0 || blit.dst.h <= 0)
			continue;

		if (!initialized) {
			bounds      = blit.dst;
			initialized = true;
			continue;
		}

		float x0 = std::min(bounds.x, blit.dst.x);
		float y0 = std::min(bounds.y, blit.dst.y);
		float x1 = std::max(bounds.x + bounds.w, blit.dst.x + blit.dst.w);
		float y1 = std::max(bounds.y + bounds.h, blit.dst.y + blit.dst.h);
		bounds.x = x0;
		bounds.y = y0;
		bounds.w = x1 - x0;
		bounds.h = y1 - y0;
	}

	return bounds;
}

static void pivasSourceExtentForBlits(const std::vector<BlitData> &blits, SDL_Surface *source, float &extent_w, float &extent_h) {
	extent_w = source ? static_cast<float>(source->w) : 0.0f;
	extent_h = source ? static_cast<float>(source->h) : 0.0f;

	for (const auto &blit : blits) {
		if (blit.src.x + blit.src.w > extent_w)
			extent_w = blit.src.x + blit.src.w;
		if (blit.src.y + blit.src.h > extent_h)
			extent_h = blit.src.y + blit.src.h;
	}
}

struct PivasDynamicTextWindowCache {
	GPU_Image *image{nullptr};
	SDL_Surface *source{nullptr};
	GPU_Rect bounds{0, 0, 0, 0};
	std::vector<BlitData> blits;
	std::vector<SDL_Color> fallback_colours;
	float source_extent_w{0};
	float source_extent_h{0};
	bool has_output_stats{false};
	struct PivasSurfaceStats {
		unsigned long long pixels{0};
		unsigned long long alpha_pixels{0};
		unsigned long long opaque_pixels{0};
		unsigned long long rgb_gt_alpha_pixels{0};
		unsigned long long r_sum{0};
		unsigned long long g_sum{0};
		unsigned long long b_sum{0};
		unsigned long long a_sum{0};
		uint8_t min_alpha{255};
		uint8_t max_alpha{0};
		uint8_t max_r{0};
		uint8_t max_g{0};
		uint8_t max_b{0};
	} output_stats;
};

static void pivasFreeDynamicTextWindowCacheImage(PivasDynamicTextWindowCache &cache) {
	if (!cache.image)
		return;

	if (cache.image->texture_w > 0 && cache.image->texture_h > 0 &&
	    (cache.image->w != cache.image->texture_w || cache.image->h != cache.image->texture_h)) {
		GPU_SetImageVirtualResolution(cache.image,
		                              static_cast<uint16_t>(cache.image->texture_w),
		                              static_cast<uint16_t>(cache.image->texture_h));
	}
	gpu.freeImage(cache.image);
	cache.image = nullptr;
}

#if PIVAS_DYNAMIC_WINDOW_TRACE
static int pivas_dynamic_window_diag_count = 0;
#endif
#if PIVAS_DYNAMIC_WINDOW_PROBE
static int pivas_dynamic_window_probe_count = 0;
#endif
#if PIVAS_DYNAMIC_WINDOW_GL_TRACE
static int pivas_dynamic_window_gl_count = 0;
#endif

#if PIVAS_DYNAMIC_WINDOW_TRACE
static const char *pivasBlendModeName(BlendModeId mode) {
	switch (mode) {
		case BlendModeId::NORMAL: return "NORMAL";
		case BlendModeId::ADD: return "ADD";
		case BlendModeId::SUB: return "SUB";
		case BlendModeId::MUL: return "MUL";
		case BlendModeId::ALPHA: return "ALPHA";
		default: return "UNKNOWN";
	}
}

static const char *pivasTransModeName(int mode) {
	switch (mode) {
		case AnimationInfo::TRANS_ALPHA: return "TRANS_ALPHA";
		case AnimationInfo::TRANS_TOPLEFT: return "TRANS_TOPLEFT";
		case AnimationInfo::TRANS_COPY: return "TRANS_COPY";
		case AnimationInfo::TRANS_STRING: return "TRANS_STRING";
		case AnimationInfo::TRANS_DIRECT: return "TRANS_DIRECT";
		case AnimationInfo::TRANS_PALETTE: return "TRANS_PALETTE";
		case AnimationInfo::TRANS_TOPRIGHT: return "TRANS_TOPRIGHT";
		case AnimationInfo::TRANS_MASK: return "TRANS_MASK";
		case AnimationInfo::TRANS_LAYER: return "TRANS_LAYER";
		default: return "TRANS_UNKNOWN";
	}
}
#endif

#if PIVAS_DYNAMIC_WINDOW_GL_TRACE
static const char *pivasGlErrorName(GLenum err) {
	switch (err) {
		case GL_NO_ERROR: return "GL_NO_ERROR";
		case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
		case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
		case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
		case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
		default: return "GL_UNKNOWN";
	}
}
#endif

static void pivasDrainDynamicWindowGlErrors(const char *stage) {
#if PIVAS_DYNAMIC_WINDOW_GL_TRACE
	GPU_FlushBlitBuffer();
	for (int i = 0; i < 8; i++) {
		GLenum err = glGetError();
		if (err == GL_NO_ERROR)
			break;
		if (pivas_dynamic_window_gl_count++ < 64) {
			sendToLog(LogLevel::Warn,
			          "[dyn-window-gl] stale before %s err=0x%04x %s\n",
			          stage ? stage : "(null)", err, pivasGlErrorName(err));
		}
	}
#else
	(void)stage;
#endif
}

static void pivasCheckDynamicWindowGlError(const char *stage) {
#if PIVAS_DYNAMIC_WINDOW_GL_TRACE
	GPU_FlushBlitBuffer();
	GLenum err = glGetError();
	if (err != GL_NO_ERROR && pivas_dynamic_window_gl_count++ < 64) {
		sendToLog(LogLevel::Error,
		          "[dyn-window-gl] after %s err=0x%04x %s\n",
		          stage ? stage : "(null)", err, pivasGlErrorName(err));
	}
#else
	(void)stage;
#endif
}

static PivasDynamicTextWindowCache::PivasSurfaceStats pivasAnalyzeSurfaceRegion(SDL_Surface *surface, GPU_Rect rect) {
	PivasDynamicTextWindowCache::PivasSurfaceStats stats{};
	if (!surface || !surface->format || surface->w <= 0 || surface->h <= 0)
		return stats;

	int x0 = std::max(0, static_cast<int>(std::floor(rect.x)));
	int y0 = std::max(0, static_cast<int>(std::floor(rect.y)));
	int x1 = std::min(surface->w, static_cast<int>(std::ceil(rect.x + rect.w)));
	int y1 = std::min(surface->h, static_cast<int>(std::ceil(rect.y + rect.h)));
	if (x1 <= x0 || y1 <= y0)
		return stats;

	bool locked = false;
	if (SDL_MUSTLOCK(surface)) {
		if (SDL_LockSurface(surface) != 0)
			return stats;
		locked = true;
	}

	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			uint8_t r = 0, g = 0, b = 0, a = 0;
			SDL_GetRGBA(getSurfacePixel(surface, x, y), surface->format, &r, &g, &b, &a);
			stats.pixels++;
			if (a != 0) {
				stats.alpha_pixels++;
				stats.r_sum += r;
				stats.g_sum += g;
				stats.b_sum += b;
				stats.a_sum += a;
				if (a == 255)
					stats.opaque_pixels++;
				if (r > a || g > a || b > a)
					stats.rgb_gt_alpha_pixels++;
			}
			stats.min_alpha = std::min(stats.min_alpha, a);
			stats.max_alpha = std::max(stats.max_alpha, a);
			stats.max_r     = std::max(stats.max_r, r);
			stats.max_g     = std::max(stats.max_g, g);
			stats.max_b     = std::max(stats.max_b, b);
		}
	}

	if (locked)
		SDL_UnlockSurface(surface);
	return stats;
}

#if PIVAS_DYNAMIC_WINDOW_TRACE
static void pivasLogSurfaceStats(const char *label, const PivasDynamicTextWindowCache::PivasSurfaceStats &stats) {
	double denom = stats.alpha_pixels ? static_cast<double>(stats.alpha_pixels) : 1.0;
	sendToLog(LogLevel::Info,
	          "[dyn-window] stats %s pixels=%llu alpha=%llu opaque=%llu rgb_gt_alpha=%llu a_min=%u a_max=%u avg_rgba=%.2f,%.2f,%.2f,%.2f max_rgb=%u,%u,%u\n",
	          label ? label : "(null)",
	          stats.pixels,
	          stats.alpha_pixels,
	          stats.opaque_pixels,
	          stats.rgb_gt_alpha_pixels,
	          static_cast<unsigned>(stats.min_alpha),
	          static_cast<unsigned>(stats.max_alpha),
	          stats.r_sum / denom,
	          stats.g_sum / denom,
	          stats.b_sum / denom,
	          stats.a_sum / denom,
	          static_cast<unsigned>(stats.max_r),
	          static_cast<unsigned>(stats.max_g),
	          static_cast<unsigned>(stats.max_b));
}
#endif

static bool pivasLiftDynamicWindowPixel(uint8_t &r, uint8_t &g, uint8_t &b, uint8_t a) {
#if !PIVAS_DYNAMIC_WINDOW_LUMA_LIFT
	(void)r;
	(void)g;
	(void)b;
	(void)a;
	return false;
#else
	if (a == 0)
		return false;

	uint8_t luma_floor = static_cast<uint8_t>((static_cast<uint16_t>(a) * 96 + 127) / 255);
	uint8_t max_rgb    = std::max(r, std::max(g, b));
	if (max_rgb >= luma_floor)
		return false;

	r = std::max(r, luma_floor);
	g = std::max(g, luma_floor);
	b = std::max(b, luma_floor);
	return true;
#endif
}

static void pivasResetDynamicWindowImageState(GPU_Image *image) {
	if (!image)
		return;

	GPU_SetRGBA(image, 255, 255, 255, 255);
	GPU_SetBlending(image, true);
	GPU_SetImageFilter(image, GPU_FILTER_LINEAR);
	if (image->snap_mode != GPU_SNAP_NONE)
		GPU_SetSnapMode(image, GPU_SNAP_NONE);
}

static SDL_Color pivasDynamicWindowFallbackColour(const PivasDynamicTextWindowCache::PivasSurfaceStats &stats) {
	if (stats.alpha_pixels == 0)
		return SDL_Color{0, 0, 0, 0};

	uint8_t avg_a = static_cast<uint8_t>(stats.a_sum / stats.alpha_pixels);
	uint8_t avg_r = static_cast<uint8_t>(stats.r_sum / stats.alpha_pixels);
	uint8_t avg_g = static_cast<uint8_t>(stats.g_sum / stats.alpha_pixels);
	uint8_t avg_b = static_cast<uint8_t>(stats.b_sum / stats.alpha_pixels);
	pivasLiftDynamicWindowPixel(avg_r, avg_g, avg_b, avg_a);

	uint8_t shade_floor = 56;
	avg_r               = std::max(avg_r, shade_floor);
	avg_g               = std::max(avg_g, shade_floor);
	avg_b               = std::max(avg_b, shade_floor);
	return SDL_Color{avg_r, avg_g, avg_b, 255};
}
#endif

// Should probably use regex
bool ONScripter::isAlphanumeric(char16_t codepoint) {
	if (codepoint >= u'a' && codepoint <= u'z')
		return true;
	if (codepoint >= u'A' && codepoint <= u'Z')
		return true;
	if (codepoint >= u'0' && codepoint <= u'9')
		return true;
	if (codepoint >= u'А' && codepoint <= u'я')
		return true;
	if (codepoint == u'Ё' || codepoint == u'ё')
		return true;
	return false;
}

void ONScripter::processSpecialCharacters(std::u16string &text, Fontinfo &info, Fontinfo::InlineOverrides &io) {
	bool modifiedString;
	do {
		modifiedString = false;
		modifiedString |= processTransformedCharacterSequence(text, info);
		modifiedString |= processSmartQuote(text, info);
		modifiedString |= processInlineCommand(text, info, io);
		modifiedString |= processHashColor(text, info);
		modifiedString |= processIgnored(text, info);
	} while (modifiedString);
}

bool ONScripter::processTransformedCharacterSequence(std::u16string &string, Fontinfo & /*info*/) {
	bool modified = false;
	while (true) {
		if (string.empty())
			return modified;
		if (string[0] == u'`') {
			string.erase(0, 1);
			modified = true;
			continue;
		}
		if (ons.isAlphanumeric(string[0]) || string[0] == u'`' || string[0] == u'‐' || string[0] == u'*') {
			// Look for "..." after the start of the string (plus any number of additional "..."s)
			auto foundNonDot = string.find_first_not_of(u'.', 1);
			if (foundNonDot != std::u16string::npos && foundNonDot >= 4 && (foundNonDot % 3 == 1)) {
				auto afterDots                    = string[foundNonDot];
				const std::u16string permissibles = u"*{‘“";
				bool splittable                   = ons.isAlphanumeric(afterDots) || permissibles.find(afterDots) != std::u16string::npos;
				if (splittable) {
					for (auto it = string.begin() + (foundNonDot); it != string.begin() + 1; it -= 3) {
						it = string.insert(it, ZeroWidthSpace);
					}
					modified = true;
					continue;
				}
			}
		}
		if (string[0] == u'{') {
			if (string.compare(1, 2, u"n}") == 0) {
				string.replace(0, 3, 1, NewLine);
				modified = true;
				continue;
			}
			if (string.compare(1, 2, u"0}") == 0) {
				string.replace(0, 3, 1, ZeroWidthSpace);
				modified = true;
				continue;
			}
			if (string.compare(1, 3, u"qt}") == 0) {
				string.replace(0, 4, 1, NormalQuote);
				modified = true;
				continue;
			}
			if (string.compare(1, 3, u"ob}") == 0) {
				string.replace(0, 4, 1, OpeningCurlyBrace);
				modified = true;
				continue;
			}
			if (string.compare(1, 3, u"eb}") == 0) {
				string.replace(0, 4, 1, ClosingCurlyBrace);
				modified = true;
				continue;
			}
			if (string.compare(1, 3, u"os}") == 0) {
				string.replace(0, 4, 1, OpeningSquareBrace);
				modified = true;
				continue;
			}
			if (string.compare(1, 3, u"es}") == 0) {
				string.replace(0, 4, 1, ClosingSquareBrace);
				modified = true;
				continue;
			}
			if (string.compare(1, 2, u"-}") == 0) {
				string.replace(0, 3, 1, SoftHyphen);
				modified = true;
				continue;
			}
		}
		// Replace horizontal bar with em-dash
		if (string.compare(0, 1, u"―") == 0) {
			string.replace(0, 1, 1, EmDash);
			modified = true;
			continue;
		}
		// Replace em-dash quote with nobr guard
		if (string.compare(0, 2, u"—\"") == 0 && string.compare(0, 3, u"—\"}") != 0) {
			// FIXME: Isn't this dangerous due to incorrectly nested style stack pops?
			string.replace(0, 2, u"{nobr:—\"}");
			modified = true;
			continue;
		}
		if (string.length() >= 3 && string[1] == u'*') {
			auto prev = string.at(0);
			auto next = string.at(2);
			if (ons.isAlphanumeric(prev) && ons.isAlphanumeric(next)) {
				string.replace(1, 1, 1, LinebreakableAsterisk);
				modified = true;
			}
		}
		break;
	}
	return modified;
}

bool ONScripter::processSmartQuote(std::u16string &string, Fontinfo &info) {
	if (string.empty())
		return false;
	uint32_t codepoint  = static_cast<uint32_t>(string.at(0));
	auto &lastCodepoint = info.layoutData.last_printed_codepoint;

	// Smart quote support
	// b indicates we're in ru single-quote parsing mode
	bool b = !info.smart_single_quotes_represented_by_dumb_double && codepoint == '\'';
	if (info.smart_quotes && (b || codepoint == '"')) {
		uint32_t &oq  = b ? info.opening_single_quote : info.opening_double_quote;
		uint32_t &cq  = b ? info.closing_single_quote : info.closing_double_quote;
		const int &qn = b ? info.style().opened_single_quotes : info.style().opened_double_quotes;
		if (lastCodepoint == oq || (b && lastCodepoint == info.opening_double_quote)) {
			// “" becomes ““ (handles cases like ""this"".)
			// for RU: also, “' becomes “‘ (not “’)
			info.styleStack.push(info.style());
			int &qnNew = b ? info.changeStyle().opened_single_quotes : info.changeStyle().opened_double_quotes;
			qnNew++;
			codepoint = oq;
			string.replace(0, 1, 1, codepoint);
			return true;
		}
		if (lastCodepoint == ' ' || lastCodepoint == 0) {
			// a space "then quotes
			if (qn == 0 || !info.smart_single_quotes_represented_by_dumb_double) {
				// becomes “ when there aren't any double quotes open yet
				// for RU: becomes the appropriate opening quote here and now regardless of number context
				info.styleStack.push(info.style());
				int &qnNew = b ? info.changeStyle().opened_single_quotes : info.changeStyle().opened_double_quotes;
				qnNew++;
				codepoint = oq;
				string.replace(0, 1, 1, codepoint);
				return true;
			}
			// if there are double quotes already open, it becomes ‘
			info.styleStack.push(info.style());
			info.changeStyle().opened_single_quotes++;
			codepoint = info.opening_single_quote;
			string.replace(0, 1, 1, codepoint);
			return true;
		}
		if (info.smart_single_quotes_represented_by_dumb_double && info.style().opened_single_quotes > 0) {
			// if we are already two or more levels deep -- “in a case like ‘this"
			// then make a single closing quote
			// RU does not enter this block
			if (info.styleStack.size() > 1) {
				info.styleStack.pop();
				info.fontInfoChanged = true;
			}
			codepoint = info.closing_single_quote;
			string.replace(0, 1, 1, codepoint);
			return true;
		}
		// If there is no single quote open, and we do this" or this.", then close a double quote.
		// for RU: we get whichever closing quote we asked for.
		// Warning! If RU attempts an apostrophe, it will be treated as an unmatched closing quote.
		if (info.styleStack.size() > 1) {
			info.styleStack.pop();
			info.fontInfoChanged = true;
		}
		codepoint = cq;
		string.replace(0, 1, 1, codepoint);
		return true;
	}
	if (info.smart_quotes && codepoint == '\'') {
		codepoint = info.apostrophe;
		string.replace(0, 1, 1, codepoint);
		return true;
	}
	return false;
}

bool ONScripter::processInlineCommand(std::u16string &string, Fontinfo &info, Fontinfo::InlineOverrides &io) {
	if (string.empty())
		return false;
	bool modified      = false;
	uint32_t codepoint = static_cast<uint32_t>(string.at(0));
	if (codepoint == '}') {
		if (info.styleStack.size() > 1) {
			// we need to give character layouting an opportunity to layout this ruby now it's complete
			auto inruby = !info.style().ruby_text.empty();
			info.styleStack.pop();
			info.fontInfoChanged = true;
			string.erase(0, 1);
			if (inruby && info.style().ruby_text.empty())
				string.insert(0, 1, NoOp);
			io |= info.style().inlineOverrides;
			modified = true;
		}
		return modified;
	}

	if (codepoint != '{')
		return modified;

	info.styleStack.push(info.style());
	string.erase(0, 1);

	// read the name
	auto specialCharPos = string.find_first_not_of(u"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
	if (specialCharPos == std::u16string::npos) {
		sendToLog(LogLevel::Warn, "Inline command tag not closed\n");
		return true;
	}

	auto specialChar           = string.at(specialCharPos);
	std::u16string commandName = string.substr(0, specialCharPos);
	string.erase(0, specialCharPos);

	// read the parameter if any
	std::u16string param;
	if (specialChar != '}') {
		string.erase(0, 1);
		auto endParamCharPos          = string.find_first_of(specialChar);
		auto closeBracePos            = string.find_first_of(u"{}");
		bool endParamBeforeCloseBrace = endParamCharPos != std::u16string::npos &&
		                                (closeBracePos == std::u16string::npos ||
		                                 endParamCharPos < closeBracePos);
		if (endParamBeforeCloseBrace) {
			param = string.substr(0, endParamCharPos);
			string.erase(0, endParamCharPos + 1);
		}
	}

	// This command is meant to be ignored
	if (info.style().ignore_text) {
		return true;
	}

	// pass 'em in
	std::string cname{decodeUTF16String(commandName)};
	std::string cparam{decodeUTF16String(param)};
	bool wantsLinebreak = executeInlineTextCommand(cname, cparam, info);
	io |= info.style().inlineOverrides;

	// Encapsulate this in a fn if it gets any larger
	if (wantsLinebreak) {
		// Allow linebreaking before ruby (also passes control back to caller of processSpecialCharacters for correct handling of ruby prefontinfo)
		string.insert(0, u"{0}");
	}
	return true;
}

bool ONScripter::processHashColor(std::u16string &string, Fontinfo &info) {
	if (string.length() < 7)
		return false;
	if (string[0] != u'#')
		return false;
	if (string.find_first_not_of(u"0123456789abcdefABCDEF", 1, 6) != std::string::npos)
		return false;
	std::string value   = decodeUTF16String(string.substr(1, 6));
	std::string command = "color";
	string.erase(0, 7);
	executeInlineTextCommand(command, value, info);
	return true;
}

bool ONScripter::executeInlineTextCommand(std::string &command, std::string &param, Fontinfo &info) {
	// Currently case-sensitive.

	auto processItalic = [](std::string & /* command */, std::string & /* param */, Fontinfo &info) {
		info.changeStyle().is_bold   = false;
		info.changeStyle().is_italic = true;
		return false;
	};

	auto processBold = [](std::string & /* command */, std::string & /* param */, Fontinfo &info) {
		info.changeStyle().is_bold   = true;
		info.changeStyle().is_italic = false;
		return false;
	};

	auto processBoldItalic = [](std::string & /* command */, std::string & /* param */, Fontinfo &info) {
		info.changeStyle().is_bold   = true;
		info.changeStyle().is_italic = true;
		return false;
	};

	auto processUnderline = [](std::string & /* command */, std::string & /* param */, Fontinfo &info) {
		info.changeStyle().is_underline = true;
		return false;
	};

	auto processGradient = [](std::string & /* command */, std::string &param, Fontinfo &info) {
		bool enable                    = param == "1" || param == "on" || param == "yes" || param == "y";
		info.changeStyle().is_gradient = enable;
		return false;
	};

	auto processLeft = [](std::string & /* command */, std::string & /* param */, Fontinfo & /* info */) {
		//FIXME: implement
		return false;
	};

	auto processCentre = [](std::string & /* command */, std::string & /* param */, Fontinfo &info) {
		info.changeStyle().inlineOverrides.is_centered.set(true); //TODO: replace by some kind of alignment enum.
		return false;
	};

	auto processRight = [](std::string & /* command */, std::string & /* param */, Fontinfo & /* info */) {
		//FIXME: implement
		return false;
	};

	auto processAlignment = [](std::string & /* command */, std::string &param, Fontinfo &info) {
		if (param[0] == 'c')
			info.changeStyle().inlineOverrides.is_centered.set(true);
		//TODO: support others?
		return false;
	};

	auto processFit = [](std::string & /* command */, std::string & /* param */, Fontinfo &info) {
		info.changeStyle().inlineOverrides.is_fitted.set(true);
		return false;
	};

	auto processNobreak = [](std::string & /* command */, std::string & /* param */, Fontinfo &info) {
		info.changeStyle().no_break = true;
		return false;
	};

	auto processFont = [](std::string & /* command */, std::string &param, Fontinfo &info) {
		info.changeCurrentFont(std::stoi(param), info.changeStyle().preset_id);
		return false;
	};

	auto processBorder = [](std::string & /* command */, std::string &param, Fontinfo &info) {
		int paramInt                    = std::stoi(param);
		info.changeStyle().is_border    = paramInt;
		info.changeStyle().border_width = paramInt * 25;
		return false;
	};

	auto processShadow = [](std::string & /* command */, std::string &param, Fontinfo &info) {
		auto commaLoc = param.find_first_of(',');
		if (commaLoc == std::string::npos)
			return false;
		int shadowX                           = std::stoi(param.substr(0, commaLoc));
		int shadowY                           = std::stoi(param.substr(commaLoc + 1, param.length() - (commaLoc + 1)));
		info.changeStyle().is_shadow          = shadowX || shadowY;
		info.changeStyle().shadow_distance[0] = shadowX;
		info.changeStyle().shadow_distance[1] = shadowY;
		return false;
	};

	auto processYesNo = [this](std::string &command, std::string &param, Fontinfo &info) {
		size_t idx = std::stoi(param);
		// if conditions is set to 1 and command is n, we want to ignore, and vice versa
		if (conditions[idx] != (command == "y")) {
			info.changeStyle().ignore_text = true;
		}
		return false;
	};

	auto processPreset = [this](std::string & /* command */, std::string &param, Fontinfo &info) {
		int paramInt = std::stoi(param);
		auto pr      = presets.find(paramInt);
		if (pr != presets.end())
			info.changeStyle() = pr->second;
		return false;
	};

	auto processFontSize = [](std::string & /* command */, std::string &param, Fontinfo &info) {
		info.changeStyle().font_size = std::stoi(param);
		return false;
	};

	auto processFontSizePercent = [](std::string & /* command */, std::string &param, Fontinfo &info) {
		info.changeStyle().font_size = (info.style().font_size * std::stoi(param)) / 100;
		return false;
	};

	auto processCharacterSpacing = [](std::string & /* command */, std::string &param, Fontinfo &info) {
		info.changeStyle().character_spacing = std::stoi(param);
		return false;
	};

	auto processRuby = [](std::string & /* command */, std::string &param, Fontinfo &info) {
		info.changeStyle().ruby_text = param;
		return true;
	};

	auto processLoghint = [](std::string & /* command */, std::string &param, Fontinfo &info) {
		if (info.changeStyle().can_loghint)
			info.changeStyle().ruby_text = param;
		return true;
	};

	auto processWidth = [](std::string & /* command */, std::string &param, Fontinfo &info) {
		info.changeStyle().inlineOverrides.wrap_limit.set(std::stoi(param));
		return false;
	};

	auto processParallel = [](std::string & /* command */, std::string & /* param */, Fontinfo &info) {
		info.changeStyle().inlineOverrides.startsNewRun.set(true); // no real reason this needs to be an override... could have put it directly in fontInfo...
		return false;
	};

	auto processColour = [this](std::string & /* command */, std::string &param, Fontinfo &info) {
		if (param.length() != 6)
			return false;
		std::string colorString = "#" + param;
		readColor(&info.changeStyle().color, colorString.data());
		return false;
	};

	auto processShadowColour = [this](std::string & /* command */, std::string &param, Fontinfo &info) {
		if (param.length() != 6)
			return false;
		std::string colorString = "#" + param;
		readColor(&info.changeStyle().shadow_color, colorString.data());
		return false;
	};

	auto processBorderColour = [this](std::string & /* command */, std::string &param, Fontinfo &info) {
		if (param.length() != 6)
			return false;
		std::string colorString = "#" + param;
		readColor(&info.changeStyle().border_color, colorString.data());
		return false;
	};

	if (processFuncs.empty()) {
		processFuncs["italic"] = processFuncs["i"] = processItalic;
		processFuncs["bold"] = processFuncs["b"] = processBold;
		processFuncs["bolditalic"] = processFuncs["x"] = processBoldItalic;
		processFuncs["underline"] = processFuncs["u"] = processUnderline;

		processFuncs["gradient"] = processFuncs["g"] = processGradient;

		processFuncs["left"] = processFuncs["al"] = processLeft;
		processFuncs["center"] = processFuncs["centre"] = processFuncs["ac"] = processCentre;
		processFuncs["right"] = processFuncs["ar"] = processRight;
		processFuncs["alignment"] = processFuncs["a"] = processAlignment;

		processFuncs["fit"] = processFuncs["j"] = processFit;
		processFuncs["nobreak"] = processFuncs["nobr"] = processNobreak;

		processFuncs["font"] = processFuncs["f"] = processFont;

		processFuncs["border"] = processFuncs["borderwidth"] = processFuncs["o"] = processBorder;
		processFuncs["shadow"] = processFuncs["shadowdistance"] = processFuncs["s"] = processShadow;

		processFuncs["y"] = processFuncs["n"] = processYesNo;
		processFuncs["preset"] = processFuncs["p"] = processPreset;

		processFuncs["fontsize"] = processFuncs["fontsizeabsolute"] = processFuncs["size"] = processFuncs["d"] = processFontSize;
		processFuncs["fontsizepercent"] = processFuncs["fontsizepc"] = processFuncs["sizepercent"] = processFuncs["sizepc"] = processFuncs["e"] = processFontSizePercent;
		processFuncs["characterspacing"] = processFuncs["charspacing"] = processFuncs["m"] = processCharacterSpacing;

		processFuncs["ruby"] = processFuncs["h"] = processRuby;
		processFuncs["loghint"] = processFuncs["l"] = processLoghint;

		processFuncs["width"] = processFuncs["w"] = processWidth;
		processFuncs["parallel"] = processFuncs["t"] = processParallel;

		processFuncs["color"] = processFuncs["colour"] = processFuncs["c"] = processColour;
		processFuncs["shadowcolor"] = processFuncs["shadowcolour"] = processFuncs["v"] = processShadowColour;
		processFuncs["bordercolor"] = processFuncs["bordercolour"] = processFuncs["r"] = processBorderColour;
	}

	auto cmd = processFuncs.find(command);
	if (cmd != processFuncs.end())
		return cmd->second(command, param, info);
	return false;
}

bool ONScripter::processIgnored(std::u16string &string, Fontinfo &info) {
	if (info.style().ignore_text) {
		auto cmdLoc = string.find_first_of(u"{}");
		if (cmdLoc != std::string::npos) {
			string.erase(0, cmdLoc);
			return true;
		}
	}
	return false;
}

void ONScripter::resetGlyphCache() {
	if (!use_text_atlas) {
		throw std::runtime_error("Attempted to reset disabled text atlas");
	}

	sendToLog(LogLevel::Warn, "Resetting glyph cache will cause degraded performance!\n");
	glyphAtlas.reset();
	auto sz = glyphCache.size();
	glyphCache.resize(0);
	glyphCache.resize(sz);
}

void ONScripter::renderGlyphValues(const GlyphValues &values, GPU_Rect *dst_clip, TextRenderingState::TextRenderingDst dst, float x, float y, float r, bool render_border, int alpha) {
	GPU_Image *coloured_glyph{nullptr};
	GPU_Rect *src_rect{nullptr};
	if ((!render_border && values.glyph_pos.has()) || (render_border && values.border_pos.has())) {
		// new approach goes here
		coloured_glyph = glyphAtlas.atlas;
		src_rect       = render_border ? &values.border_pos.get() : &values.glyph_pos.get();
	} else {
		coloured_glyph = render_border ? values.border_gpu : values.glyph_gpu;
	}

	if (coloured_glyph) {
		x += r * (src_rect ? src_rect->w : coloured_glyph->w) / 2.0;
		y += 1 * (src_rect ? src_rect->h : coloured_glyph->h) / 2.0;
		if (alpha < 255) {
			GPU_SetRGBA(coloured_glyph, alpha, alpha, alpha, alpha);
		}
		if (dst.target) {
			gpu.copyGPUImage(coloured_glyph, src_rect, dst_clip, dst.target, x, y, r, 1, 0, true);
		} else {
			if (r == 1)
				gpu.copyGPUImage(coloured_glyph, src_rect, dst_clip, dst.bigImage, x, y);
			else
				errorAndExit("BigImages do not support scaled text at this moment!");
		}
		if (alpha < 255) {
			GPU_SetRGBA(coloured_glyph, 255, 255, 255, 255);
		}
	}
}

/* const: you may not alter the properties of the returned GlyphValues, because that would change our nice cached version */
const GlyphValues *ONScripter::renderUnicodeGlyph(Font *font, GlyphParams *key) {
	static SDL_Color fcol = {0xff, 0xff, 0xff, 0xff}, bcol = {0, 0, 0, 0};

	GlyphParams k = *key;

	GlyphValues *glyph;
	try {
		glyph = glyphCache.get(k);
	} catch (int) {
		// No coloured glyph found... we'll have to get an uncolored one and color it.
		// First let's see if there's an uncolored one already in the cache.
		GlyphParams uncolored = k;
		uncolored.is_colored  = false;
		GlyphValues *uncolored_glyph;
		try {
			uncolored_glyph = glyphCache.get(uncolored);
		} catch (int) {
			// No uncoloured one in the cache either. Looks like we gotta render it from FT. (Then put it in the cache for later use.)
			uncolored_glyph = font->renderGlyph(&uncolored, fcol, bcol);
			if (uncolored_glyph->buildGPUImages(use_text_atlas ? &glyphAtlas : nullptr)) {
				glyphCache.set(uncolored, uncolored_glyph);
			} else {
				delete uncolored_glyph;
				resetGlyphCache();
				return renderUnicodeGlyph(font, key);
			}
		}
		// OK, so we have the uncolored glyph one way or another... now let's paint it
		// (but not if we are asked to paint it black)
		bool black_glyph  = k.glyph_color.r == 0 && k.glyph_color.g == 0 && k.glyph_color.b == 0;
		bool black_border = k.border_color.r == 0 && k.border_color.g == 0 && k.border_color.b == 0;
		if (black_glyph && black_border) {
			return uncolored_glyph;
		}
		bool should_set = true;
		glyph           = new GlyphValues(*uncolored_glyph); // so we don't ruin the uncolored one in the cache (prevents trying to recolor an already colored glyph)
		if (!black_glyph)
			should_set = colorGlyph(key, glyph, &k.glyph_color, false, use_text_atlas ? &glyphAtlas : nullptr); // Color the glyph
		if (!black_border && should_set)
			should_set = colorGlyph(key, glyph, &k.border_color, true, use_text_atlas ? &glyphAtlas : nullptr); // Color the border
		if (should_set) {
			glyphCache.set(k, glyph); // Store the colored glyph in the cache so we don't need to color it repeatedly.
		} else {
			delete glyph;
			resetGlyphCache();
			return renderUnicodeGlyph(font, key);
		}
	}

	return glyph;
}

const GlyphValues *ONScripter::measureUnicodeGlyph(Font *font, GlyphParams *key) {
	GlyphParams k = *key;
	GlyphValues *glyph;
	try {
		glyph = glyphMeasureCache.get(k);
	} catch (int) {
		glyph = font->measureGlyph(&k);
		glyphMeasureCache.set(k, glyph);
	}
	return glyph;
}

#if defined(PIVAS)
extern uint32_t pivas_final_window_hold_until; // ONScripter.cpp
extern uint32_t pivas_perf_autosave_ms;        // ONScripter.cpp
#if PIVAS_DYNAMIC_WINDOW_FINAL_DIRECT_ART
extern bool pivas_final_window_seen_live_dialogue; // ONScripter.cpp
#endif
#endif

void ONScripter::enterTextDisplayMode() {
#if defined(PIVAS) && PIVAS_RENDER_TRACE
	if (pivas_text_state_trace_count++ < 160) {
		sendToLog(LogLevel::Info,
		          "[text-state] enter begin display=0x%x rwtm=0x%x dynamic=%d page_enter=%d dlg_active=%d dlg_rendering=%d before_hud=%g,%g,%g,%g dirty_hud=%g,%g,%g,%g\n",
		          display_mode,
		          refresh_window_text_mode,
		          wndCtrl.usingDynamicTextWindow ? 1 : 0,
		          page_enter_status,
		          dlgCtrl.dialogueProcessingState.active ? 1 : 0,
		          dlgCtrl.dialogueIsRendering ? 1 : 0,
		          before_dirty_rect_hud.bounding_box_script.x,
		          before_dirty_rect_hud.bounding_box_script.y,
		          before_dirty_rect_hud.bounding_box_script.w,
		          before_dirty_rect_hud.bounding_box_script.h,
		          dirty_rect_hud.bounding_box_script.x,
		          dirty_rect_hud.bounding_box_script.y,
		          dirty_rect_hud.bounding_box_script.w,
		          dirty_rect_hud.bounding_box_script.h);
	}
#endif
	if (saveon_flag && internal_saveon_flag) {
#if defined(PIVAS)
		uint32_t pivas_autosave_t0 = SDL_GetTicks();
		saveSaveFile(-1);
		pivas_perf_autosave_ms += SDL_GetTicks() - pivas_autosave_t0;
#else
		saveSaveFile(-1);
#endif
		internal_saveon_flag = false;
	}

	did_leavetext = false;

	if (wndCtrl.usingDynamicTextWindow) {
		// When we are using a normal window textbox area is static, the only possible change is setwindow-based and
		// we can always add a new rect to the dirty_rect (which actually happens in those commands).
		// That's why enterTextDisplay is optimised not to refresh anything if we are already in text mode.
		// Dynamic window is not like that. We no longer know its previous dimensions when it dlgCtrl is deactivated,
		// which means that we have to cleanup a bigger area to avoid issues (i. e. script area).
		// This is done here, because earlier may well collide with pretext actions.
		// This is not done in texec3, because texec3 is a logical command that cleans the text out.
		before_dirty_rect_hud.add({0, 0, static_cast<float>(text_gpu->w), static_cast<float>(text_gpu->h)});
		dirty_rect_hud.add({0, 0, static_cast<float>(text_gpu->w), static_cast<float>(text_gpu->h)});
	}

	if (!(display_mode & DISPLAY_MODE_TEXT)) {

		display_mode = DISPLAY_MODE_TEXT;
#if defined(PIVAS)
		pivas_hide_recompose_pending = false;
		pivas_script_hide       = false; // texton / text shown again
#endif

		if (!wndCtrl.usingDynamicTextWindow)
			addTextWindowClip(before_dirty_rect_hud);

		// Unsure if perfectly safe...
		if (!(skip_mode & SKIP_SUPERSKIP)) {
#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_FINAL_DIRECT_ART
			// A window-style switch (textoff + setwindow in
			// set_name_window_base) cancels the hold, and the reduced
			// window_effect below presents before anything re-arms it:
			// presentVitaReducedEffectTargets checks the gap gate without
			// composing through combineWithCamera, where the hold is normally
			// armed. Without this an empty textbox is presented and then held
			// until the first glyph. Genuine hides still cancel the hold in
			// leaveTextDisplayMode; skip modes bypass the gate entirely.
			if (wndCtrl.usingDynamicTextWindow &&
			    pivas_final_window_seen_live_dialogue &&
			    dlgCtrl.dialogueProcessingState.active &&
			    pivas_final_window_hold_until == 0) {
				pivas_final_window_hold_until = SDL_GetTicks() + 250;
#if PIVAS_GATE_TRACE
				static uint32_t pivas_enter_hold_arm_trace = 0;
				if (pivas_enter_hold_arm_trace < 32) {
					pivas_enter_hold_arm_trace++;
					sendToLog(LogLevel::Info, "[gap-gate] re-armed hold at text-display enter (window-switch advance)\n");
				}
#endif
			}
#endif
			if (constantRefreshEffect(&window_effect, false, false,
			                          REFRESH_BEFORESCENE_MODE | REFRESH_NORMAL_MODE,     // refresh from no window (on beforescene)
			                          REFRESH_BEFORESCENE_MODE | refresh_window_text_mode // to window              (on beforescene)
			                          ))
				return;
		}
	} else if (wndCtrl.usingDynamicTextWindow) {
		// This will make sure we are refreshing what we need to
		flush(refresh_window_text_mode);
	}
}

void ONScripter::leaveTextDisplayMode(bool force_leave_flag, bool perform_effect) {
	//sendToLog(LogLevel::Info, "leaveTextDisplayMode(%u)\n", force_leave_flag);
#if defined(PIVAS) && PIVAS_RENDER_TRACE
	if (pivas_text_state_trace_count++ < 160) {
		sendToLog(LogLevel::Info,
		          "[text-state] leave begin display=0x%x rwtm=0x%x force=%d perform=%d did_leave=%d erase_mode=%d dynamic=%d dlg_active=%d dlg_rendering=%d\n",
		          display_mode,
		          refresh_window_text_mode,
		          force_leave_flag ? 1 : 0,
		          perform_effect ? 1 : 0,
		          did_leavetext ? 1 : 0,
		          erase_text_window_mode,
		          wndCtrl.usingDynamicTextWindow ? 1 : 0,
		          dlgCtrl.dialogueProcessingState.active ? 1 : 0,
		          dlgCtrl.dialogueIsRendering ? 1 : 0);
	}
#endif

	//ons-en feature: when in certain skip modes, don't actually leave
	//text display mode unless forced to (but say you did)
	if (!force_leave_flag && (skip_mode & (SKIP_NORMAL) || keyState.ctrl)) {
		did_leavetext = true;
		return;
	}
	if (force_leave_flag)
		did_leavetext = false;

	if (!did_leavetext && (display_mode & DISPLAY_MODE_TEXT) &&
	    (force_leave_flag || (erase_text_window_mode != 0))) {

		//sendToLog(LogLevel::Info, "leaveTextDisplayMode(%u) body\n", force_leave_flag);

#if defined(PIVAS)
		// Intentional hides (textoff etc.) cancel the textbox hold that
		// bridges between-line gaps. This must stay in the genuine-leave
		// body: with 'erasetextwindow 0' nearly every visual command calls
		// this function as a no-op, and cancelling there would drop the
		// hold mid-line.
		pivas_final_window_hold_until = 0;
		// See btnwaitCommand: the hide only shows once the script waits.
		pivas_hide_recompose_pending = true;
#endif

		addTextWindowClip(dirty_rect_hud);

		display_mode = DISPLAY_MODE_NORMAL;

		// Unsure if perfectly safe...
		if (perform_effect && !(skip_mode & SKIP_SUPERSKIP)) {
			if (constantRefreshEffect(&window_effect, false, false,
			                          REFRESH_BEFORESCENE_MODE | refresh_window_text_mode, // refresh from window (on beforescene)
			                          REFRESH_BEFORESCENE_MODE | REFRESH_NORMAL_MODE       // to no window        (on beforescene)
			                          ))
				return;
		}
	}

	display_mode |= DISPLAY_MODE_UPDATED;
}

void ONScripter::renderDynamicTextWindow(GPU_Target *target, GPU_Rect *canvas_clip_dst, int refresh_mode, bool useCamera) {
	// The normal, non-dynamic way
	// drawToGPUTarget(target, sentence_font_info.oldNew(refresh_mode), refresh_mode, clip_dst);

#if defined(PIVAS) && PIVAS_DIRECT_TEXT_WINDOW
	GPU_Rect window_rect = wndCtrl.getExtendedWindow();
	if (useCamera) {
		window_rect.x += camera.center_pos.x;
		window_rect.y += camera.center_pos.y;
	}
	if (window_rect.w <= 0 || window_rect.h <= 0)
		return;
	if (canvas_clip_dst && doClipping(&window_rect, canvas_clip_dst))
		return;

	SDL_Color color{
	    static_cast<uint8_t>(sentence_font.is_transparent ? sentence_font.window_color.x : 0x30),
	    static_cast<uint8_t>(sentence_font.is_transparent ? sentence_font.window_color.y : 0x30),
	    static_cast<uint8_t>(sentence_font.is_transparent ? sentence_font.window_color.z : 0x30),
	    0xff};
	GPU_SetShapeBlending(false);
	GPU_RectangleFilled2(target, window_rect, color);
	return;
#endif

	AnimationInfo *info = sentence_font_info.oldNew(refresh_mode);
	GPU_Image *src      = info->gpu_image;
	if (!src) {
#if defined(PIVAS) && PIVAS_RENDER_TRACE
		if (pivas_dynamic_window_trace_count++ < 192) {
			sendToLog(LogLevel::Warn,
			          "[dyn-window] skipped: missing sentence font image rm=0x%x display=0x%x active=%d\n",
			          refresh_mode,
			          display_mode,
			          dlgCtrl.dialogueProcessingState.active ? 1 : 0);
		}
#endif
		return;
	}

	auto blits = wndCtrl.getRegions();
#if defined(PIVAS)
	float source_extent_w = src->w;
	float source_extent_h = src->h;
	for (const auto &blit : blits) {
		if (blit.src.x + blit.src.w > source_extent_w)
			source_extent_w = blit.src.x + blit.src.w;
		if (blit.src.y + blit.src.h > source_extent_h)
			source_extent_h = blit.src.y + blit.src.h;
	}

	float texture_w = static_cast<float>(src->texture_w);
	float texture_h = static_cast<float>(src->texture_h);
	bool use_physical_source_rects =
	    window.hasRenderScale() &&
	    texture_w > 0 && texture_h > 0 &&
	    source_extent_w > 0 && source_extent_h > 0 &&
	    (texture_w != source_extent_w || texture_h != source_extent_h);
	float pivas_source_scale_x = use_physical_source_rects ? texture_w / source_extent_w : 1.0f;
	float pivas_source_scale_y = use_physical_source_rects ? texture_h / source_extent_h : 1.0f;
	uint16_t original_image_w  = static_cast<uint16_t>(src->w);
	uint16_t original_image_h  = static_cast<uint16_t>(src->h);

	if (use_physical_source_rects)
		GPU_SetImageVirtualResolution(src, static_cast<uint16_t>(src->texture_w), static_cast<uint16_t>(src->texture_h));

	gpu.pushBlendMode(info->blending_mode);

#if PIVAS_DYNAMIC_WINDOW_TRACE
	auto pivasTargetName = [&]() -> const char * {
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
	bool pivas_trace_this_draw = pivas_dynamic_window_diag_count++ < 160;
	if (pivas_trace_this_draw) {
		SDL_Surface *surface = info->image_surface;
		sendToLog(LogLevel::Info,
		          "[dyn-window] begin target=%s rm=0x%x use_camera=%d active=%d rendering=%d page_cleared=%d display=0x%x blend=%s trans=%s trans_val=%d src_img=%dx%d tex=%dx%d surface=%p surface_wh=%dx%d bpp=%d pitch=%d masks=%08x,%08x,%08x,%08x blits=%d extent=%g,%g phys_scale=%g,%g clip=%g,%g,%g,%g bounds=%g,%g,%g,%g\n",
		          pivasTargetName(),
		          refresh_mode,
		          useCamera ? 1 : 0,
		          dlgCtrl.dialogueProcessingState.active ? 1 : 0,
		          dlgCtrl.dialogueIsRendering ? 1 : 0,
		          dlgCtrl.vitaPageClearedByDialogue ? 1 : 0,
		          display_mode,
		          pivasBlendModeName(info->blending_mode),
		          pivasTransModeName(info->trans_mode),
		          info->trans,
		          src->w,
		          src->h,
		          src->texture_w,
		          src->texture_h,
		          static_cast<void *>(surface),
		          surface ? surface->w : 0,
		          surface ? surface->h : 0,
		          surface && surface->format ? surface->format->BytesPerPixel : 0,
		          surface ? surface->pitch : 0,
		          surface && surface->format ? surface->format->Rmask : 0,
		          surface && surface->format ? surface->format->Gmask : 0,
		          surface && surface->format ? surface->format->Bmask : 0,
		          surface && surface->format ? surface->format->Amask : 0,
		          static_cast<int>(blits.size()),
		          source_extent_w,
		          source_extent_h,
		          pivas_source_scale_x,
		          pivas_source_scale_y,
		          canvas_clip_dst ? canvas_clip_dst->x : 0,
		          canvas_clip_dst ? canvas_clip_dst->y : 0,
		          canvas_clip_dst ? canvas_clip_dst->w : 0,
		          canvas_clip_dst ? canvas_clip_dst->h : 0,
		          pivasBoundsForBlits(blits).x,
		          pivasBoundsForBlits(blits).y,
		          pivasBoundsForBlits(blits).w,
		          pivasBoundsForBlits(blits).h);

		for (size_t i = 0; i < blits.size() && i < 6; i++) {
			GPU_Rect mapped = blits[i].src;
			if (use_physical_source_rects) {
				mapped = pivasMapDynamicWindowSourceRect(mapped, pivas_source_scale_x, pivas_source_scale_y, texture_w, texture_h);
			}
			sendToLog(LogLevel::Info,
			          "[dyn-window] blit%u src=%g,%g,%g,%g mapped=%g,%g,%g,%g dst=%g,%g,%g,%g\n",
			          static_cast<unsigned>(i),
			          blits[i].src.x,
			          blits[i].src.y,
			          blits[i].src.w,
			          blits[i].src.h,
			          mapped.x,
			          mapped.y,
			          mapped.w,
			          mapped.h,
			          blits[i].dst.x,
			          blits[i].dst.y,
			          blits[i].dst.w,
			          blits[i].dst.h);
		}
		if (surface) {
			pivasLogSurfaceStats("source-full", pivasAnalyzeSurfaceRegion(surface, {0, 0, static_cast<float>(surface->w), static_cast<float>(surface->h)}));
			for (size_t i = 0; i < blits.size() && i < 3; i++) {
				GPU_Rect mapped = pivasMapDynamicWindowSourceRect(blits[i].src,
				                                                  surface->w / source_extent_w,
				                                                  surface->h / source_extent_h,
				                                                  static_cast<float>(surface->w),
				                                                  static_cast<float>(surface->h));
				char label[32];
				std::snprintf(label, sizeof(label), "source-blit%u", static_cast<unsigned>(i));
				pivasLogSurfaceStats(label, pivasAnalyzeSurfaceRegion(surface, mapped));
			}
		}
	}
#endif
#endif
#if defined(PIVAS) && PIVAS_CPU_DYNAMIC_TEXT_WINDOW
	if (info->image_surface && !blits.empty()) {
		static PivasDynamicTextWindowCache cache;
		SDL_Surface *surface = info->image_surface;
		GPU_Rect bounds      = pivasBoundsForBlits(blits);
		float cpu_extent_w   = 0.0f;
		float cpu_extent_h   = 0.0f;
		pivasSourceExtentForBlits(blits, surface, cpu_extent_w, cpu_extent_h);

		bool valid_bounds = bounds.w > 0 && bounds.h > 0 &&
		                    cpu_extent_w > 0 && cpu_extent_h > 0 &&
		                    surface->w > 0 && surface->h > 0;
		bool cache_valid = cache.image &&
		                   cache.source == surface &&
		                   pivasRectNearlyEqual(cache.bounds, bounds) &&
		                   pivasBlitsNearlyEqual(cache.blits, blits) &&
		                   std::fabs(cache.source_extent_w - cpu_extent_w) < 0.01f &&
		                   std::fabs(cache.source_extent_h - cpu_extent_h) < 0.01f;

		if (valid_bounds && !cache_valid) {
			// The previous art is freed only once the replacement upload
			// succeeds (below), so an alloc or upload failure keeps drawing
			// the stale art instead of falling through to the atlas path,
			// which does not render on vitaGL.
			cache.fallback_colours.assign(blits.size(), SDL_Color{0, 0, 0, 0});

			int output_w = window.physicalFromLogical(static_cast<int>(std::ceil(bounds.w)));
			int output_h = window.physicalFromLogical(static_cast<int>(std::ceil(bounds.h)));
			SDL_Surface *composed = SDL_CreateRGBSurfaceWithFormat(0, output_w, output_h, 32, SDL_PIXELFORMAT_RGBA32);

			if (composed) {
				SDL_SetSurfaceBlendMode(composed, SDL_BLENDMODE_NONE);
				SDL_FillRect(composed, nullptr, SDL_MapRGBA(composed->format, 0, 0, 0, 0));

				if (SDL_MUSTLOCK(surface))
					SDL_LockSurface(surface);
				if (SDL_MUSTLOCK(composed))
					SDL_LockSurface(composed);

				float dst_scale_x = output_w / bounds.w;
				float dst_scale_y = output_h / bounds.h;
				float src_scale_x = surface->w / cpu_extent_w;
				float src_scale_y = surface->h / cpu_extent_h;
				unsigned long long pivas_luma_lift_pixels = 0;

				for (size_t blit_index = 0; blit_index < blits.size(); blit_index++) {
					const auto &blit = blits[blit_index];
					if (blit.src.w <= 0 || blit.src.h <= 0 || blit.dst.w <= 0 || blit.dst.h <= 0)
						continue;

					GPU_Rect src_rect = pivasMapDynamicWindowSourceRect(blit.src, src_scale_x, src_scale_y,
					                                                    static_cast<float>(surface->w),
					                                                    static_cast<float>(surface->h));
					if (src_rect.w <= 0 || src_rect.h <= 0)
						continue;

					cache.fallback_colours[blit_index] =
					    pivasDynamicWindowFallbackColour(pivasAnalyzeSurfaceRegion(surface, src_rect));

					int dst_x0 = std::max(0, static_cast<int>(std::floor((blit.dst.x - bounds.x) * dst_scale_x)));
					int dst_y0 = std::max(0, static_cast<int>(std::floor((blit.dst.y - bounds.y) * dst_scale_y)));
					// +1px overlap: adjacent slice edges land on different
					// logical coordinates, and after the 2/3 scale floor/ceil
					// can leave a 1px transparent seam between slices.
					int dst_x1 = std::min(output_w, static_cast<int>(std::ceil((blit.dst.x + blit.dst.w - bounds.x) * dst_scale_x)) + 1);
					int dst_y1 = std::min(output_h, static_cast<int>(std::ceil((blit.dst.y + blit.dst.h - bounds.y) * dst_scale_y)) + 1);
					int dst_w  = dst_x1 - dst_x0;
					int dst_h  = dst_y1 - dst_y0;
					if (dst_w <= 0 || dst_h <= 0)
						continue;

					// Sample at least half a texel inside the region so the
					// downscaled atlas's transparent gutters never bleed in.
					float src_y_min = src_rect.y + 0.5f;
					float src_y_max = src_rect.y + src_rect.h - 0.5f;
					float src_x_min = src_rect.x + 0.5f;
					float src_x_max = src_rect.x + src_rect.w - 0.5f;

					for (int y = 0; y < dst_h; y++) {
						float src_yf = src_rect.y + ((y + 0.5f) * src_rect.h / dst_h);
						if (src_y_max >= src_y_min)
							src_yf = std::max(src_y_min, std::min(src_y_max, src_yf));
						int src_y = std::max(0, std::min(surface->h - 1, static_cast<int>(std::floor(src_yf))));
						for (int x = 0; x < dst_w; x++) {
							float src_xf = src_rect.x + ((x + 0.5f) * src_rect.w / dst_w);
							if (src_x_max >= src_x_min)
								src_xf = std::max(src_x_min, std::min(src_x_max, src_xf));
							int src_x = std::max(0, std::min(surface->w - 1, static_cast<int>(std::floor(src_xf))));

							uint8_t r = 0, g = 0, b = 0, a = 0;
							SDL_GetRGBA(getSurfacePixel(surface, src_x, src_y), surface->format, &r, &g, &b, &a);
							if (pivasLiftDynamicWindowPixel(r, g, b, a))
								pivas_luma_lift_pixels++;
							setSurfacePixel(composed, dst_x0 + x, dst_y0 + y, SDL_MapRGBA(composed->format, r, g, b, a));
						}
					}
				}

				if (SDL_MUSTLOCK(composed))
					SDL_UnlockSurface(composed);
				if (SDL_MUSTLOCK(surface))
					SDL_UnlockSurface(surface);

				cache.output_stats     = pivasAnalyzeSurfaceRegion(composed, {0, 0, static_cast<float>(composed->w), static_cast<float>(composed->h)});
				cache.has_output_stats = true;
#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_TRACE
				if (pivas_trace_this_draw) {
					sendToLog(LogLevel::Info,
					          "[dyn-window] cpu-compose output=%dx%d logical_bounds=%g,%g,%g,%g dst_scale=%g,%g src_scale=%g,%g cache_valid_before=%d luma_lift=%llu\n",
					          composed->w,
					          composed->h,
					          bounds.x,
					          bounds.y,
					          bounds.w,
					          bounds.h,
					          dst_scale_x,
					          dst_scale_y,
					          src_scale_x,
					          src_scale_y,
					          cache_valid ? 1 : 0,
					          pivas_luma_lift_pixels);
					pivasLogSurfaceStats("cpu-output", cache.output_stats);
				}
#endif
				pivasDrainDynamicWindowGlErrors("cpu-compose-byte-upload");
				GPU_Image *pivas_new_art = gpu.copyImageFromSurfaceBytes(composed);
				pivasCheckDynamicWindowGlError("cpu-compose-byte-upload");
				SDL_FreeSurface(composed);
				if (pivas_new_art) {
					pivasFreeDynamicTextWindowCacheImage(cache);
					cache.image = pivas_new_art;
					pivasResetDynamicWindowImageState(cache.image);
					cache.source          = surface;
					cache.bounds          = bounds;
					cache.blits           = blits;
					cache.source_extent_w = cpu_extent_w;
					cache.source_extent_h = cpu_extent_h;
				} else {
					static uint32_t pivas_art_upload_fail_trace = 0;
					if (pivas_art_upload_fail_trace < 16) {
						pivas_art_upload_fail_trace++;
						sendToLog(LogLevel::Error, "[dyn-window] art rebuild upload FAILED (%dx%d), keeping stale art\n", output_w, output_h);
					}
				}
			} else {
				static uint32_t pivas_art_surface_fail_trace = 0;
				if (pivas_art_surface_fail_trace < 16) {
					pivas_art_surface_fail_trace++;
					sendToLog(LogLevel::Error, "[dyn-window] compose surface alloc FAILED (%dx%d), keeping stale art\n", output_w, output_h);
				}
			}
		}

		if (valid_bounds && cache.image) {
			GPU_Rect draw_clip = bounds;
			if (useCamera) {
				draw_clip.x += camera.center_pos.x;
				draw_clip.y += camera.center_pos.y;
			}
			bool clipped = canvas_clip_dst && doClipping(&draw_clip, canvas_clip_dst);
#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_TRACE
			if (pivas_trace_this_draw) {
				sendToLog(LogLevel::Info,
				          "[dyn-window] cpu-draw target=%s clipped=%d draw_clip=%g,%g,%g,%g cache_img=%dx%d tex=%dx%d stats=%d alpha_pixels=%llu blend=%s\n",
				          pivasTargetName(),
				          clipped ? 1 : 0,
				          draw_clip.x,
				          draw_clip.y,
				          draw_clip.w,
				          draw_clip.h,
				          cache.image ? cache.image->w : 0,
				          cache.image ? cache.image->h : 0,
			          cache.image ? cache.image->texture_w : 0,
			          cache.image ? cache.image->texture_h : 0,
			          cache.has_output_stats ? 1 : 0,
			          cache.output_stats.alpha_pixels,
			          pivasBlendModeName(info->blending_mode));
			}
#endif
			if (!clipped) {
				float coord_x = (useCamera ? bounds.x + camera.center_pos.x : bounds.x) + bounds.w / 2.0f;
				float coord_y = (useCamera ? bounds.y + camera.center_pos.y : bounds.y) + bounds.h / 2.0f;
				GPU_Rect src_rect{0, 0, static_cast<float>(cache.image->w), static_cast<float>(cache.image->h)};
				float draw_scale_x = cache.image->w > 0 ? bounds.w / cache.image->w : 1.0f;
				float draw_scale_y = cache.image->h > 0 ? bounds.h / cache.image->h : 1.0f;
				pivasResetDynamicWindowImageState(cache.image);
#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_TRACE
				if (pivas_trace_this_draw) {
					sendToLog(LogLevel::Info,
					          "[dyn-window] image-state target=%s use_blending=%d color=%u,%u,%u,%u filter=%d snap=%d src=%g,%g,%g,%g scale=%g,%g\n",
					          pivasTargetName(),
					          cache.image->use_blending ? 1 : 0,
					          static_cast<unsigned>(cache.image->color.r),
					          static_cast<unsigned>(cache.image->color.g),
					          static_cast<unsigned>(cache.image->color.b),
					          static_cast<unsigned>(cache.image->color.a),
					          static_cast<int>(cache.image->filter_mode),
					          static_cast<int>(cache.image->snap_mode),
					          src_rect.x,
					          src_rect.y,
					          src_rect.w,
					          src_rect.h,
					          draw_scale_x,
					          draw_scale_y);
				}
#endif
				pivasDrainDynamicWindowGlErrors("cpu-draw-normal");
				gpu.copyGPUImage(cache.image, &src_rect, &draw_clip, target, coord_x, coord_y, draw_scale_x, draw_scale_y, 0, true);
				pivasCheckDynamicWindowGlError("cpu-draw-normal");

#if PIVAS_DYNAMIC_WINDOW_GEOMETRY_FALLBACK
				int fallback_rects = 0;
				GPU_SetShapeBlending(false);
				for (size_t i = 0; i < cache.blits.size() && i < cache.fallback_colours.size(); i++) {
					SDL_Color colour = cache.fallback_colours[i];
					if (colour.a == 0)
						continue;

					GPU_Rect rect = cache.blits[i].dst;
					if (useCamera) {
						rect.x += camera.center_pos.x;
						rect.y += camera.center_pos.y;
					}
					if (canvas_clip_dst && doClipping(&rect, canvas_clip_dst))
						continue;

					GPU_RectangleFilled2(target, rect, colour);
					fallback_rects++;
				}
#if defined(PIVAS_DYNAMIC_WINDOW_TRACE) && PIVAS_DYNAMIC_WINDOW_TRACE
				if (pivas_trace_this_draw) {
					SDL_Color first = cache.fallback_colours.empty() ? SDL_Color{0, 0, 0, 0} : cache.fallback_colours.front();
					sendToLog(LogLevel::Info,
					          "[dyn-window] geometry-fallback target=%s rects=%d first=%u,%u,%u,%u\n",
					          pivasTargetName(),
					          fallback_rects,
					          static_cast<unsigned>(first.r),
					          static_cast<unsigned>(first.g),
					          static_cast<unsigned>(first.b),
					          static_cast<unsigned>(first.a));
				}
#endif
#endif

#if PIVAS_DYNAMIC_WINDOW_SOLID_FALLBACK
				GPU_Rect solid_rect = bounds;
				if (useCamera) {
					solid_rect.x += camera.center_pos.x;
					solid_rect.y += camera.center_pos.y;
				}

				bool solid_clipped = canvas_clip_dst && doClipping(&solid_rect, canvas_clip_dst);
				if (!solid_clipped) {
					GPU_SetShapeBlending(false);
					GPU_RectangleFilled2(target, solid_rect, SDL_Color{72, 72, 72, 255});

					const float border = 4.0f;
					GPU_Rect top{solid_rect.x, solid_rect.y, solid_rect.w, border};
					GPU_Rect bottom{solid_rect.x, solid_rect.y + solid_rect.h - border, solid_rect.w, border};
					GPU_Rect left{solid_rect.x, solid_rect.y, border, solid_rect.h};
					GPU_Rect right{solid_rect.x + solid_rect.w - border, solid_rect.y, border, solid_rect.h};
					SDL_Color border_colour{128, 128, 128, 255};
					GPU_RectangleFilled2(target, top, border_colour);
					GPU_RectangleFilled2(target, bottom, border_colour);
					GPU_RectangleFilled2(target, left, border_colour);
					GPU_RectangleFilled2(target, right, border_colour);
				}
#if defined(PIVAS_DYNAMIC_WINDOW_TRACE) && PIVAS_DYNAMIC_WINDOW_TRACE
				if (pivas_trace_this_draw) {
					sendToLog(LogLevel::Info,
					          "[dyn-window] solid-fallback target=%s clipped=%d rect=%g,%g,%g,%g\n",
					          pivasTargetName(),
					          solid_clipped ? 1 : 0,
					          solid_rect.x,
					          solid_rect.y,
					          solid_rect.w,
					          solid_rect.h);
				}
#endif
#endif

#if PIVAS_DYNAMIC_WINDOW_CLEAR_FALLBACK
				GPU_Rect clear_rect = bounds;
				if (useCamera) {
					clear_rect.x += camera.center_pos.x;
					clear_rect.y += camera.center_pos.y;
				}

				bool clear_clipped = canvas_clip_dst && doClipping(&clear_rect, canvas_clip_dst);
				if (!clear_clipped) {
					GPU_FlushBlitBuffer();
					gpu.setClipRect(target, clear_rect);
					gpu.clear(target, 72, 72, 72, 255);
					GPU_UnsetClip(target);
				}
#if defined(PIVAS_DYNAMIC_WINDOW_TRACE) && PIVAS_DYNAMIC_WINDOW_TRACE
				if (pivas_trace_this_draw) {
					sendToLog(LogLevel::Info,
					          "[dyn-window] clear-fallback target=%s clipped=%d rect=%g,%g,%g,%g\n",
					          pivasTargetName(),
					          clear_clipped ? 1 : 0,
					          clear_rect.x,
					          clear_rect.y,
					          clear_rect.w,
					          clear_rect.h);
				}
#endif
#endif

#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_PROBE
				if (pivas_dynamic_window_probe_count++ < 16) {
					GPU_Rect probe = bounds;
					if (useCamera) {
						probe.x += camera.center_pos.x;
						probe.y += camera.center_pos.y;
					}
					if (!canvas_clip_dst || !doClipping(&probe, canvas_clip_dst)) {
						const float gap = 8.0f;
						float tile_w    = (probe.w - gap * 3.0f) / 4.0f;
						float tile_h    = std::max(56.0f, std::min(108.0f, probe.h * 0.30f));
						if (tile_w > 48.0f && tile_h > 32.0f) {
							GPU_Rect dst0{probe.x, probe.y, tile_w, tile_h};
							GPU_Rect dst1{probe.x + (tile_w + gap), probe.y, tile_w, tile_h};
							GPU_Rect dst2{probe.x + (tile_w + gap) * 2.0f, probe.y, tile_w, tile_h};
							GPU_Rect dst3{probe.x + (tile_w + gap) * 3.0f, probe.y, tile_w, tile_h};

							GPU_UnsetClip(target);
							GPU_SetShapeBlending(false);
							GPU_RectangleFilled2(target, dst0, SDL_Color{128, 128, 128, 255});

							GPU_bool src_old_blending       = src->use_blending;
							GPU_BlendMode src_old_blendmode = src->blend_mode;
							GPU_SetBlending(src, false);
							GPU_Rect atlas_src{0, 0,
							                   texture_w > 0 ? texture_w : static_cast<float>(src->w),
							                   texture_h > 0 ? texture_h : static_cast<float>(src->h)};
							pivasDrainDynamicWindowGlErrors("probe-atlas-unblended");
							gpu.copyGPUImage(src, &atlas_src, canvas_clip_dst, target,
							                 dst1.x + dst1.w / 2.0f,
							                 dst1.y + dst1.h / 2.0f,
							                 dst1.w / atlas_src.w,
							                 dst1.h / atlas_src.h,
							                 0,
							                 true);
							pivasCheckDynamicWindowGlError("probe-atlas-unblended");
							src->blend_mode = src_old_blendmode;
							GPU_SetBlending(src, src_old_blending);

							GPU_bool cache_old_blending       = cache.image->use_blending;
							GPU_BlendMode cache_old_blendmode = cache.image->blend_mode;
							pivasDrainDynamicWindowGlErrors("probe-composed-normal");
							gpu.copyGPUImage(cache.image, nullptr, canvas_clip_dst, target,
							                 dst2.x + dst2.w / 2.0f,
							                 dst2.y + dst2.h / 2.0f,
							                 dst2.w / cache.image->w,
							                 dst2.h / cache.image->h,
							                 0,
							                 true);
							pivasCheckDynamicWindowGlError("probe-composed-normal");

							GPU_SetBlending(cache.image, false);
							pivasDrainDynamicWindowGlErrors("probe-composed-unblended");
							gpu.copyGPUImage(cache.image, nullptr, canvas_clip_dst, target,
							                 dst3.x + dst3.w / 2.0f,
							                 dst3.y + dst3.h / 2.0f,
							                 dst3.w / cache.image->w,
							                 dst3.h / cache.image->h,
							                 0,
							                 true);
							pivasCheckDynamicWindowGlError("probe-composed-unblended");
							cache.image->blend_mode = cache_old_blendmode;
							GPU_SetBlending(cache.image, cache_old_blending);

							sendToLog(LogLevel::Info,
							          "[dyn-window-probe] target=%s expected=gray_ref,atlas_unblended,composed_normal,composed_unblended probe=%g,%g,%g,%g tile=%gx%g stats_alpha=%llu\n",
							          pivasTargetName(),
							          probe.x,
							          probe.y,
							          probe.w,
							          probe.h,
							          tile_w,
							          tile_h,
							          cache.output_stats.alpha_pixels);
						}
					}
				}
#endif
			}
#if defined(PIVAS)
			gpu.popBlendMode();
			if (use_physical_source_rects)
				GPU_SetImageVirtualResolution(src, original_image_w, original_image_h);
#endif
			return;
		}
	}
#endif
#if defined(PIVAS) && PIVAS_RENDER_TRACE
	if (pivas_dynamic_window_trace_count++ < 192) {
		GPU_Rect src0{};
		GPU_Rect dst0{};
		if (!blits.empty()) {
			src0 = blits.front().src;
			dst0 = blits.front().dst;
		}
		sendToLog(LogLevel::Info,
		          "[dyn-window] render rm=0x%x use_camera=%d target=%dx%d src=%dx%d blits=%d extension=%d clip=%g,%g,%g,%g first_src=%g,%g,%g,%g first_dst=%g,%g,%g,%g\n",
		          refresh_mode,
		          useCamera ? 1 : 0,
		          target ? target->w : 0,
		          target ? target->h : 0,
		          src->w,
		          src->h,
		          static_cast<int>(blits.size()),
		          wndCtrl.extension,
		          canvas_clip_dst ? canvas_clip_dst->x : 0,
		          canvas_clip_dst ? canvas_clip_dst->y : 0,
		          canvas_clip_dst ? canvas_clip_dst->w : 0,
		          canvas_clip_dst ? canvas_clip_dst->h : 0,
		          src0.x,
		          src0.y,
		          src0.w,
		          src0.h,
		          dst0.x,
		          dst0.y,
		          dst0.w,
		          dst0.h);
	}
#endif
	for (auto blit : blits) {
		GPU_Rect clip_src = blit.src;
		GPU_Rect real_dst = blit.dst;
#if defined(PIVAS)
		if (use_physical_source_rects) {
			clip_src = pivasMapDynamicWindowSourceRect(clip_src, pivas_source_scale_x, pivas_source_scale_y, texture_w, texture_h);
			if (clip_src.w <= 0 || clip_src.h <= 0)
				continue;
		}
#endif
		if (useCamera) {
			real_dst.x += camera.center_pos.x;
			real_dst.y += camera.center_pos.y;
		}
		float coord_x = real_dst.x + (real_dst.w / 2.0);
		float coord_y = real_dst.y + (real_dst.h / 2.0);
		float wResize = 1;
		float hResize = 1;
#if defined(PIVAS)
		if (use_physical_source_rects) {
			if (clip_src.w > 0)
				wResize = real_dst.w / clip_src.w;
			if (clip_src.h > 0)
				hResize = real_dst.h / clip_src.h;
		} else
#endif
		if (real_dst.w > clip_src.w && clip_src.w > 0) {
			wResize = real_dst.w / clip_src.w;
		}
		if (real_dst.h > clip_src.h && clip_src.h > 0) {
			hResize = real_dst.h / clip_src.h;
		}
		if (canvas_clip_dst) {
			if (doClipping(&real_dst, canvas_clip_dst))
				continue;
		}
#if defined(PIVAS) && PIVAS_DYNAMIC_WINDOW_TRACE
		if (pivas_trace_this_draw) {
			sendToLog(LogLevel::Info,
			          "[dyn-window] gpu-blit target=%s src=%g,%g,%g,%g dst=%g,%g,%g,%g ratio=%g,%g blend=%s\n",
			          pivasTargetName(),
			          clip_src.x,
			          clip_src.y,
			          clip_src.w,
			          clip_src.h,
			          real_dst.x,
			          real_dst.y,
			          real_dst.w,
			          real_dst.h,
			          wResize,
			          hResize,
			          pivasBlendModeName(info->blending_mode));
		}
#endif
#if defined(PIVAS)
		pivasDrainDynamicWindowGlErrors("gpu-blit");
#endif
		gpu.copyGPUImage(src, &clip_src, &real_dst, target, coord_x, coord_y, wResize, hResize, 0, true);
#if defined(PIVAS)
		pivasCheckDynamicWindowGlError("gpu-blit");
#endif
	}
#if defined(PIVAS)
	gpu.popBlendMode();
	if (use_physical_source_rects)
		GPU_SetImageVirtualResolution(src, original_image_w, original_image_h);
#endif
}

bool ONScripter::doClickEnd() {
	draw_cursor_flag          = true;
	internal_slowdown_counter = 0;

	if (!((skip_mode & SKIP_TO_EOL) && clickskippage_flag))
		skip_mode &= ~(SKIP_TO_WAIT | SKIP_TO_EOL);

	if (automode_flag) {
		event_mode = WAIT_TEXT_MODE | WAIT_INPUT_MODE |
		             WAIT_VOICE_MODE | WAIT_TIMER_MODE;
		if (automode_time < 0)
			waitEvent(-automode_time * dlgCtrl.dialogueRenderState.clickPartCharacterCount());
		else
			waitEvent(automode_time);
	} else if (autoclick_time > 0) {
		event_mode = WAIT_SLEEP_MODE | WAIT_TIMER_MODE;
		waitEvent(autoclick_time);
	} else {
		event_mode = WAIT_TEXT_MODE | WAIT_INPUT_MODE | WAIT_TIMER_MODE;
		waitEvent(-1);
	}

	draw_cursor_flag = false;

	// previously waitEvent was returning a result
	return false;
}

// "allowed" seems a far better name than the ambiguous "enabled"
bool ONScripter::skipIsAllowed() {
	if (!skip_enabled)
		return false;
	return skip_unread || !script_h.logState.unreadDialogue;
}

bool ONScripter::clickWait() {
	int tmp_skip = skip_mode;
	skip_mode &= ~(SKIP_TO_WAIT | SKIP_TO_EOL);
	internal_slowdown_counter = 0;

	flush(refreshMode());

	//Mion: apparently NScr doesn't call textgosub on clickwaits
	// while in skip mode (but does call it on pagewaits)
	// ^ We don't care what NScr does, its nonsense causes us bugs :D
	if (((skip_mode & (SKIP_NORMAL)) ||
	     ((tmp_skip & SKIP_TO_EOL) && clickskippage_flag) ||
	     keyState.ctrl) &&
	    !textgosub_label) {
		skip_mode      = tmp_skip;
		clickstr_state = CLICK_NONE;
		//if (textgosub_label && (script_h.getNext()[0] != 0x0a))
		//    new_line_skip_flag = true;
		event_mode = IDLE_EVENT_MODE;
		waitEvent(0);
	} else {

		keyState.pressedFlag = false;

		if (textgosub_label) {
			if ((tmp_skip & SKIP_TO_EOL) && clickskippage_flag)
				skip_mode = tmp_skip;
			saveoffCommand();
			clickstr_state = CLICK_NONE;

			const char *next = script_h.getNext();
			if (*next == 0x0a) {
				textgosub_clickstr_state = CLICK_WAITEOL;
			} else {
				new_line_skip_flag       = true;
				textgosub_clickstr_state = CLICK_WAIT;
			}
			if ((skip_mode & SKIP_NORMAL || keyState.ctrl) && skipgosub_label)
				gosubReal(skipgosub_label, next, true);
			else
				gosubReal(textgosub_label, next, true);

			return false;
		}

		clickstr_state = CLICK_WAIT;
		if (doClickEnd())
			return false;

		clickstr_state       = CLICK_NONE;
		keyState.pressedFlag = false;
	}

	return true;
}

bool ONScripter::clickNewPage() {
	skip_mode &= ~(SKIP_TO_WAIT | SKIP_TO_EOL);

	flush(refreshMode());
	clickstr_state = CLICK_NEWPAGE;

	bool skipping{skip_mode & SKIP_NORMAL || keyState.ctrl};

	if (skipping && !textgosub_label) {
		clickstr_state = CLICK_NONE;

		event_mode = IDLE_EVENT_MODE;
		waitEvent(0);
	} else {
		keyState.pressedFlag = false;

		if (textgosub_label) {
			saveoffCommand();
			clickstr_state = CLICK_NONE;

			const char *next         = script_h.getNext();
			textgosub_clickstr_state = CLICK_NEWPAGE;

			if (skipping && skipgosub_label)
				gosubReal(skipgosub_label, next, true);
			else
				gosubReal(textgosub_label, next, true);

			return false;
		}

		if (doClickEnd())
			return false;
	}

#if defined(PIVAS)
	if (dlgCtrl.dialogueProcessingState.active) {
		dlgCtrl.vitaPageClearedByDialogue = true;
		vita_text_gpu_valid               = false;
		vita_text_gpu_last_segment        = -1;
		vita_text_gpu_cached_generation   = 0;
	}
#endif
	newPage(true);
	clickstr_state       = CLICK_NONE;
	keyState.pressedFlag = false;

	return true;
}

int ONScripter::textCommand() {
	if (saveon_flag && internal_saveon_flag) {
		saveSaveFile(-1);
		internal_saveon_flag = false;
	}

	if (dlgCtrl.dialogueProcessingState.active) {
#if defined(PIVAS) && PIVAS_RENDER_TRACE
		if (pivas_text_command_trace_count++ < 192) {
			sendToLog(LogLevel::Info,
			          "[text-command] active page_enter=%d display=0x%x rwtm=0x%x layout=%d ready=%d seg=%d bounds=%g,%g,%g,%g\n",
			          page_enter_status,
			          display_mode,
			          refresh_window_text_mode,
			          dlgCtrl.dialogueProcessingState.layoutDone ? 1 : 0,
			          dlgCtrl.dialogueProcessingState.readyToRun ? 1 : 0,
			          dlgCtrl.dialogueRenderState.segmentIndex,
			          dlgCtrl.dialogueRenderState.bounds.x,
			          dlgCtrl.dialogueRenderState.bounds.y,
			          dlgCtrl.dialogueRenderState.bounds.w,
			          dlgCtrl.dialogueRenderState.bounds.h);
		}
#endif

		script_h.popStringBuffer();

		if (pretextgosub_label && !dlgCtrl.dialogueProcessingState.pretextHasBeenToldToRunOnce) {
			// even in new model we want to handle pretext before allowing dialogueCommand / textCommand to complete, right?
			gosubReal(pretextgosub_label, dlgCtrl.dialogue_pos, true);
			dlgCtrl.dialogueProcessingState.pretextHasBeenToldToRunOnce = true;
			return RET_CONTINUE;
		}

		// feel like it's ok to let these two complete now instead of in CR, too
		if (!dlgCtrl.dialogueProcessingState.layoutDone) {
			dlgCtrl.layoutDialogue();
		}

		if (!page_enter_status) {
			refresh_window_text_mode = REFRESH_NORMAL_MODE | REFRESH_WINDOW_MODE | REFRESH_TEXT_MODE;
			enterTextDisplayMode();
			page_enter_status = 1;
		}

		dlgCtrl.dialogueProcessingState.readyToRun = true;

		//sendToLog(LogLevel::Info, "Start of dialogue dialogue event\n");
		dlgCtrl.events.emplace_get().firstCall = true;

		LabelInfo *label = current_label_info;
		if (!callStack.empty())
			label = callStack.front().label;
		auto id                                  = script_h.getLabelIndex(label);
		script_h.logState.currDialogueLabelIndex = id;
		script_h.logState.unreadDialogue         = !script_h.logState.readLabels[id];

	} else {
		errorAndExit("dlgCtrl is inactive but textCommand was called");
	}
	return RET_CONTINUE;
}

void ONScripter::displayDialogue() {
#if defined(PIVAS) && PIVAS_RENDER_TRACE
	if (pivas_dialogue_display_trace_count++ < 192) {
		sendToLog(LogLevel::Info,
		          "[dialogue-display] display seg=%d active=%d rendering=%d layout=%d ready=%d skip=0x%x bounds=%g,%g,%g,%g click=%d textgosub_click=%d\n",
		          dlgCtrl.dialogueRenderState.segmentIndex,
		          dlgCtrl.dialogueProcessingState.active ? 1 : 0,
		          dlgCtrl.dialogueIsRendering ? 1 : 0,
		          dlgCtrl.dialogueProcessingState.layoutDone ? 1 : 0,
		          dlgCtrl.dialogueProcessingState.readyToRun ? 1 : 0,
		          skip_mode,
		          dlgCtrl.dialogueRenderState.bounds.x,
		          dlgCtrl.dialogueRenderState.bounds.y,
		          dlgCtrl.dialogueRenderState.bounds.w,
		          dlgCtrl.dialogueRenderState.bounds.h,
		          clickstr_state,
		          textgosub_clickstr_state);
	}
#endif
	if (skip_mode) {
		//sendToLog(LogLevel::Info, "skip-mode display dialogue event\n");
		dlgCtrl.events.emplace();
		for (const auto &a : fetchConstantRefreshActions<DialogueController::TextRenderingMonitorAction>()) {
			auto act = dynamic_cast<DialogueController::TextRenderingMonitorAction *>(a.get());
			act->lastCompletedSegment++;
		}
		return;
	}
	dlgCtrl.timeCurrentDialogueSegment();
	dlgCtrl.dialogueIsRendering = true;
	auto segAct                 = DialogueController::SegmentRenderingAction::create();
	segAct->segment             = dlgCtrl.dialogueRenderState.segmentIndex;
	Lock lock(&ons.registeredCRActions);
	registeredCRActions.emplace_back(segAct); // renders segment to completion
}

int ONScripter::getCharacterPreDisplayDelay(char16_t /*codepoint*/, int /*speed*/) {
	return 0;
}

int ONScripter::getCharacterPostDisplayDelay(char16_t codepoint, int speed) {
	int base             = 20;
	uint32_t codepoint_u = codepoint;
	if (codepoint == u'⅓')
		base = 13; // special character indicating the delay for a terminating punctuation which will be followed by another
	else if (codepoint == ',')
		base = 100;
	else if (codepoint == ';' || codepoint == ':' || codepoint == u'—')
		base = 145;
	else if (codepoint == '.' || codepoint == '?' || codepoint == '!')
		base = 170;
	else if (isCJKChar(codepoint_u))
		base = 60;
	return base - (base * speed) / 10;
}

int ONScripter::unpackInlineCall(const char *cmd, int &val) {
	assert(cmd[0] == '!');

	std::string num;
	for (int i = 0; cmd[2 + i] >= '0' && cmd[2 + i] <= '9'; i++) num += cmd[2 + i];
	val = std::stoi(num);

	switch (cmd[1]) {
		case 'w':
			return 0;
		case 'd':
			return 1;
		default:
			ons.errorAndExit("This command cannot not be executed from here"); // for !s and friends
			return -1;                                                         //dummy
	}
}

int ONScripter::executeSingleCommandFromTreeNode(StringTree &command_node) {

	int res = RET_NO_READ;

	std::string &cmd = command_node[0].value;

	for (int i = 1; command_node.has(i); i++) {
		variableQueue.push(command_node.getById(i).value);
	}

	if (cmd.length() >= sizeof(script_h.current_cmd)) {
		errorAndExit("command buffer overflow");
	}

	if (isBuiltInCommand(cmd.c_str())) {
		setVariableQueue(true, cmd);
		// We need to backup & restore SH Data here (following command may kill string_buffer)
		ScriptHandler::ScriptLoanStorable storable{script_h.getScriptStateData()};
		evaluateBuiltInCommand(cmd.c_str());
		script_h.swapScriptStateData(storable);
		setVariableQueue(false);
	} else {
		inVariableQueueSubroutine = true;

		// The caller of tree_exec function should give us proper reexecution position (its start point)
		assert(currentCommandPosition.has());
		script_h.setCurrent(ons.currentCommandPosition.get());

		res = ScriptParser::evaluateCommand(cmd.c_str(), false);
	}

	return res;
}

const char *ONScripter::getFontPath(int i, bool /*fallback*/) {
	const char *path = sentence_font.getFontPath(i);
	if (!path)
		path = sentence_font.getFontPath(0);
	return path;
}

const char *ONScripter::getSubtitleFontDir() {
	char *path = static_cast<char *>(malloc(PATH_MAX));
	std::snprintf(path, PATH_MAX, "%s%c%s", fonts.fontdir, DELIMITER, "subfonts");
	return path;
}

void ONScripter::addTextWindowClip(DirtyRect &rect) {
	if (wndCtrl.usingDynamicTextWindow) {
		// This represents the whole text window, when it is current and active
		// dlgCtrl.dialogueProcessingState.layoutDone == false means we are in pretext
		// pretext is better to think that we are still using a previous window (which is text_gpu)
		// this will avoid possible glitches if it tries to do anything with it
		if (dlgCtrl.dialogueProcessingState.active && dlgCtrl.dialogueProcessingState.layoutDone) {
			auto blits = wndCtrl.getRegions();
			for (auto &b : blits) rect.add(b.dst);
			// At this step we only have text_gpu & window_gpu left, it is guaranteed that text window is no bigger
		} else {
			rect.add({0, 0, static_cast<float>(text_gpu->w), static_cast<float>(text_gpu->h)});
		}
	} else {
		rect.add(sentence_font_info.pos);
	}
}
