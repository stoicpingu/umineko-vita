/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/log.h"
#include "utils/logger.h"
#include <psp2/kernel/clib.h>
#include <stdlib.h>

static const char *safe_log_str(const char *s) {
    return s ? s : "(null)";
}

#define print_common \
    const char *safe_tag = safe_log_str(tag); \
    const char *safe_text = safe_log_str(text); \
    switch (prio) { \
        case ANDROID_LOG_INFO: \
            l_info("[ALOG][%s] %s", safe_tag, safe_text); \
            break; \
        case ANDROID_LOG_WARN: \
            l_warn("[ALOG][%s] %s", safe_tag, safe_text); \
            break; \
        case ANDROID_LOG_ERROR: \
        case ANDROID_LOG_FATAL: \
            l_error("[ALOG][%s] %s", safe_tag, safe_text); \
            break; \
        case ANDROID_LOG_UNKNOWN: \
        case ANDROID_LOG_DEFAULT: \
        case ANDROID_LOG_VERBOSE: \
        case ANDROID_LOG_DEBUG: \
        case ANDROID_LOG_SILENT: \
        default: \
            l_debug("[ALOG][%s] %s", safe_tag, safe_text); \
            break; \
    }

int __android_log_write(int prio, const char* tag, const char* text) {
    print_common
    return 0;
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    va_list list;
    char text[1024];

    if (fmt) {
        va_start(list, fmt);
        sceClibVsnprintf(text, sizeof(text), fmt, list);
        va_end(list);
    } else {
        sceClibSnprintf(text, sizeof(text), "(null)");
    }

    print_common

    return 0;
}

int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
    char text[1024];

    if (fmt)
        sceClibVsnprintf(text, sizeof(text), fmt, ap);
    else
        sceClibSnprintf(text, sizeof(text), "(null)");

    print_common

    return 0;
}

void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
    if (fmt) {
        va_list list;
        char text[1024];

        va_start(list, fmt);
        sceClibVsnprintf(text, sizeof(text), fmt, list);
        va_end(list);

        l_fatal("[ALOG][ASSERT] %s", text);
    } else {
        if (cond) {
            l_fatal("[ALOG][ASSERT] Assertion failed: %s", safe_log_str(cond));
        } else {
            l_fatal("[ALOG][ASSERT] Unspecified assertion failed");
        }
    }

    abort();
}
