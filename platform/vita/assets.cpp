#include "platform/vita/assets.hpp"
#include "platform/vita/platform.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace {
constexpr size_t maxManifestBytes = 8 * 1024 * 1024;
constexpr size_t maxManifestEntries = 65536;
constexpr size_t maxLineBytes = 1024;
std::unordered_map<std::string, VitaAssets::ImageSize> sizes;
bool loaded = false;

std::string normalise(std::string name) {
	for (auto &c : name) {
		if (c == '\\') c = '/';
		else if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
	}
	while (name.compare(0, 2, "./") == 0)
		name.erase(0, 2);
	return name;
}

std::string relativeKey(const std::string &name) {
	auto key = normalise(name);
	static const std::string root = normalise(VitaPlatform::gameRoot());
	if (key.compare(0, root.size(), root) == 0)
		key.erase(0, root.size());
	return normalise(key);
}

bool png(const std::string &name) {
	return name.size() >= 4 && name.compare(name.size() - 4, 4, ".png") == 0;
}

bool dimension(const std::string &text, int &value) {
	if (text.empty() || text.size() > 5)
		return false;
	unsigned n = 0;
	for (auto c : text) {
		if (c < '0' || c > '9')
			return false;
		n = n * 10 + static_cast<unsigned>(c - '0');
	}
	if (!n || n > 65535)
		return false;
	value = static_cast<int>(n);
	return true;
}
void loadSizes(const std::string &path) {
	FILE *file = std::fopen(path.c_str(), "rb");
	if (!file)
		return;

	size_t bytes = 0;
	while (bytes < maxManifestBytes && sizes.size() < maxManifestEntries) {
		std::string line;
		bool oversized = false;
		int c = EOF;
		while (bytes < maxManifestBytes && (c = std::fgetc(file)) != EOF) {
			++bytes;
			if (c == '\n')
				break;
			if (line.size() < maxLineBytes)
				line.push_back(static_cast<char>(c));
			else
				oversized = true;
		}
		if (oversized || (bytes == maxManifestBytes && c != '\n'))
			continue;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		auto tab = line.find('\t');
		auto tab2 = tab == std::string::npos ? tab : line.find('\t', tab + 1);
		if (tab != std::string::npos && tab2 != std::string::npos) {
			auto key = relativeKey(line.substr(0, tab));
			VitaAssets::ImageSize size{};
			if (png(key) && key.find('\0') == std::string::npos &&
			    dimension(line.substr(tab + 1, tab2 - tab - 1), size.width) &&
			    dimension(line.substr(tab2 + 1), size.height))
				sizes.emplace(std::move(key), size);
		}
		if (c == EOF)
			break;
	}
	std::fclose(file);
}
}

void VitaAssets::initializeImageSizes(const std::string &bundledPath) {
	if (loaded)
		return;
	loaded = true;
	loadSizes(std::string(VitaPlatform::gameRoot()) + "native-image-sizes.tsv");
	// First entry wins: custom asset conversions can override the shipped
	// baseline, while the standard 1.0 data needs no additional loose file.
	loadSizes(bundledPath);
}

void VitaAssets::loadLanguageImageSizes(const std::string &directory) {
	initializeImageSizes();
	loadSizes(directory + "/native-image-sizes.tsv");
}

VitaAssets::ImageSize VitaAssets::logicalImageSize(const std::string &filename, int physicalWidth, int physicalHeight) {
	ImageSize physical{physicalWidth, physicalHeight};
	if (filename.empty() || filename.front() == '>' || filename.front() == '*' ||
	    physicalWidth <= 0 || physicalHeight <= 0)
		return physical;
	auto key = relativeKey(filename);
	if (!png(key))
		return physical;
	auto found = sizes.find(key);
	if (found != sizes.end())
		return found->second;
	// Marker/fill PNGs are copied unchanged by the asset builder. Without
	// exact metadata inverse scaling cannot recover truncation or cell cuts.
	if (physicalWidth <= 4 || physicalHeight <= 4)
		return physical;
	float scale = VitaPlatform::renderScale();
	if (!std::isfinite(scale) || scale < 0.25f || scale > 1.0f)
		return physical;
	return {static_cast<int>(std::lround(physicalWidth / scale)),
	        static_cast<int>(std::lround(physicalHeight / scale))};
}
