#include "reimpl/asset_manager.h"
#include "reimpl/io.h"
#include "utils/logger.h"

#include <pthread.h>
#include <malloc.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <libc_bridge/libc_bridge.h>
#include <string>
#include <fcntl.h>

typedef struct assetManager {
    int dummy = 0; // TODO: mb we will need to store something here in future
    pthread_mutex_t mLock;
} assetManager;

typedef struct aAsset {
    char * filename;
    FILE* f;
    size_t bytesRead;
    size_t fileSize;
    bool opened = false;
} asset;

static AAssetManager * g_AAssetManager = nullptr;

static bool asset_path_is_absolute(const char *path) {
    return path && (path[0] == '/' || strstr(path, ":/") != nullptr);
}

static bool asset_path_starts_with(const char *path, const char *prefix) {
    return path && prefix && strncmp(path, prefix, strlen(prefix)) == 0;
}

static FILE *asset_fopen(const char *path) {
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fopen(path, "rb");
#else
    return fopen(path, "rb");
#endif
}

AAssetManager * AAssetManager_create() {
    if (g_AAssetManager) return g_AAssetManager;

    assetManager am;

    pthread_mutex_init(&am.mLock, nullptr);

    g_AAssetManager = (AAssetManager *) malloc(sizeof(assetManager));
    memcpy(g_AAssetManager, &am, sizeof(assetManager));

    return g_AAssetManager;
}

AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename, int mode) {
    const char *requested = filename ? filename : "";

    std::string candidates[4];
    int candidate_count = 0;
    auto add_candidate = [&](const std::string &path) {
        if (path.empty())
            return;
        for (int i = 0; i < candidate_count; ++i) {
            if (candidates[i] == path)
                return;
        }
        candidates[candidate_count++] = path;
    };

    if (asset_path_is_absolute(requested))
        add_candidate(requested);
    add_candidate(std::string(DATA_PATH) + "assets/" + requested);
    if (asset_path_starts_with(requested, "assets/"))
        add_candidate(std::string(DATA_PATH) + requested);
    add_candidate(std::string(DATA_PATH) + requested);

    FILE *file = nullptr;
    std::string realp;
    int saved_errno = 0;
    for (int i = 0; i < candidate_count; ++i) {
        errno = 0;
        file = asset_fopen(candidates[i].c_str());
        saved_errno = errno;
        if (file) {
            realp = candidates[i];
            break;
        }
    }

    if (!file) {
        l_warn("AAssetManager_open(%p, %s, %d) failed errno=%d",
               mgr, requested, mode, saved_errno);
        return nullptr;
    }

    long file_size = -1;
#ifdef USE_SCELIBC_IO
    if (sceLibcBridge_fseek(file, 0, SEEK_END) == 0)
        file_size = sceLibcBridge_ftell(file);
    sceLibcBridge_fseek(file, 0, SEEK_SET);
#else
    if (fseek(file, 0, SEEK_END) == 0)
        file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
#endif
    if (file_size < 0) {
        l_warn("AAssetManager_open(%p, %s, %d) could not determine size",
               mgr, requested, mode);
#ifdef USE_SCELIBC_IO
        sceLibcBridge_fclose(file);
#else
        fclose(file);
#endif
        return nullptr;
    }

    auto * a = new aAsset;
    a->filename = (char *) malloc(realp.length() + 1);
    strcpy(a->filename, realp.c_str());
    a->bytesRead = 0;
    a->fileSize = (size_t)file_size;
    a->f = file;
    a->opened = true;

    l_debug("AAssetManager_open<%p>(%p, %s, %i): %p", __builtin_return_address(0), mgr, realp.c_str(), mode, a);
    return (AAsset *) a;
}

void AAsset_close(AAsset* asset) {
    l_debug("AAsset_close<%p>(%p)", __builtin_return_address(0), asset);

    if (asset) {
        auto * a = (aAsset *) asset;
        free(a->filename);
        if (a->opened) {
#ifdef USE_SCELIBC_IO
            sceLibcBridge_fclose(a->f);
#else
            fclose(a->f);
#endif
        }
        delete a;
    }
}

