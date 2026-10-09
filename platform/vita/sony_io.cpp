#include "platform/vita/sony_io.hpp"
#include "platform/vita/sony_io_path.hpp"
#include "platform/vita/fios/fios.h"
#include "platform/vita/fios/sony_stdio.h"
#include <cerrno>
#include <climits>
#include <cstring>
#include <new>
#include <unistd.h>

namespace VitaIO {
static bool ready;
static char root[256];
struct Cookie { void *sony; };

static int read(void *opaque, char *buffer, int bytes) {
    auto cookie = static_cast<Cookie *>(opaque);
    // size=1 avoids the Sony fread byte-count/element-count ambiguity.
    const auto count = onsSonyFread(buffer, 1, static_cast<size_t>(bytes), cookie->sony);
    if (!count && onsSonyFerror(cookie->sony)) { errno = EIO; return -1; }
    return static_cast<int>(count);
}
static fpos_t seek(void *opaque, fpos_t offset, int origin) {
    if (offset < LONG_MIN || offset > LONG_MAX) { errno = EOVERFLOW; return -1; }
    auto sony = static_cast<Cookie *>(opaque)->sony;
    if (onsSonyFseek(sony, static_cast<long>(offset), origin) != 0) { errno = EIO; return -1; }
    auto position = onsSonyFtell(sony);
    if (position < 0) errno = EIO;
    return position;
}
static int close(void *opaque) {
    auto cookie = static_cast<Cookie *>(opaque);
    const int result = onsSonyFclose(cookie->sony);
    delete cookie;
    if (result) errno = EIO;
    return result;
}
void initialize(const char *gameRoot) {
    if (ready) return;
    auto normalized = normalizePath(gameRoot);
    if (normalized.empty() || normalized.back() != '/') normalized += '/';
    if (normalized.size() >= sizeof(root)) return;
    std::memcpy(root, normalized.c_str(), normalized.size() + 1);
    const int result = fios_init(root);
    ready = result == 0;
    if (!ready)
        std::fprintf(stderr, "Native I/O: FIOS unavailable (%d); using newlib\n", result);
    // Cache storage lives until process exit; stdio and worker teardown may
    // still close streams after controller deinitialization. Never free it early.
}
FILE *open(const char *path, const char *mode) {
    if (!ready) return std::fopen(path, mode);
    std::string absolute = path;
    if (absolute.find(':') == absolute.npos && (absolute.empty() || absolute.front() != '/')) {
        char cwd[1024];
        if (!getcwd(cwd, sizeof(cwd))) return std::fopen(path, mode);
        absolute = std::string(cwd) + '/' + absolute;
    }
    absolute = normalizePath(absolute);
    if (absolute.size() >= 256 || !assetPath(absolute, root, mode)) return std::fopen(path, mode);
    void *sony = onsSonyFopen(absolute.c_str(), "rb");
    if (!sony) return std::fopen(path, mode);
    auto cookie = new (std::nothrow) Cookie{sony};
    if (!cookie) { onsSonyFclose(sony); return std::fopen(path, mode); }
    FILE *stream = funopen(cookie, read, nullptr, seek, close);
    if (!stream) { close(cookie); return std::fopen(path, mode); }
    return stream;
}
}
