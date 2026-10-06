/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

static const char *safe_log_str(const char *s) {
    return s ? s : "(null)";
}

static int has_vita_device_prefix(const char *path) {
    const char *colon = strchr(path, ':');
    const char *slash = strchr(path, '/');
    return colon && (!slash || colon < slash);
}

static const char *resolve_relative_path(const char *path, char *buffer,
                                         size_t buffer_size) {
    if (!path || !*path)
        return path;

    if (path[0] == '/' || has_vita_device_prefix(path))
        return path;

    if (strcmp(path, ".") == 0) {
        snprintf(buffer, buffer_size, "%s", DATA_PATH);
        return buffer;
    }

    if (strncmp(path, "./", 2) == 0)
        path += 2;

    snprintf(buffer, buffer_size, "%s%s", DATA_PATH, path);
    return buffer;
}

static void stat_sce_to_bionic(const SceIoStat *src, stat64_bionic *dst) {
    memset(dst, 0, sizeof(*dst));

    if (SCE_S_ISDIR(src->st_mode))
        dst->st_mode = S_IFDIR | 0777;
    else if (SCE_S_ISLNK(src->st_mode))
        dst->st_mode = S_IFLNK | 0777;
    else
        dst->st_mode = S_IFREG | 0666;

    dst->st_nlink = 1;
    dst->st_size = src->st_size;
    dst->st_blksize = 4096;
    dst->st_blocks = (src->st_size + 511) / 512;
}

#define TRACKED_FILE_MAX 64

typedef struct tracked_file {
    int fd;
    FILE *stream;
    char path[PATH_MAX];
} tracked_file;

static tracked_file g_tracked_files[TRACKED_FILE_MAX];

static void stat_size_to_bionic(long long size, stat64_bionic *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->st_mode = S_IFREG | 0644;
    dst->st_nlink = 1;
    dst->st_size = size;
    dst->st_blksize = 4096;
    dst->st_blocks = (size + 511) / 512;
}

static int stream_fileno_raw(FILE *stream) {
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fileno(stream);
#else
    return fileno(stream);
#endif
}

static void track_file_fd(int fd, FILE *stream, const char *path) {
    if (fd < 0 || !path)
        return;

    int free_slot = -1;
    for (int i = 0; i < TRACKED_FILE_MAX; i++) {
        if (g_tracked_files[i].fd == fd) {
            g_tracked_files[i].stream = stream;
            snprintf(g_tracked_files[i].path, sizeof(g_tracked_files[i].path),
                     "%s", path);
            return;
        }
        if (free_slot < 0 && g_tracked_files[i].path[0] == '\0')
            free_slot = i;
    }

    if (free_slot >= 0) {
        g_tracked_files[free_slot].fd = fd;
        g_tracked_files[free_slot].stream = stream;
        snprintf(g_tracked_files[free_slot].path,
                 sizeof(g_tracked_files[free_slot].path), "%s", path);
    } else {
        l_warn("fd path tracker full, cannot track fd %d for %s", fd, path);
    }
}

static FILE *find_tracked_fd_stream(int fd) {
    if (fd < 0)
        return NULL;

    for (int i = 0; i < TRACKED_FILE_MAX; i++) {
        if (g_tracked_files[i].fd == fd &&
            g_tracked_files[i].path[0] != '\0')
            return g_tracked_files[i].stream;
    }

    return NULL;
}

static const char *find_tracked_fd_path(int fd) {
    if (fd < 0)
        return NULL;

    for (int i = 0; i < TRACKED_FILE_MAX; i++) {
        if (g_tracked_files[i].fd == fd &&
            g_tracked_files[i].path[0] != '\0')
            return g_tracked_files[i].path;
    }

    return NULL;
}

static void untrack_fd_path(int fd) {
    if (fd < 0)
        return;

    for (int i = 0; i < TRACKED_FILE_MAX; i++) {
        if (g_tracked_files[i].fd == fd) {
            g_tracked_files[i].fd = 0;
            g_tracked_files[i].stream = NULL;
            g_tracked_files[i].path[0] = '\0';
            return;
        }
    }
}

size_t fread_soloader(void *ptr, size_t size, size_t nmemb, FILE *stream) {
#ifdef USE_SCELIBC_IO
    size_t ret = sceLibcBridge_fread(ptr, size, nmemb, stream);
#else
    size_t ret = fread(ptr, size, nmemb, stream);
#endif

    size_t normalized = ret;
    if (size > 1 && nmemb > 0 && ret > nmemb && ret <= size * nmemb)
        normalized = ret / size;

    /* Short reads at EOF are normal libc behaviour — only the SceLibc
     * byte-count quirk and genuine stream errors are log-worthy. The old
     * unconditional ferror/feof/ftell probes also cost three bridge calls
     * on EVERY fread on the asset hot path; only pay them on short reads. */
    if (normalized != ret || ret != nmemb) {
#ifdef USE_SCELIBC_IO
        int err = sceLibcBridge_ferror(stream);
        int eof = sceLibcBridge_feof(stream);
        long pos = sceLibcBridge_ftell(stream);
#else
        int err = ferror(stream);
        int eof = feof(stream);
        long pos = ftell(stream);
#endif
        if (normalized != ret) {
            l_warn("fread(ptr=%p size=%zu nmemb=%zu fp=%p) returned byte count "
                   "%zu, normalized to %zu err=%d eof=%d pos=%ld",
                   ptr, size, nmemb, stream, ret, normalized, err, eof, pos);
        } else if (err) {
            l_warn("fread(ptr=%p size=%zu nmemb=%zu fp=%p) = %zu err=%d eof=%d "
                   "pos=%ld",
                   ptr, size, nmemb, stream, ret, err, eof, pos);
        }
    }

    return normalized;
}