int AAsset_read(AAsset* asset, void* buf, size_t count) {
    l_debug("AAsset_read<%p>(%p, %p, %i)", __builtin_return_address(0), asset, buf, count);

    if (!asset) {
        return -1;
    }

    auto * a = (aAsset *) asset;

    if (!a->opened) {
        return -1;
    }

#ifdef USE_SCELIBC_IO
    size_t ret = sceLibcBridge_fread(buf, 1, count, a->f);
#else
    size_t ret = fread(buf, 1, count, a->f);
#endif

    if (ret > 0) {
        a->bytesRead += ret;
        return (int) ret;
    } else {
#ifdef USE_SCELIBC_IO
        if (sceLibcBridge_feof(a->f)) {
#else
        if (feof(a->f)) {
#endif
            return 0;
        } else {
            return -1;
        }
    }
}

off_t AAsset_seek(AAsset* asset, off_t offset, int whence) {
    l_debug("AAsset_seek(%p, %d, %i)", asset, offset, whence);

    if (!asset) {
        return (off_t) -1;
    }

    auto * a = (aAsset *) asset;

    if (!a->opened) {
        return -1;
    }

#ifdef USE_SCELIBC_IO
    int rc = sceLibcBridge_fseek(a->f, offset, whence);
    off_t ret = rc == 0 ? (off_t)sceLibcBridge_ftell(a->f) : (off_t)-1;
#else
    int rc = fseek(a->f, offset, whence);
    off_t ret = rc == 0 ? (off_t)ftell(a->f) : (off_t)-1;
#endif
    if (ret >= 0)
        a->bytesRead = (size_t)ret;

    return ret;
}

off_t AAsset_getRemainingLength(AAsset* asset) {
    l_debug("AAsset_getRemainingLength");
    if (!asset) {
        return 0;
    }

    auto * a = (aAsset *) asset;

    if (!a->opened) {
        return 0;
    }

    if (a->bytesRead >= a->fileSize)
        return 0;

    return (off_t)(a->fileSize - a->bytesRead);
}

off_t AAsset_getLength(AAsset* asset) {
    l_debug("AAsset_getLength");
    if (!asset) {
        return 0;
    }

    auto * a = (aAsset *) asset;
    if (!a->opened)
        return 0;

    return (off_t)a->fileSize;
}

AAssetDir* AAssetManager_openDir(AAssetManager* mgr, const char* dirName) {
    l_error("UNIMPLEMENTED: AAssetManager_openDir: %s", dirName);
    return (AAssetDir *)strdup("dummy");
}

const char* AAssetDir_getNextFileName(AAssetDir* assetDir) {
    l_error("UNIMPLEMENTED: AAssetDir_getNextFileName: %p", assetDir);
    return "";
}

void AAssetDir_close(AAssetDir* assetDir) {
    l_error("UNIMPLEMENTED: AAssetDir_close");
    free(assetDir);
}

int AAsset_openFileDescriptor(AAsset* asset, off_t* outStart, off_t* outLength) {
    if (!asset) {
        l_warn("AAsset_openFileDescriptor(%p, %p, %p): asset is null", asset, outStart, outLength);
        return -1;
    }
    auto * a = (aAsset *) asset;
    if (outStart) *outStart = 0;
    if (outLength) *outLength = a->fileSize;
    if (a->opened) {
        if (a->opened) {
#ifdef USE_SCELIBC_IO
            sceLibcBridge_fclose(a->f);
#else
            fclose(a->f);
#endif
        }
        a->opened = false;
    }
    int ret = open_soloader(a->filename, O_RDONLY);
    l_debug("AAsset_openFileDescriptor(%p/\"%s\", %p, %p): ret %i", asset, a->filename, outStart, outLength, ret);
    return ret;
}
