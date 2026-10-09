/**
 *  GlyphAtlas.hpp
 *  ONScripter-RU
 *
 *  Glyph map in a form of unified atlas for fast rendering.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#pragma once

#include "External/Compatibility.hpp"
#include "Engine/Components/Base.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_gpu.h>

#include <memory>
#include <vector>

// double 4096 is a bit too much for iOS
const int NUM_GLYPH_CACHE = 2048;
const int GLYPH_ATLAS_W   = 2048;
#ifdef VITA
const int GLYPH_ATLAS_H   = 2048;
#else
const int GLYPH_ATLAS_H   = 4096;
#endif

class GlyphAtlasNode {
	std::unique_ptr<GlyphAtlasNode> left, right;
	SDL_Rect rect{};
	bool exists{false};

public:
	void reset(int w, int h);
	SDL_Rect *insert(int w, int h);
};

class GlyphAtlasController : public BaseController {
	GlyphAtlasNode root;
	int width, height;

protected:
	int ownInit() override;
	int ownDeinit() override;

public:
	GlyphAtlasController(int w, int h)
	    : BaseController(this), width(w), height(h) {
		root.reset(width, height);
	}

	bool add(int w, int h, GPU_Rect &pos);
	void reset();
#ifdef VITA
	// Takes ownership of a padded RGBA glyph. Upload together before sampling.
	bool queue(SDL_Surface *surface, GPU_Rect &ink);
	void flush();
private:
	struct Upload { GPU_Rect rect; std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> surface; };
	std::vector<Upload> pending;
public:
#endif

	GPU_Image *atlas{nullptr};
};