size_t fwrite_soloader(const void *ptr, size_t size, size_t nmemb,
                       FILE *stream) {
#ifdef USE_SCELIBC_IO
    size_t ret = sceLibcBridge_fwrite(ptr, size, nmemb, stream);
#else
    size_t ret = fwrite(ptr, size, nmemb, stream);
#endif
    if (ret != nmemb)
        l_warn("fwrite(ptr=%p size=%zu nmemb=%zu fp=%p) = %zu", ptr, size,
               nmemb, stream, ret);
    return ret;
}

// SceLibcBridge exposes no clearerr; fseek(0, SEEK_SET) covers the rewind
// semantics the engine relies on. Never let a SceLibc FILE reach newlib
// rewind: the FILE layouts differ and newlib would chase garbage pointers.
void rewind_soloader(FILE *stream) {
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fseek(stream, 0L, SEEK_SET);
    if (ret != 0)
        l_warn("rewind(%p): fseek ret %d", stream, ret);
#else
    rewind(stream);
#endif
}

int fseek_soloader(FILE *stream, long offset, int whence) {
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fseek(stream, offset, whence);
#else
    int ret = fseek(stream, offset, whence);
#endif
    if (ret != 0)
        l_warn("fseek(%p, %ld, %d): %d", stream, offset, whence, ret);
    return ret;
}

long ftell_soloader(FILE *stream) {
#ifdef USE_SCELIBC_IO
    long ret = sceLibcBridge_ftell(stream);
#else
    long ret = ftell(stream);
#endif
    if (ret < 0)
        l_warn("ftell(%p): %ld", stream, ret);
    return ret;
}

int fseeko_soloader(FILE *stream, off_t offset, int whence) {
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fseek(stream, (long)offset, whence);
#else
    int ret = fseeko(stream, offset, whence);
#endif
    if (ret != 0)
        l_warn("fseeko(%p, %lld, %d): %d", stream, (long long)offset, whence, ret);
    return ret;
}

off_t ftello_soloader(FILE *stream) {
#ifdef USE_SCELIBC_IO
    long ret = sceLibcBridge_ftell(stream);
#else
    off_t ret = ftello(stream);
#endif
    if (ret < 0)
        l_warn("ftello(%p): %lld", stream, (long long)ret);
    return (off_t)ret;
}

int fileno_soloader(FILE *stream) {
    return stream_fileno_raw(stream);
}

FILE * fopen_soloader(const char * filename, const char * mode) {
    if (!filename || !mode) {
        errno = EFAULT;
        l_warn("fopen(%s, %s): invalid null argument", safe_log_str(filename),
               safe_log_str(mode));
        return NULL;
    }

    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    }

    char resolved_path[PATH_MAX];
    const char *real_filename =
        resolve_relative_path(filename, resolved_path, sizeof(resolved_path));

    errno = 0;
#ifdef USE_SCELIBC_IO
    FILE* ret = sceLibcBridge_fopen(real_filename, mode);
#else
    FILE* ret = fopen(real_filename, mode);
#endif
    int saved_errno = errno;
    if (!ret && saved_errno == 0) {
        saved_errno = ENOENT;
        errno = saved_errno;
    }

    if (ret) {
        int fd = stream_fileno_raw(ret);
        track_file_fd(fd, ret, real_filename);
    } else if (saved_errno != ENOENT) {
        l_warn("fopen(%s -> %s, %s) failed errno=%d", safe_log_str(filename),
               safe_log_str(real_filename), safe_log_str(mode), saved_errno);
    }

    return ret;
}

FILE * fopen64_soloader(const char * filename, const char * mode) {
    return fopen_soloader(filename, mode);
}

int open_soloader(const char * path, int oflag, ...) {
    if (!path) {
        errno = EFAULT;
        l_warn("open((null), flags_bionic=0x%x): invalid null path", oflag);
        return -1;
    }

    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    } else if (strcmp(path, "/dev/urandom") == 0) {
        return open_soloader("app0:/urandom", oflag);
    }

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    char resolved_path[PATH_MAX];
    const char *real_path = resolve_relative_path(path, resolved_path,
                                                  sizeof(resolved_path));

    int bionic_oflag = oflag;
    oflag = oflags_bionic_to_newlib(oflag);
    errno = 0;
    int ret = open(real_path, oflag, mode);
    int saved_errno = errno;
    if (ret < 0)
        l_warn("open(%s -> %s, flags_bionic=0x%x flags_newlib=0x%x mode=0%o) "
               "failed errno=%d",
               safe_log_str(path), safe_log_str(real_path), bionic_oflag,
               oflag, mode, saved_errno);
    if (ret >= 0)
        track_file_fd(ret, NULL, real_path);
    return ret;
}

