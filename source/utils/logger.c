/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#ifndef LOG_FILE_PATH
#define LOG_FILE_PATH DATA_PATH "loader.log"
#endif

#ifndef SOLOADER_WRITE_LOG_FILE
#define SOLOADER_WRITE_LOG_FILE 0
#endif

static SceUID _log_fd = -1;
static bool _log_fd_tried = false;

/*
 * Release builds write no log file. The file writer below is commented out
 * so the SOLOADER_WRITE_LOG_FILE build option cannot re-enable it by
 * accident; uncomment both bodies to restore it.
 */
static void _log_file_open_once(void) {
//#if SOLOADER_WRITE_LOG_FILE
//    if (_log_fd_tried)
//        return;
//    _log_fd_tried = true;
//
//    sceIoMkdir("ux0:data", 0777);
//    sceIoMkdir(DATA_PATH, 0777);
//    _log_fd = sceIoOpen(LOG_FILE_PATH,
//                        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
//#endif
    (void)_log_fd;
    (void)_log_fd_tried;
}

static void _log_file_write(const char *buf, bool force_sync) {
//#if SOLOADER_WRITE_LOG_FILE
//    if (_log_fd >= 0) {
//        sceIoWrite(_log_fd, buf, (SceSize)strlen(buf));
//        /* Without syncing, exFAT reports a stale size to readers while
//         * the process is alive or hung, hiding the log tail. Throttle to
//         * 1/s for info spam, always sync warnings and errors. */
//        static SceUInt64 last_sync_us = 0;
//        SceUInt64 now_us = sceKernelGetProcessTimeWide();
//        if (force_sync || now_us - last_sync_us > 1000000ULL) {
//            sceIoSyncByFd(_log_fd, 0);
//            last_sync_us = now_us;
//        }
//    }
//#else
    (void)buf;
    (void)force_sync;
//#endif
}

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];

void _log_print(int t, const char* fmt, ...) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    switch (t) {
        case LT_DEBUG:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s• debug%s    %s\n",
                            COLOR_PINK, COLOR_END, fmt); break;
        case LT_INFO:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %sℹ info%s     %s\n",
                            COLOR_BLUE, COLOR_END, fmt); break;
        case LT_WARN:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⚠ warning%s  %s\n",
                            COLOR_ORANGE, COLOR_END, fmt); break;
        case LT_ERROR:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⨯ error%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_FATAL:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! fatal%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_SUCCESS:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! success%s  %s\n",
                            COLOR_GREEN, COLOR_END, fmt); break;
        case LT_WAIT:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s… waiting%s  %s\n",
                            COLOR_CYAN, COLOR_END, fmt); break;
        default:
            if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
                sceKernelUnlockLwMutex(&_log_mutex, 1);
            }
            return;
    }

    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);
    sceClibPrintf("%s", buffer_b);

    _log_file_open_once();
    _log_file_write(buffer_b,
                    t == LT_WARN || t == LT_ERROR || t == LT_FATAL);

    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
    }
}
