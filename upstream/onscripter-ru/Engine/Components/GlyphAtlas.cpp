/**
 *  GlyphAtlas.cpp
 *  ONScripter-RU
 *
 *  Glyph map in a form of unified atlas for fast rendering.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Components/GlyphAtlas.hpp"
#include "Engine/Graphics/GPU.hpp"
#ifdef VITA
#include "platform/vita/glyph_upload.h"
#include <cstring>
#include <stdexcept>
#endif

void GlyphAtlasNode::reset(int w, int h) {
	rect = SDL_Rect{0, 0, w, h};
	left.reset();
	right.reset();
	exists = false;
}

SDL_Rect *GlyphAtlasNode::insert(int w, int h) {
	if (left) {
		// We're not a leaf
		auto newNode = left->insert(w, h);
		if (newNode)
			return newNode;
		return right->insert(w, h);
	}

	// We can't insert here, the entire space is used
	if (exists)
		return nullptr;

	// We can't insert here, the space is too small
	if (w > rect.w || h > rect.h)
		return nullptr;

	// The size is perfect, insert here
	if (w == rect.w && h == rect.h) {
		// let's set exists here...
		exists = true;
		return &rect;
	}

	// We have "more than" enough room here so we must split the space
	left  = std::make_unique<GlyphAtlasNode>();
	right = std::make_unique<GlyphAtlasNode>();

	// Decide which way to split
	auto dw = rect.w - w;
	auto dh = rect.h - h;
	if (dw > dh) {
		left->rect  = SDL_Rect{rect.x, rect.y, w, rect.h};
		right->rect = SDL_Rect{rect.x + w, rect.y, dw, rect.h};
	} else {
		left->rect  = SDL_Rect{rect.x, rect.y, rect.w, h};
		right->rect = SDL_Rect{rect.x, rect.y + h, rect.w, dh};
	}

	// Insert into left node (has sufficient space)
	return left->insert(w, h);
}

int GlyphAtlasController::ownInit() {
	atlas = gpu.createImage(width, height, 4);
#ifndef VITA
	GPU_GetTarget(atlas);
#endif
	return 0;
}

int GlyphAtlasController::ownDeinit() {
	if (atlas)
		gpu.freeImage(atlas);
	return 0;
}

bool GlyphAtlasController::add(int w, int h, GPU_Rect &pos) {
	auto rect = root.insert(w, h);
	if (!rect) {
		return false;
	}
	pos.x = rect->x;
	pos.y = rect->y;
	pos.w = rect->w;
	pos.h = rect->h;
	return true;
}

void GlyphAtlasController::reset() {
	root.reset(width, height);
#ifdef VITA
	// Every new glyph uploads its transparent gutter too. Old unused texels
	// need no clear, and this sampling-only atlas never needs a depth buffer.
	pending.clear();
#else
	gpu.clearWholeTarget(atlas->target);
#endif
}

#ifdef VITA
bool GlyphAtlasController::queue(SDL_Surface *surface, GPU_Rect &ink) {
	std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> owned(surface, SDL_FreeSurface);
	GPU_Rect rect;
	if (!surface || !add(surface->w, surface->h, rect)) return false;
	ink = {rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2};
	pending.push_back({rect, std::move(owned)});
	return true;
}

void GlyphAtlasController::flush() {
	if (pending.empty()) return;
	unsigned pitch = 0;
	auto pixels = ons_vita_begin_glyph_upload(atlas, &pitch);
	if (!pixels) throw std::runtime_error("Invalid native glyph atlas storage");
	for (auto &upload : pending) {
		auto &rect = upload.rect;
		auto src = static_cast<const unsigned char *>(upload.surface->pixels);
		auto dst = pixels + static_cast<unsigned>(rect.y) * pitch + static_cast<unsigned>(rect.x) * 4;
		for (int y = 0; y < upload.surface->h; ++y)
			std::memcpy(dst + y * pitch, src + y * upload.surface->pitch, upload.surface->w * 4);
	}
	pending.clear();
}
#endif
