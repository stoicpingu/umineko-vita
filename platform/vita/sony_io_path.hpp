#pragma once
#include <algorithm>
#include <cctype>
#include <string>
namespace VitaIO {
inline std::string normalizePath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    auto colon = path.find(':');
    if (colon != path.npos && colon + 1 < path.size() && path[colon + 1] == '/')
        path.erase(colon + 1, 1);
    // Canonicalize harmless ./; leave traversal to the ordinary libc path.
    for (size_t at = path.find("/./"); at != path.npos; at = path.find("/./"))
        path.erase(at, 2);
    return path;
}
inline bool assetPath(const std::string &path, const std::string &root, const char *mode) {
    if (!mode || (std::string(mode) != "r" && std::string(mode) != "rb")) return false;
    std::string lower = path, base = root;
    auto lowercase = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::transform(lower.begin(), lower.end(), lower.begin(), lowercase);
    std::transform(base.begin(), base.end(), base.begin(), lowercase);
    if (lower.compare(0, base.size(), base) != 0) return false;
    auto relative = lower.substr(base.size());
    if (relative.empty() || relative.find("..") != relative.npos) return false;
    // Generated caches and all saved user state bypass FIOS's asset cache.
    for (auto component : {"save/", "saves/", "savedata/", "caches/"})
        if (relative.compare(0, std::char_traits<char>::length(component), component) == 0 ||
            relative.find(std::string("/") + component) != relative.npos) return false;
    // Restrict to known read-only game-resource extensions, not arbitrary files.
    auto dot = relative.find_last_of('.');
    if (dot == relative.npos) return false;
    auto extension = relative.substr(dot);
    for (auto allowed : {".file", ".png", ".jpg", ".jpeg", ".bmp", ".webp", ".ogg", ".wav",
                         ".mp3", ".flac", ".ttf", ".otf", ".nsa", ".ns2", ".ns3", ".sar"})
        if (extension == allowed) return true;
    return false;
}
}
