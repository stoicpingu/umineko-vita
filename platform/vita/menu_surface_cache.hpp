#pragma once

#include <SDL2/SDL.h>
#include <algorithm>
#include <cstddef>
#include <list>
#include <memory>
#include <new>
#include <string>

namespace VitaMenu {
// Small immutable menu PNGs are repeatedly loaded by the original settings
// handlers. Keep decoded pixels, never mutable animation/GPU state. The caller
// serializes access with the engine's image-loading mutex.
class SurfaceCache {
    using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
    struct Entry {
        std::string path;
        Surface surface;
        size_t bytes;
    };
    std::list<Entry> entries;
    size_t used = 0;
    const size_t budget, perImage, countLimit;

    static SDL_Surface *clone(SDL_Surface *surface) {
        // SDL copies row pitches, palette/colour key and alpha modulation too.
        return SDL_ConvertSurface(surface, surface->format, 0);
    }

public:
    explicit SurfaceCache(size_t bytes = 8 * 1024 * 1024,
                          size_t imageBytes = 1024 * 1024, size_t count = 256)
        : budget(bytes), perImage(imageBytes), countLimit(count) {}

    static std::string normalize(std::string path) {
        std::replace(path.begin(), path.end(), '\\', '/');
        return path;
    }

    static bool eligible(const char *filename) {
        if (!filename) return false;
        std::string path = normalize(filename);
        // These directories are used by the shipped script, including its
        // localized menu_en/config controls. Exclude generated saves/caches.
        if (path.find("../") != std::string::npos || path.find("/./") != std::string::npos)
            return false;
        if (path.compare(0, 14, "graphics/menu/") != 0 &&
            path.compare(0, 14, "graphics/menu_") != 0)
            return false;
        if (path.size() < 4) return false;
        const auto extension = path.substr(path.size() - 4);
        return extension == ".png" || extension == ".PNG";
    }

    static std::string key(const char *filename, const char *resolved, const char *gameRoot) {
        if (!eligible(filename) || !resolved || !gameRoot) return {};
        auto path = normalize(resolved), root = normalize(gameRoot);
        if (root.empty()) return {};
        if (root.back() != '/') root += '/';
        if (path.compare(0, root.size(), root) != 0) return {};
        // DirectReader resolves in search order. Only immutable game-root
        // menu files qualify; a save-root override cannot reuse a game entry.
        auto relative = path.substr(root.size());
        if (relative.compare(0, 3, "jp/") == 0) relative.erase(0, 3);
        else if (relative.compare(0, 12, "language_pt/") == 0) relative.erase(0, 12);
        return eligible(relative.c_str()) ? path : std::string{};
    }

    SDL_Surface *get(const std::string &path) {
        if (path.empty()) return nullptr;
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->path != path) continue;
            SDL_Surface *copy = clone(it->surface.get());
            if (copy) entries.splice(entries.begin(), entries, it);
            return copy; // Allocation failure falls back to the normal loader.
        }
        return nullptr;
    }

    void put(const std::string &path, SDL_Surface *surface) {
        if (path.empty() || !surface || surface->pitch <= 0 || surface->h <= 0 || !countLimit)
            return;
        const size_t pitch = static_cast<size_t>(surface->pitch);
        if (static_cast<size_t>(surface->h) > perImage / pitch) return;
        for (const auto &entry : entries)
            if (entry.path == path) return;
        Surface copy(clone(surface), SDL_FreeSurface);
        if (!copy) return;
        // Reserve palette/SDL structure and list/string overhead as well as
        // the cloned pitch (which can differ from the decoded input pitch).
        const size_t bytes = size_t(copy->pitch) * size_t(copy->h) + 2048 + path.size();
        if (bytes > budget) return;
        try {
            entries.push_front({path, std::move(copy), bytes});
        } catch (const std::bad_alloc &) {
            return; // Caching is optional; the original surface remains valid.
        }
        used += bytes;
        while (used > budget || entries.size() > countLimit) {
            used -= entries.back().bytes;
            entries.pop_back();
        }
    }

    size_t bytes() const { return used; }
    size_t size() const { return entries.size(); }
};
}
