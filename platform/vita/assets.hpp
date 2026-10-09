#pragma once

#include <string>

namespace VitaAssets {
struct ImageSize {
	int width;
	int height;
};

// Read installation-specific metadata first, then fill gaps from the VPK.
void initializeImageSizes(const std::string &bundledPath = "app0:/native-image-sizes.tsv");
// Add a language pack's metadata without replacing any base-game entries.
void loadLanguageImageSizes(const std::string &directory);
ImageSize logicalImageSize(const std::string &filename, int physicalWidth, int physicalHeight);
}
