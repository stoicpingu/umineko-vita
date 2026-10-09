/**
 *  TextImageGeometry.hpp
 *  ONScripter-RU
 *
 *  Preserve logical text anchors when generated images include ruby padding.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#pragma once

#include <array>

inline bool useComputedImageCenter(int rotation, bool hasHotspot, bool hasScaleCenter, bool paddedAffine) {
	return rotation != 0 || hasHotspot || hasScaleCenter || paddedAffine;
}

inline std::array<float, 2> textImageCenterOffset(float offsetX, float offsetY, float extraWidth, float extraHeight, bool hasHotspot) {
	return {{offsetX + (hasHotspot ? 0 : extraWidth / 2),
	         offsetY + (hasHotspot ? 0 : extraHeight / 2)}};
}
