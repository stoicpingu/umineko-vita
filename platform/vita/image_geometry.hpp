#pragma once
#include <cmath>
#include <cstdint>

namespace VitaGeometry {
// Cell-local script coordinates map to cell-local storage coordinates, so
// rounding an odd-width mouth strip never samples the adjacent mouth cell.
inline int pixelCoordinate(float value, float logicalExtent, int physicalExtent) {
    if (!std::isfinite(value) || !std::isfinite(logicalExtent) || value < 0 ||
        logicalExtent <= 0 || value >= logicalExtent || physicalExtent <= 0)
        return -1;
    return static_cast<int>(std::floor(static_cast<double>(value) * physicalExtent / logicalExtent));
}

inline unsigned alphaAt(const uint8_t *pixels, int width, int height, int pitch,
                        int bytesPerPixel, int x, int y, uint32_t alphaMask, unsigned alphaShift) {
    if (!pixels || x < 0 || y < 0 || x >= width || y >= height ||
        (bytesPerPixel != 3 && bytesPerPixel != 4) || pitch < width * bytesPerPixel)
        return 0;
    if (!alphaMask)
        return 255;
    const auto *p = pixels + y * pitch + x * bytesPerPixel;
    uint32_t value = p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
    if (bytesPerPixel == 4) value |= uint32_t(p[3]) << 24;
    return (value & alphaMask) >> alphaShift;
}

inline void applyTiledMask(uint8_t *pixels, int width, int height, int pitch,
                           const uint8_t *mask, int maskWidth, int maskHeight,
                           int maskPitch, int horizontalCells, bool cropAlpha) {
    if (!pixels || !mask || width <= 0 || height <= 0 || maskWidth <= 0 || maskHeight <= 0 ||
        pitch < width * 4 || maskPitch < maskWidth * 4 || horizontalCells <= 0 ||
        width / horizontalCells == 0) return;
    const int cellWidth = width / horizontalCells;
    for (int y = 0; y < height; ++y) {
        auto *row = pixels + y * pitch;
        const auto *maskRow = mask + (y % maskHeight) * maskPitch;
        for (int x = 0; x < width; ++x) {
            const auto alpha = uint8_t(maskRow[((x % cellWidth) % maskWidth) * 4] ^ 0xff);
            auto &destination = row[x * 4 + 3];
            destination = cropAlpha && destination < alpha ? destination : alpha;
        }
    }
}
}
