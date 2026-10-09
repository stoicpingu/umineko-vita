#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <zlib.h>

namespace VitaFontCache {
struct Fingerprint {
    uint64_t bytes{0};
    uint32_t crc{0};
};

// Takes ownership of an opened file; bounded streaming avoids a second image
// or font-sized allocation during startup. CRC detects stale/partial bundles.
inline Fingerprint fingerprint(FILE *file) {
    if (!file) return {};
    Fingerprint result;
    unsigned char buffer[65536];
    size_t size;
    while ((size = std::fread(buffer, 1, sizeof(buffer), file)) != 0) {
        result.bytes += size;
        if (result.bytes > 64 * 1024 * 1024) break;
        result.crc = static_cast<uint32_t>(crc32(result.crc, buffer, size));
    }
    bool valid = !std::ferror(file) && result.bytes <= 64 * 1024 * 1024;
    std::fclose(file);
    return valid ? result : Fingerprint{};
}

// A manifest supplies no filesystem paths and no game variables. It can only
// seed the font marker after every fixed input and output has been verified.
// The original script still compares this marker with the current font config.
template <typename Measure>
std::string bundledMarker(const std::string &manifest, const std::string &script,
                          const char *savedMarker, Measure measure) {
    if ((savedMarker && *savedMarker) || manifest.size() > 4096) return {};
    std::istringstream input(manifest);
    std::string header, language, marker;
    if (!std::getline(input, header) || header != "UMINEKO_NATIVE_FONT_CACHE_V1" ||
        !std::getline(input, language) || language != script ||
        !std::getline(input, marker) || marker.compare(0, 5, "hash#") != 0 ||
        marker.size() > 512 || marker.find('\0') != std::string::npos) return {};
    std::array<Fingerprint, 26> expected{};
    for (auto &entry : expected) {
        uint64_t crc = 0;
        if (!(input >> entry.bytes >> crc) || !entry.bytes ||
            entry.bytes > 64 * 1024 * 1024 || crc > UINT32_MAX) return {};
        entry.crc = static_cast<uint32_t>(crc);
    }
    input >> std::ws;
    if (!input.eof()) return {};
    for (size_t i = 0; i < expected.size(); ++i) {
        Fingerprint actual = measure(i);
        if (actual.bytes != expected[i].bytes || actual.crc != expected[i].crc) return {};
    }
    return marker;
}
} // namespace VitaFontCache