int open64_soloader(const char * path, int oflag, ...) {
    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    return open_soloader(path, oflag, mode);
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    errno = 0;
    int res = fstat(fd, &st);
    int saved_errno = errno;

    if (res == 0) {
        stat_newlib_to_bionic(&st, buf);
        return res;
    }

    const char *tracked_path = find_tracked_fd_path(fd);
    if (tracked_path) {
        SceIoStat sce_path_st;
        memset(&sce_path_st, 0, sizeof(sce_path_st));
        int sce_path_res = sceIoGetstat(tracked_path, &sce_path_st);
        if (sce_path_res >= 0) {
            stat_sce_to_bionic(&sce_path_st, buf);
            return 0;
        }

        struct stat tracked_st;
        errno = 0;
        int stat_res = stat(tracked_path, &tracked_st);
        int stat_errno = errno;
        if (stat_res == 0) {
            stat_newlib_to_bionic(&tracked_st, buf);
            return 0;
        }

        l_warn("fstat(%i): tracked path %s stat=%i errno=%d sceIoGetstat=%i",
               fd, safe_log_str(tracked_path), stat_res, stat_errno,
               sce_path_res);
    }

    FILE *tracked_stream = find_tracked_fd_stream(fd);
    if (tracked_stream) {
        long pos = ftell_soloader(tracked_stream);
        int seek_end = fseek_soloader(tracked_stream, 0, SEEK_END);
        long size = ftell_soloader(tracked_stream);
        int seek_restore = 0;
        if (pos >= 0)
            seek_restore = fseek_soloader(tracked_stream, pos, SEEK_SET);
        clearerr(tracked_stream);

        if (seek_end == 0 && size >= 0 && seek_restore == 0) {
            stat_size_to_bionic(size, buf);
            return 0;
        }

        l_warn("fstat(%i): tracked FILE* %p sizing failed pos=%ld "
               "seek_end=%d size=%ld seek_restore=%d",
               fd, tracked_stream, pos, seek_end, size, seek_restore);
    }

    SceIoStat sce_st;
    memset(&sce_st, 0, sizeof(sce_st));
    int sce_res = sceIoGetstatByFd(fd, &sce_st);
    if (sce_res >= 0) {
        stat_sce_to_bionic(&sce_st, buf);
        return 0;
    }

    if (saved_errno == 0)
        saved_errno = EBADF;
    errno = saved_errno;
    l_warn("fstat(%i): newlib=%i errno=%d sceIoGetstatByFd=%i", fd, res,
           saved_errno, sce_res);
    return -1;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    if (!path || !buf) {
        errno = EFAULT;
        l_warn("stat(%s, %p): invalid null argument", safe_log_str(path), buf);
        return -1;
    }

    if (strcmp(path, "/system/lib/libOpenSLES.so") == 0) {
        return 0;
    }

    char resolved_path[PATH_MAX];
    const char *real_path = resolve_relative_path(path, resolved_path,
                                                  sizeof(resolved_path));

    struct stat st;
    int res = stat(real_path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    return res;
}

int fclose_soloader(FILE * f) {
    int fd = stream_fileno_raw(f);
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fclose(f);
#else
    int ret = fclose(f);
#endif

    if (ret == 0)
        untrack_fd_path(fd);
    else
        l_warn("fclose(%p): %i", f, ret);
    return ret;
}

int close_soloader(int fd) {
    int ret = close(fd);
    if (ret == 0)
        untrack_fd_path(fd);
    else
        l_warn("close(%i): %i", fd, ret);
    return ret;
}

ssize_t read_soloader(int fd, void *buf, size_t count) {
    return read(fd, buf, count);
}

off_t lseek_soloader(int fd, off_t offset, int whence) {
    off_t ret = lseek(fd, offset, whence);
    if (ret < 0)
        l_warn("lseek(%d, %lld, %d): %lld", fd, (long long)offset, whence,
               (long long)ret);
    return ret;
}

off_t lseek64_soloader(int fd, off_t offset, int whence) {
    return lseek_soloader(fd, offset, whence);
}

DIR* opendir_soloader(char* _pathname) {
    if (!_pathname) {
        errno = EFAULT;
        l_warn("opendir((null)): invalid null path");
        return NULL;
    }

    char resolved_path[PATH_MAX];
    const char *real_path = resolve_relative_path(_pathname, resolved_path,
                                                  sizeof(resolved_path));
    return opendir(real_path);
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    return ret;
}

int closedir_soloader(DIR * dir) {
    return closedir(dir);
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    return fsync(fd);
}
