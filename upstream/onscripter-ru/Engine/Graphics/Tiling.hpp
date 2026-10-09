/** Native-independent geometry shared by GPUBigImage and its host tests. */
#pragma once

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace GPUImageTiling {

// Keep integer storage boundaries until applying the virtual view. Independently
// rounded widths accumulate gaps; rounding common edges preserves adjacency.
template <typename Rect>
std::vector<Rect> makeTiles(uint32_t width, uint32_t height, uint32_t limit) {
	if (!width || !height || !limit)
		throw std::invalid_argument("Image tiling requires nonzero dimensions and texture limit");
	std::vector<Rect> tiles;
	for (uint32_t y = 0; y < height;) {
		const uint32_t h = height - y < limit ? height - y : limit;
		for (uint32_t x = 0; x < width;) {
			const uint32_t w = width - x < limit ? width - x : limit;
			tiles.push_back({static_cast<float>(x), static_cast<float>(y),
			                 static_cast<float>(w), static_cast<float>(h)});
			x += w;
		}
		y += h;
	}
	return tiles;
}

template <typename Rect>
Rect virtualTile(const Rect &storage, uint32_t storageW, uint32_t storageH,
                 uint32_t logicalW, uint32_t logicalH) {
	if (!storageW || !storageH || !logicalW || !logicalH)
		throw std::invalid_argument("Virtual image dimensions must be nonzero");
	const auto edge = [](double coordinate, uint32_t logical, uint32_t physical) {
		return static_cast<float>(std::round(coordinate * logical / physical));
	};
	const float x = edge(storage.x, logicalW, storageW);
	const float y = edge(storage.y, logicalH, storageH);
	const float right = edge(double(storage.x) + storage.w, logicalW, storageW);
	const float bottom = edge(double(storage.y) + storage.h, logicalH, storageH);
	return {x, y, right - x, bottom - y};
}

template <typename Rect>
bool intersects(const Rect &a, const Rect &b) {
	return a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 &&
	       a.x < b.x + b.w && a.x + a.w > b.x &&
	       a.y < b.y + b.h && a.y + a.h > b.y;
}
}
