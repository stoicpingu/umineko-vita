/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 * Copyright (C) 2026      Ellie J Turner
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dynlib.c
 * @brief Resolving dynamic imports of the .so.
 */

#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <math.h>
#include <netdb.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <zlib.h>
#include <locale.h>
#include <poll.h>
#include <stdarg.h>

#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <so_util/so_util.h>
#include <utime.h>

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/utils.h"
#include "utils/logger.h"

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "reimpl/errno.h"
#include "reimpl/io.h"
#include "reimpl/log.h"
#include "reimpl/mem.h"
#include "reimpl/pthr.h"
#include "reimpl/sys.h"
#include "reimpl/egl.h"
#include "reimpl/time64.h"
#include "reimpl/asset_manager.h"
#include "reimpl/video.h"

const unsigned int __page_size = PAGE_SIZE;

extern void * _ZNSt9exceptionD2Ev;
extern void * _ZSt17__throw_bad_allocv;
extern void * _ZSt9terminatev;
extern void * _ZdaPv;
extern void * _ZdlPv;
extern void * _Znaj;
extern void * __cxa_allocate_exception;
extern void * __cxa_begin_catch;
extern void * __cxa_end_catch;
extern void * __cxa_free_exception;
extern void * __cxa_rethrow;
extern void * __cxa_throw;
extern void * __gxx_personality_v0;
extern void *_ZNSt8bad_castD1Ev;
extern void *_ZTISt8bad_cast;
extern void *_ZTISt9exception;
extern void *_ZTVN10__cxxabiv117__class_type_infoE;
extern void *_ZTVN10__cxxabiv120__si_class_type_infoE;
extern void *_ZTVN10__cxxabiv121__vmi_class_type_infoE;
extern void *_Znwj;
extern void *__aeabi_atexit;
extern void *__aeabi_d2lz;
extern void *__aeabi_d2ulz;
extern void *__aeabi_dadd;
extern void *__aeabi_dcmpgt;
extern void *__aeabi_dcmplt;
extern void *__aeabi_ddiv;
extern void *__aeabi_dmul;
extern void *__aeabi_f2lz;
extern void *__aeabi_f2ulz;
extern void *__aeabi_i2d;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_l2d;
extern void *__aeabi_l2f;
extern void *__aeabi_ldivmod;
extern void *__aeabi_memclr;
extern void *__aeabi_memcpy;
extern void *__aeabi_memmove;
extern void *__aeabi_memset4;
extern void *__aeabi_memset8;
extern void *__aeabi_memset;
extern void *__aeabi_ui2d;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_ul2d;
extern void *__aeabi_ul2f;
extern void *__aeabi_uldivmod;
extern void *__aeabi_unwind_cpp_pr0;
extern void *__aeabi_unwind_cpp_pr1;
extern void *__cxa_atexit;
extern void *__cxa_call_unexpected;
extern void *__cxa_finalize;
extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;
extern void *__cxa_pure_virtual;
extern void *__gnu_ldivmod_helper;
extern void *__gnu_unwind_frame;
extern void *__srget;
extern void *__stack_chk_guard;
extern void *__swbuf;

extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;

static FILE __sF_fake[3];

void *dlsym_soloader(void * handle, const char * symbol);
extern so_module so_mod;
static so_module *g_unwind_mod = NULL;
static uintptr_t unwind_find_exidx_soloader(uintptr_t pc, int *pcount);

static uintptr_t android_looper_stub;
static uintptr_t android_window_stub;
static uintptr_t android_sensor_manager_stub;
static uintptr_t android_sensor_queue_stub;
static uintptr_t android_sensor_stub;
static void *android_sensor_list[] = { &android_sensor_stub };

#define BIONIC_SC_CLK_TCK 0x0006
#define BIONIC_SC_OPEN_MAX 0x000b
#define BIONIC_SC_ATEXIT_MAX 0x0025
#define BIONIC_SC_IOV_MAX 0x0026
#define BIONIC_SC_PAGESIZE 0x0027
#define BIONIC_SC_PAGE_SIZE 0x0028
#define BIONIC_SC_GETGR_R_SIZE_MAX 0x0047
#define BIONIC_SC_GETPW_R_SIZE_MAX 0x0048
#define BIONIC_SC_LOGIN_NAME_MAX 0x0049
#define BIONIC_SC_THREAD_DESTRUCTOR_ITERATIONS 0x004a
#define BIONIC_SC_THREAD_KEYS_MAX 0x004b
#define BIONIC_SC_THREAD_STACK_MIN 0x004c
#define BIONIC_SC_THREAD_THREADS_MAX 0x004d
#define BIONIC_SC_TTY_NAME_MAX 0x004e
#define BIONIC_SC_THREADS 0x004f
#define BIONIC_SC_THREAD_ATTR_STACKADDR 0x0050
#define BIONIC_SC_THREAD_ATTR_STACKSIZE 0x0051
#define BIONIC_SC_THREAD_PRIORITY_SCHEDULING 0x0052
#define BIONIC_SC_THREAD_PRIO_INHERIT 0x0053
#define BIONIC_SC_THREAD_PRIO_PROTECT 0x0054
#define BIONIC_SC_THREAD_SAFE_FUNCTIONS 0x0055
#define BIONIC_SC_NPROCESSORS_CONF 0x0060
#define BIONIC_SC_NPROCESSORS_ONLN 0x0061
#define BIONIC_SC_PHYS_PAGES 0x0062
#define BIONIC_SC_AVPHYS_PAGES 0x0063
#define BIONIC_SC_MONOTONIC_CLOCK 0x0064
#define BIONIC_SC_LEVEL1_ICACHE_SIZE 0x008f
#define BIONIC_SC_LEVEL1_ICACHE_ASSOC 0x0090
#define BIONIC_SC_LEVEL1_ICACHE_LINESIZE 0x0091
#define BIONIC_SC_LEVEL1_DCACHE_SIZE 0x0092
#define BIONIC_SC_LEVEL1_DCACHE_ASSOC 0x0093
#define BIONIC_SC_LEVEL1_DCACHE_LINESIZE 0x0094
#define BIONIC_SC_LEVEL2_CACHE_SIZE 0x0095
#define BIONIC_SC_LEVEL2_CACHE_ASSOC 0x0096
#define BIONIC_SC_LEVEL2_CACHE_LINESIZE 0x0097

static long sysconf_soloader(int name) {
    switch (name) {
    case BIONIC_SC_CLK_TCK:
        return 100;
    case BIONIC_SC_OPEN_MAX:
        return 256;
    case BIONIC_SC_ATEXIT_MAX:
        return 2147483647L;
    case BIONIC_SC_IOV_MAX:
        return 1024;
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE:
        return PAGE_SIZE;
    case BIONIC_SC_GETGR_R_SIZE_MAX:
    case BIONIC_SC_GETPW_R_SIZE_MAX:
        return 1024;
    case BIONIC_SC_LOGIN_NAME_MAX:
    case BIONIC_SC_TTY_NAME_MAX:
        return 256;
    case BIONIC_SC_THREAD_DESTRUCTOR_ITERATIONS:
        return 4;
    case BIONIC_SC_THREAD_KEYS_MAX:
        return 128;
    case BIONIC_SC_THREAD_STACK_MIN:
        return 16 * 1024;
    case BIONIC_SC_THREAD_THREADS_MAX:
        return 1024;
    case BIONIC_SC_THREADS:
    case BIONIC_SC_THREAD_ATTR_STACKADDR:
    case BIONIC_SC_THREAD_ATTR_STACKSIZE:
    case BIONIC_SC_THREAD_PRIORITY_SCHEDULING:
    case BIONIC_SC_THREAD_SAFE_FUNCTIONS:
    case BIONIC_SC_MONOTONIC_CLOCK:
        return 1;
    case BIONIC_SC_THREAD_PRIO_INHERIT:
    case BIONIC_SC_THREAD_PRIO_PROTECT:
        return -1;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN:
        return 4;
    case BIONIC_SC_PHYS_PAGES:
        return (256 * 1024 * 1024) / PAGE_SIZE;
    case BIONIC_SC_AVPHYS_PAGES:
        return (128 * 1024 * 1024) / PAGE_SIZE;
    case BIONIC_SC_LEVEL1_ICACHE_SIZE:
    case BIONIC_SC_LEVEL1_DCACHE_SIZE:
        return 32 * 1024;
    case BIONIC_SC_LEVEL1_ICACHE_ASSOC:
    case BIONIC_SC_LEVEL1_DCACHE_ASSOC:
        return 4;
    case BIONIC_SC_LEVEL1_ICACHE_LINESIZE:
    case BIONIC_SC_LEVEL1_DCACHE_LINESIZE:
    case BIONIC_SC_LEVEL2_CACHE_LINESIZE:
        return 32;
    case BIONIC_SC_LEVEL2_CACHE_SIZE:
        return 512 * 1024;
    case BIONIC_SC_LEVEL2_CACHE_ASSOC:
        return 8;
    default:
        errno = EINVAL;
        l_warn("sysconf(%d): unimplemented Android query.", name);
        return -1;
    }
}

static void *ALooper_forThread_soloader(void) {
    return &android_looper_stub;
}

static void *ALooper_prepare_soloader(int opts) {
    (void)opts;
    return &android_looper_stub;
}

static int ALooper_pollAll_soloader(int timeoutMillis, int *outFd,
                                    int *outEvents, void **outData) {
    if (outFd) *outFd = -1;
    if (outEvents) *outEvents = 0;
    if (outData) *outData = NULL;

    if (timeoutMillis > 0)
        sceKernelDelayThread(timeoutMillis * 1000);

    return -3; // ALOOPER_POLL_TIMEOUT
}

static void *ANativeWindow_fromSurface_soloader(void *env, void *surface) {
    (void)env;
    (void)surface;
    return &android_window_stub;
}

static int ANativeWindow_getWidth_soloader(void *window) {
    (void)window;
    return 960;
}

static int ANativeWindow_getHeight_soloader(void *window) {
    (void)window;
    return 544;
}

static void ANativeWindow_release_soloader(void *window) {
    (void)window;
}

static int ANativeWindow_setBuffersGeometry_soloader(void *window, int width,
                                                     int height, int format) {
    (void)window;
    (void)width;
    (void)height;
    (void)format;
    return 0;
}

static void *ASensorManager_getInstance_soloader(void) {
    return &android_sensor_manager_stub;
}

static int ASensorManager_getSensorList_soloader(void *manager, void *list) {
    (void)manager;
    if (list)
        *(void ***)list = android_sensor_list;
    return 0;
}

static void *ASensorManager_createEventQueue_soloader(void *manager,
                                                      void *looper,
                                                      int ident,
                                                      void *callback,
                                                      void *data) {
    (void)manager;
    (void)looper;
    (void)ident;
    (void)callback;
    (void)data;
    return &android_sensor_queue_stub;
}

static int ASensorManager_destroyEventQueue_soloader(void *manager,
                                                     void *queue) {
    (void)manager;
    (void)queue;
    return 0;
}

static int ASensorEventQueue_enableSensor_soloader(void *queue, void *sensor) {
    (void)queue;
    (void)sensor;
    return 0;
}

static int ASensorEventQueue_disableSensor_soloader(void *queue, void *sensor) {
    (void)queue;
    (void)sensor;
    return 0;
}

static int ASensorEventQueue_setEventRate_soloader(void *queue, void *sensor,
                                                   int32_t usec) {
    (void)queue;
    (void)sensor;
    (void)usec;
    return 0;
}

static ssize_t ASensorEventQueue_getEvents_soloader(void *queue, void *events,
                                                    size_t count) {
    (void)queue;
    (void)events;
    (void)count;
    return 0;
}

static const char *ASensor_getName_soloader(void *sensor) {
    (void)sensor;
    return "VitaSensorStub";
}

static int ASensor_getType_soloader(void *sensor) {
    (void)sensor;
    return 1; // ASENSOR_TYPE_ACCELEROMETER
}

static int ASensor_getMinDelay_soloader(void *sensor) {
    (void)sensor;
    return 16666;
}

static unsigned int arc4random_soloader(void) {
    return ((unsigned int)rand() << 16) ^ (unsigned int)rand();
}

static void abort_message_soloader(const char *fmt, ...) {
    char msg[1024];
    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(msg, sizeof(msg), fmt ? fmt : "(null)", list);
    va_end(list);

    l_fatal("abort_message: %s", msg);
    abort();
}

static char *strndup_soloader(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *copy = malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

static char *strdup_soloader(const char *s) {
    return strndup_soloader(s, strlen(s));
}

static const char *printf_safe_string(const char *s) {
    return s ? s : "(null)";
}

static int normalize_printf_format(const char *src, char *dst, size_t dst_size) {
    if (!src || !dst_size)
        return 0;

    size_t di = 0;
    int changed = 0;

    for (size_t si = 0; src[si] && di + 1 < dst_size; ++si) {
        char c = src[si];
        dst[di++] = c;

        if (c != '%')
            continue;

        if (src[si + 1] == '%') {
            if (di + 1 < dst_size)
                dst[di++] = src[++si];
            continue;
        }

        while (src[si + 1] && strchr("#0- +'", src[si + 1]) && di + 1 < dst_size)
            dst[di++] = src[++si];

        if (src[si + 1] == '*') {
            if (di + 1 < dst_size)
                dst[di++] = src[++si];
        } else {
            while (isdigit((unsigned char)src[si + 1]) && di + 1 < dst_size)
                dst[di++] = src[++si];
        }

        if (src[si + 1] == '.' && di + 1 < dst_size) {
            dst[di++] = src[++si];
            if (src[si + 1] == '*') {
                if (di + 1 < dst_size)
                    dst[di++] = src[++si];
            } else {
                while (isdigit((unsigned char)src[si + 1]) && di + 1 < dst_size)
                    dst[di++] = src[++si];
            }
        }

        if (src[si + 1] == 'z') {
            ++si;
            changed = 1;
        }
    }

    dst[di] = '\0';
    return changed;
}

static int vsnprintf_soloader(char *str, size_t size, const char *format,
                              va_list ap) {
    if (!format) {
        if (str && size)
            str[0] = '\0';
        return 0;
    }

    if (strcmp(format, "font%zu%s") == 0) {
        size_t index = va_arg(ap, size_t);
        const char *ext = va_arg(ap, const char *);
        return snprintf(str, size, "font%u%s", (unsigned)index,
                        printf_safe_string(ext));
    }

    if (strcmp(format, "default%s") == 0) {
        const char *ext = va_arg(ap, const char *);
        return snprintf(str, size, "default%s", printf_safe_string(ext));
    }

    if (strcmp(format, "%s%s") == 0) {
        const char *a = va_arg(ap, const char *);
        const char *b = va_arg(ap, const char *);
        return snprintf(str, size, "%s%s", printf_safe_string(a),
                        printf_safe_string(b));
    }

    if (strcmp(format, "%s") == 0) {
        const char *s = va_arg(ap, const char *);
        return snprintf(str, size, "%s", printf_safe_string(s));
    }

    char normalized[512];
    if (normalize_printf_format(format, normalized, sizeof(normalized)))
        return vsnprintf(str, size, normalized, ap);

    return vsnprintf(str, size, format, ap);
}

static int snprintf_soloader(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf_soloader(str, size, format, ap);
    va_end(ap);
    return ret;
}

static int vsprintf_soloader(char *str, const char *format, va_list ap) {
    return vsnprintf_soloader(str, 0x7fffffffU, format, ap);
}

static int sprintf_soloader(char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsprintf_soloader(str, format, ap);
    va_end(ap);
    return ret;
}

static int mprotect_soloader(void *addr, size_t len, int prot) {
    (void)addr;
    (void)len;
    (void)prot;
    return 0;
}

static ssize_t readlink_soloader(const char *path, char *buf, size_t bufsiz) {
    (void)path;
    (void)buf;
    (void)bufsiz;
    errno = EINVAL;
    return -1;
}

static int execlp_soloader(const char *file, const char *arg, ...) {
    (void)file;
    (void)arg;
    errno = ENOSYS;
    return -1;
}

static pid_t vfork_soloader(void) {
    errno = ENOSYS;
    return -1;
}

typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} DlInfoSoloader;

static int dladdr_soloader(const void *addr, void *info) {
    (void)addr;
    if (info)
        memset(info, 0, sizeof(DlInfoSoloader));
    return 0;
}

static void glGetUniformfv_soloader(GLuint program, GLint location,
                                    GLfloat *params) {
    (void)program;
    (void)location;
    if (params)
        *params = 0.0f;
}

static void glGetUniformiv_soloader(GLuint program, GLint location,
                                    GLint *params) {
    (void)program;
    (void)location;
    if (params)
        *params = 0;
}

static const GLubyte *glGetString_soloader(GLenum name) {
    static const GLubyte glsl_es_100[] = "OpenGL ES GLSL ES 1.00";
    const GLubyte *ret = glGetString(name);

    if (name == GL_SHADING_LANGUAGE_VERSION && ret &&
        strcmp((const char *)ret, "1.00 ES") == 0)
        return glsl_es_100;

    return ret;
}

so_default_dynlib default_dynlib[] = {
        // PIVAS loader services (native SceAvPlayer video bridge,
        // source/reimpl/video.c; consumed by Engine/Layers/Media.cpp)
        { "pivas_video_open", (uintptr_t)&pivas_video_open },
        { "pivas_video_dimensions", (uintptr_t)&pivas_video_dimensions },
        { "pivas_video_poll", (uintptr_t)&pivas_video_poll },
        { "pivas_video_frame_to_texture", (uintptr_t)&pivas_video_frame_to_texture },
        { "pivas_video_texture_release", (uintptr_t)&pivas_video_texture_release },
        { "pivas_video_frame_to_cpu", (uintptr_t)&pivas_video_frame_to_cpu },
        { "pivas_video_set_looping", (uintptr_t)&pivas_video_set_looping },
        { "pivas_video_close", (uintptr_t)&pivas_video_close },
        // In-game system message dialog (source/utils/dialog.c; consumed by
        // Engine/Core/ONScripter.cpp pivasShowUnavailablePopup)
        { "pivas_msg_dialog_open", (uintptr_t)&pivas_msg_dialog_open },
        { "pivas_msg_dialog_running", (uintptr_t)&pivas_msg_dialog_running },

        // Common C/C++ internals
        { "_ZNSt8bad_castD1Ev", (uintptr_t)&_ZNSt8bad_castD1Ev },
        { "_ZNSt9exceptionD2Ev", (uintptr_t)&_ZNSt9exceptionD2Ev },
        { "_ZSt17__throw_bad_allocv", (uintptr_t)&_ZSt17__throw_bad_allocv },
        { "_ZSt9terminatev", (uintptr_t)&_ZSt9terminatev },
        { "_ZTISt8bad_cast", (uintptr_t)&_ZTISt8bad_cast },
        { "_ZTISt9exception", (uintptr_t)&_ZTISt9exception },
        { "_ZTVN10__cxxabiv117__class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv117__class_type_infoE },
        { "_ZTVN10__cxxabiv120__si_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv120__si_class_type_infoE },
        { "_ZTVN10__cxxabiv121__vmi_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv121__vmi_class_type_infoE },
        { "_ZdaPv", (uintptr_t)&_ZdaPv },
        { "_ZdlPv", (uintptr_t)&_ZdlPv },
        { "_Znaj", (uintptr_t)&operator_new_array_soloader },
        { "_Znwj", (uintptr_t)&operator_new_soloader },
        { "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
        { "__aeabi_d2lz", (uintptr_t)&__aeabi_d2lz },
        { "__aeabi_d2ulz", (uintptr_t)&__aeabi_d2ulz },
        { "__aeabi_dadd", (uintptr_t)&__aeabi_dadd },
        { "__aeabi_dcmpgt", (uintptr_t)&__aeabi_dcmpgt },
        { "__aeabi_dcmplt", (uintptr_t)&__aeabi_dcmplt },
        { "__aeabi_ddiv", (uintptr_t)&__aeabi_ddiv },
        { "__aeabi_dmul", (uintptr_t)&__aeabi_dmul },
        { "__aeabi_f2lz", (uintptr_t)&__aeabi_f2lz },
        { "__aeabi_f2ulz", (uintptr_t)&__aeabi_f2ulz },
        { "__aeabi_i2d", (uintptr_t)&__aeabi_i2d },
        { "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
        { "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
        { "__aeabi_l2d", (uintptr_t)&__aeabi_l2d },
        { "__aeabi_l2f", (uintptr_t)&__aeabi_l2f },
        { "__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod },
        { "__aeabi_memclr", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memclr4", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memclr8", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memcpy", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memmove", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memmove4", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memmove8", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memset", (uintptr_t)&__aeabi_memset },
        { "__aeabi_memset4",  (uintptr_t)&__aeabi_memset4 },
        { "__aeabi_memset8", (uintptr_t)&__aeabi_memset8 },
        { "__aeabi_ui2d", (uintptr_t)&__aeabi_ui2d },
        { "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
        { "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
        { "__aeabi_ul2d", (uintptr_t)&__aeabi_ul2d },
        { "__aeabi_ul2f", (uintptr_t)&__aeabi_ul2f },
        { "__aeabi_uldivmod", (uintptr_t)&__aeabi_uldivmod },
        { "__aeabi_unwind_cpp_pr0", (uintptr_t)&__aeabi_unwind_cpp_pr0 },
        { "__aeabi_unwind_cpp_pr1", (uintptr_t)&__aeabi_unwind_cpp_pr1 },
        { "__atomic_cmpxchg", (uintptr_t)&__atomic_cmpxchg },
        { "__atomic_dec", (uintptr_t)&__atomic_dec },
        { "__atomic_inc", (uintptr_t)&__atomic_inc },
        { "__atomic_swap", (uintptr_t)&__atomic_swap },
        { "__cxa_allocate_exception", (uintptr_t)&__cxa_allocate_exception },
        { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
        { "__cxa_begin_catch", (uintptr_t)&__cxa_begin_catch },
        { "__cxa_begin_cleanup", (uintptr_t)&ret0 },
        { "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
        { "__cxa_end_catch", (uintptr_t)&__cxa_end_catch },
        { "__cxa_finalize", (uintptr_t)&__cxa_finalize },
        { "__cxa_free_exception", (uintptr_t)&__cxa_free_exception },
        { "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
        { "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
        { "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual },
        { "__cxa_rethrow", (uintptr_t)&__cxa_rethrow },
        { "__cxa_throw", (uintptr_t)&__cxa_throw },
        { "__cxa_type_match", (uintptr_t)&ret0 },
        { "__gnu_Unwind_Find_exidx", (uintptr_t)&unwind_find_exidx_soloader },
        { "__gnu_ldivmod_helper", (uintptr_t)&__gnu_ldivmod_helper },
        { "__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame },
        { "__google_potentially_blocking_region_begin", (uintptr_t)&ret0 },
        { "__google_potentially_blocking_region_end", (uintptr_t)&ret0 },
        { "__gxx_personality_v0", (uintptr_t)&__gxx_personality_v0 },
        { "__isinf", (uintptr_t)&ret0 },
        { "__page_size", (uintptr_t)&__page_size },
        { "__sF", (uintptr_t)&__sF_fake },
        { "__srget", (uintptr_t)&__srget },
        { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_soloader },
        { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard },
        { "__swbuf", (uintptr_t)&__swbuf },
        { "__system_property_get", (uintptr_t)&__system_property_get_soloader },
        { "__assert2", (uintptr_t)&assert2 },
        { "dl_unwind_find_exidx", (uintptr_t)&unwind_find_exidx_soloader },


        // ctype
        { "_ctype_", (uintptr_t)&BIONIC_ctype_ },
        { "_tolower_tab_", (uintptr_t)&BIONIC_tolower_tab_ },
        { "_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_ },
        { "isalnum", (uintptr_t)&isalnum },
        { "isalpha", (uintptr_t)&isalpha },
        { "isblank", (uintptr_t)&isblank },
        { "iscntrl", (uintptr_t)&iscntrl },
        { "isgraph", (uintptr_t)&isgraph },
        { "islower", (uintptr_t)&islower },
        { "isprint", (uintptr_t)&isprint },
        { "ispunct", (uintptr_t)&ispunct },
        { "isspace", (uintptr_t)&isspace },
        { "isupper", (uintptr_t)&isupper },
        { "isxdigit", (uintptr_t)&isxdigit },
        { "tolower", (uintptr_t)&tolower },
        { "toupper", (uintptr_t)&toupper },


        // Android SDK standard logging
        { "__android_log_assert", (uintptr_t)&__android_log_assert },
        { "__android_log_print", (uintptr_t)&__android_log_print },
        { "__android_log_vprint", (uintptr_t)&__android_log_vprint },
        { "__android_log_write", (uintptr_t)&__android_log_write },


        // AAssetManager
        { "AAsset_close", (uintptr_t)&AAsset_close },
        { "AAsset_getLength", (uintptr_t)&AAsset_getLength },
        { "AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength },
        { "AAsset_read", (uintptr_t)&AAsset_read },
        { "AAsset_seek", (uintptr_t)&AAsset_seek },
        { "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor },
        { "AAssetDir_close", (uintptr_t)&AAssetDir_close },
        { "AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName },
        { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_create },
        { "AAssetManager_open", (uintptr_t)&AAssetManager_open },
        { "AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir },

        // Android native window, looper, and sensor shims used by SDL.
        { "ALooper_forThread", (uintptr_t)&ALooper_forThread_soloader },
        { "ALooper_pollAll", (uintptr_t)&ALooper_pollAll_soloader },
        { "ALooper_prepare", (uintptr_t)&ALooper_prepare_soloader },
        { "ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface_soloader },
        { "ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight_soloader },
        { "ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth_soloader },
        { "ANativeWindow_release", (uintptr_t)&ANativeWindow_release_soloader },
        { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry_soloader },
        { "ASensorEventQueue_disableSensor", (uintptr_t)&ASensorEventQueue_disableSensor_soloader },
        { "ASensorEventQueue_enableSensor", (uintptr_t)&ASensorEventQueue_enableSensor_soloader },
        { "ASensorEventQueue_getEvents", (uintptr_t)&ASensorEventQueue_getEvents_soloader },
        { "ASensorEventQueue_setEventRate", (uintptr_t)&ASensorEventQueue_setEventRate_soloader },
        { "ASensorManager_createEventQueue", (uintptr_t)&ASensorManager_createEventQueue_soloader },
        { "ASensorManager_destroyEventQueue", (uintptr_t)&ASensorManager_destroyEventQueue_soloader },
        { "ASensorManager_getInstance", (uintptr_t)&ASensorManager_getInstance_soloader },
        { "ASensorManager_getSensorList", (uintptr_t)&ASensorManager_getSensorList_soloader },
        { "ASensor_getMinDelay", (uintptr_t)&ASensor_getMinDelay_soloader },
        { "ASensor_getName", (uintptr_t)&ASensor_getName_soloader },
        { "ASensor_getType", (uintptr_t)&ASensor_getType_soloader },


        // Math
        { "acos", (uintptr_t)&acos },
        { "acosf", (uintptr_t)&acosf },
        { "asin", (uintptr_t)&asin },
        { "asinf", (uintptr_t)&asinf },
        { "atan", (uintptr_t)&atan },
        { "atan2", (uintptr_t)&atan2 },
        { "atan2f", (uintptr_t)&atan2f },
        { "atanf", (uintptr_t)&atanf },
        { "cbrt", (uintptr_t)&cbrt },
        { "cbrtf", (uintptr_t)&cbrtf },
        { "ceil", (uintptr_t)&ceil },
        { "ceilf", (uintptr_t)&ceilf },
        { "cos", (uintptr_t)&cos },
        { "cosh", (uintptr_t)&cosh },
        { "cosf", (uintptr_t)&cosf },
        { "exp", (uintptr_t)&exp },
        { "exp2", (uintptr_t)&exp2 },
        { "exp2f", (uintptr_t)&exp2f },
        { "expf", (uintptr_t)&expf },
        { "fabs", (uintptr_t)&fabs },
        { "floor", (uintptr_t)&floor },
        { "floorf", (uintptr_t)&floorf },
        { "fmax", (uintptr_t)&fmax },
        { "fmaxf", (uintptr_t)&fmaxf },
        { "fmin", (uintptr_t)&fmin },
        { "fminf", (uintptr_t)&fminf },
        { "frexpf", (uintptr_t)&frexpf },
        { "fmod", (uintptr_t)&fmod },
        { "fmodf", (uintptr_t)&fmodf },
        { "frexp", (uintptr_t)&frexp },
        { "hypot", (uintptr_t)&hypot },
        { "ldexp", (uintptr_t)&ldexp },
        { "ldexpf", (uintptr_t)&ldexpf },
        { "log", (uintptr_t)&log },
        { "log10", (uintptr_t)&log10 },
        { "log10f", (uintptr_t)&log10f },
        { "logf", (uintptr_t)&logf },
        { "llrint", (uintptr_t)&llrint },
        { "llrintf", (uintptr_t)&llrintf },
        { "lrint", (uintptr_t)&lrint },
        { "lrintf", (uintptr_t)&lrintf },
        { "lround", (uintptr_t)&lround },
        { "lroundf", (uintptr_t)&lroundf },
        { "modf", (uintptr_t)&modf },
        { "pow", (uintptr_t)&pow },
        { "powf", (uintptr_t)&powf },
        { "remainder", (uintptr_t)&remainder },
        { "rint", (uintptr_t)&rint },
        { "rintf", (uintptr_t)&rintf },
        { "round", (uintptr_t)&round },
        { "roundf", (uintptr_t)&roundf },
        { "scalbn", (uintptr_t)&scalbn },
        { "scalbnf", (uintptr_t)&scalbnf },
        { "sin", (uintptr_t)&sin },
        { "sincos", (uintptr_t)&sincos },
        { "sincosf", (uintptr_t)&sincosf },
        { "sinf", (uintptr_t)&sinf },
        { "sinh", (uintptr_t)&sinh },
        { "sinhf", (uintptr_t)&sinhf },
        { "sqrt", (uintptr_t)&sqrt },
        { "sqrtf", (uintptr_t)&sqrtf },
        { "tan", (uintptr_t)&tan },
        { "tanf", (uintptr_t)&tanf },
        { "tanh", (uintptr_t)&tanh },
        { "trunc", (uintptr_t)&trunc },
        { "truncf", (uintptr_t)&truncf },


        // Sockets
        { "accept", (uintptr_t)&accept },
        { "bind", (uintptr_t)&bind },
        { "connect", (uintptr_t)&connect },
        { "freeaddrinfo", (uintptr_t)&freeaddrinfo },
        { "gai_strerror", (uintptr_t)&ret0 },
        { "getaddrinfo", (uintptr_t)&getaddrinfo },
        { "gethostbyaddr", (uintptr_t)&gethostbyaddr },
        { "gethostbyname", (uintptr_t)&gethostbyname },
        { "gethostname", (uintptr_t)&gethostname },
        { "getpeername", (uintptr_t)&getpeername },
        { "getservbyname", (uintptr_t)&getservbyname },
        { "getsockname", (uintptr_t)&getsockname },
        { "getsockopt", (uintptr_t)&getsockopt },
        { "inet_aton", (uintptr_t)&inet_aton },
        { "inet_pton", (uintptr_t)&inet_pton },
        { "inet_ntoa", (uintptr_t)&inet_ntoa },
        { "inet_ntop", (uintptr_t)&inet_ntop },
        { "listen", (uintptr_t)&listen },
        { "poll", (uintptr_t)&poll },
        { "recv", (uintptr_t)&recv },
        { "recvfrom", (uintptr_t)&recvfrom },
        { "recvmsg", (uintptr_t)&recvmsg },
        { "select", (uintptr_t)&select },
        { "send", (uintptr_t)&send },
        { "sendmsg", (uintptr_t)&sendmsg },
        { "sendto", (uintptr_t)&sendto },
        { "setsockopt", (uintptr_t)&setsockopt },
        { "shutdown", (uintptr_t)&shutdown },
        { "socket", (uintptr_t)&socket },


        // Memory
        { "calloc", (uintptr_t)&calloc_soloader },
        { "free", (uintptr_t)&free_soloader },
        { "malloc", (uintptr_t)&malloc_soloader },
        { "memalign", (uintptr_t)&memalign_soloader },
        { "memcmp", (uintptr_t)&memcmp },
        { "memcpy", (uintptr_t)&sceClibMemcpy },
        { "memmem", (uintptr_t)&memmem },
        { "memmove", (uintptr_t)&memmove },
        { "memset", (uintptr_t)&memset },
        { "mprotect", (uintptr_t)&mprotect_soloader },
        { "mmap", (uintptr_t)&mmap },
        { "__mmap2", (uintptr_t)&mmap },
        { "munmap", (uintptr_t)&munmap },
        { "realloc", (uintptr_t)&realloc_soloader },
        { "valloc", (uintptr_t)&valloc },


        // IO
        { "close", (uintptr_t)&close_soloader },
        { "closedir", (uintptr_t)&closedir_soloader },
        { "execlp", (uintptr_t)&execlp_soloader },
        { "execv", (uintptr_t)&ret0 },
        { "fclose", (uintptr_t)&fclose_soloader },
        { "fcntl", (uintptr_t)&fcntl_soloader },
        { "fopen", (uintptr_t)&fopen_soloader },
        { "fstat", (uintptr_t)&fstat_soloader },
        { "fsync", (uintptr_t)&fsync_soloader },
        { "ioctl", (uintptr_t)&ioctl_soloader },
        { "__open_2", (uintptr_t)&open_soloader },
        { "open", (uintptr_t)&open_soloader },
        { "open64", (uintptr_t)&open64_soloader },
        { "opendir", (uintptr_t)&opendir_soloader },
        { "readdir", (uintptr_t)&readdir_soloader },
        { "readdir_r", (uintptr_t)&readdir_r_soloader },
        { "stat", (uintptr_t)&stat_soloader },
        { "utime", (uintptr_t)&utime },

        #ifdef USE_SCELIBC_IO
            { "fdopen", (uintptr_t)&sceLibcBridge_fdopen },
            { "feof", (uintptr_t)&sceLibcBridge_feof },
            { "ferror", (uintptr_t)&sceLibcBridge_ferror },
            { "fflush", (uintptr_t)&sceLibcBridge_fflush },
            { "fgetc", (uintptr_t)&sceLibcBridge_fgetc },
            { "fgetpos", (uintptr_t)&sceLibcBridge_fgetpos },
            { "fgets", (uintptr_t)&sceLibcBridge_fgets },
            { "fileno", (uintptr_t)&fileno_soloader },
            { "fputc", (uintptr_t)&sceLibcBridge_fputc },
            { "fputs", (uintptr_t)&sceLibcBridge_fputs },
            { "fread", (uintptr_t)&fread_soloader },
            { "freopen", (uintptr_t)&sceLibcBridge_freopen },
            { "fseek", (uintptr_t)&fseek_soloader },
            { "fsetpos", (uintptr_t)&sceLibcBridge_fsetpos },
            { "ftell", (uintptr_t)&ftell_soloader },
            { "fwide", (uintptr_t)&sceLibcBridge_fwide },
            { "fwrite", (uintptr_t)&sceLibcBridge_fwrite },
            { "getc", (uintptr_t)&sceLibcBridge_getc },
            { "getwc", (uintptr_t)&sceLibcBridge_getwc },
            { "putc", (uintptr_t)&sceLibcBridge_putc },
            { "putchar", (uintptr_t)&sceLibcBridge_putchar },
            { "puts", (uintptr_t)&sceLibcBridge_puts },
            { "putwc", (uintptr_t)&sceLibcBridge_putwc },
            { "setvbuf", (uintptr_t)&sceLibcBridge_setvbuf },
            { "ungetc", (uintptr_t)&sceLibcBridge_ungetc },
            { "ungetwc", (uintptr_t)&sceLibcBridge_ungetwc },
        #else
            { "fdopen", (uintptr_t)&fdopen },
            { "feof", (uintptr_t)&feof },
            { "ferror", (uintptr_t)&ferror },
            { "fflush", (uintptr_t)&fflush },
            { "fgetc", (uintptr_t)&fgetc },
            { "fgetpos", (uintptr_t)&fgetpos },
            { "fgets", (uintptr_t)&fgets },
            { "fileno", (uintptr_t)&fileno_soloader },
            { "fputc", (uintptr_t)&fputc },
            { "fputs", (uintptr_t)&fputs },
            { "fread", (uintptr_t)&fread_soloader },
            { "freopen", (uintptr_t)&freopen },
            { "fseek", (uintptr_t)&fseek_soloader },
            { "fsetpos", (uintptr_t)&fsetpos },
            { "ftell", (uintptr_t)&ftell_soloader },
            { "fwide", (uintptr_t)&fwide },
            { "fwrite", (uintptr_t)&fwrite },
            { "getc", (uintptr_t)&getc },
            { "getwc", (uintptr_t)&getwc },
            { "putc", (uintptr_t)&putc },
            { "putchar", (uintptr_t)&putchar },
            { "puts", (uintptr_t)&puts },
            { "putwc", (uintptr_t)&putwc },
            { "setvbuf", (uintptr_t)&setvbuf },
            { "ungetc", (uintptr_t)&ungetc },
            { "ungetwc", (uintptr_t)&ungetwc },
        #endif

        { "access", (uintptr_t)&access },
        { "basename", (uintptr_t)&basename },
        { "chdir", (uintptr_t)&chdir },
        { "chmod", (uintptr_t)&chmod },
        { "dup", (uintptr_t)&dup },
        { "fopen64", (uintptr_t)&fopen64_soloader },
        { "fseeko", (uintptr_t)&fseeko_soloader },
        { "ftello", (uintptr_t)&ftello_soloader },
        { "ftruncate", (uintptr_t)&ftruncate },
        { "getcwd", (uintptr_t)&getcwd },
        { "isatty", (uintptr_t)&isatty },
        { "lseek", (uintptr_t)&lseek_soloader },
        { "lseek64", (uintptr_t)&lseek64_soloader },
        { "lstat", (uintptr_t)&lstat },
        { "mkdir", (uintptr_t)&mkdir },
        { "pipe", (uintptr_t)&pipe },
        { "read", (uintptr_t)&read_soloader },
        { "readlink", (uintptr_t)&readlink_soloader },
        { "realpath", (uintptr_t)&realpath },
        { "remove", (uintptr_t)&remove },
        { "rename", (uintptr_t)&rename },
        { "rewind", (uintptr_t)&rewind_soloader },
        { "rmdir", (uintptr_t)&rmdir },
        { "truncate", (uintptr_t)&truncate },
        { "unlink", (uintptr_t)&unlink },
        { "write", (uintptr_t)&write },


        // *printf, *scanf
        { "snprintf", (uintptr_t)&snprintf_soloader },
        { "sprintf", (uintptr_t)&sprintf_soloader },
        { "vasprintf", (uintptr_t)&vasprintf },
        { "vprintf", (uintptr_t)&vprintf },
        { "vsnprintf", (uintptr_t)&vsnprintf_soloader },
        { "vsprintf", (uintptr_t)&vsprintf_soloader },
        { "vsscanf", (uintptr_t)&vsscanf },
        { "vswprintf", (uintptr_t)&vswprintf },
        { "printf", (uintptr_t)&sceClibPrintf },
        { "swprintf", (uintptr_t)&swprintf },

        #ifdef USE_SCELIBC_IO
            { "fprintf", (uintptr_t)&sceLibcBridge_fprintf },
            { "fscanf", (uintptr_t)&sceLibcBridge_fscanf },
            { "sscanf", (uintptr_t)&sceLibcBridge_sscanf },
            { "vfprintf", (uintptr_t)&sceLibcBridge_vfprintf },
        #else
            { "fprintf", (uintptr_t)&fprintf },
            { "fscanf", (uintptr_t)&fscanf },
            { "sscanf", (uintptr_t)&sscanf },
            { "vfprintf", (uintptr_t)&vfprintf },
        #endif


        // EGL
        { "eglBindAPI", (uintptr_t)&eglBindAPI },
        { "eglChooseConfig", (uintptr_t)&eglChooseConfig },
        { "eglCreateContext", (uintptr_t)&eglCreateContext },
        { "eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface },
        { "eglDestroyContext", (uintptr_t)&eglDestroyContext },
        { "eglDestroySurface", (uintptr_t)&eglDestroySurface },
        { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
        { "eglGetConfigs", (uintptr_t)&eglGetConfigs },
        { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
        { "eglGetDisplay", (uintptr_t)&eglGetDisplay },
        { "eglGetError", (uintptr_t)&eglGetError },
        { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
        { "eglInitialize", (uintptr_t)&eglInitialize },
        { "eglMakeCurrent", (uintptr_t)&eglMakeCurrent },
        { "eglQueryContext", (uintptr_t)&eglQueryContext },
        { "eglQueryString", (uintptr_t)&eglQueryString },
        { "eglQuerySurface", (uintptr_t)&eglQuerySurface },
        { "eglSwapBuffers", (uintptr_t)&eglSwapBuffers },
        { "eglTerminate", (uintptr_t)&eglTerminate },


        // OpenGL
        { "glActiveTexture", (uintptr_t)&glActiveTexture },
        { "glAlphaFunc", (uintptr_t)&glAlphaFunc },
        { "glAlphaFuncx", (uintptr_t)&glAlphaFuncx },
        { "glAttachShader", (uintptr_t)&glAttachShader },
        { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
        { "glBindBuffer", (uintptr_t)&glBindBuffer },
        { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
        { "glBindFramebufferOES", (uintptr_t)&glBindFramebuffer },
        { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer },
        { "glBindRenderbufferOES", (uintptr_t)&glBindRenderbuffer },
        { "glBindTexture", (uintptr_t)&glBindTexture },
        { "glBlendColor", (uintptr_t)&ret0 },
        { "glBlendEquation", (uintptr_t)&glBlendEquation },
        { "glBlendEquationOES", (uintptr_t)&glBlendEquation },
        { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate },
        { "glBlendEquationSeparateOES", (uintptr_t)&glBlendEquationSeparate },
        { "glBlendFunc", (uintptr_t)&glBlendFunc },
        { "glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate },
        { "glBlendFuncSeparateOES", (uintptr_t)&glBlendFuncSeparate },
        { "glBufferData", (uintptr_t)&glBufferData },
        { "glBufferSubData", (uintptr_t)&glBufferSubData },
        { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
        { "glCheckFramebufferStatusOES", (uintptr_t)&glCheckFramebufferStatus },
        { "glClear", (uintptr_t)&glClear_soloader },
        { "glClearColor", (uintptr_t)&glClearColor_soloader },
        { "glClearColorx", (uintptr_t)&glClearColorx },
        { "glClearDepthf", (uintptr_t)&glClearDepthf },
        { "glClearDepthx", (uintptr_t)&glClearDepthx },
        { "glClearStencil", (uintptr_t)&glClearStencil },
        { "glClientActiveTexture", (uintptr_t)&glClientActiveTexture },
        { "glClipPlanef", (uintptr_t)&glClipPlanef },
        { "glClipPlanex", (uintptr_t)&glClipPlanex },
        { "glColor4f", (uintptr_t)&glColor4f },
        { "glColor4ub", (uintptr_t)&glColor4ub },
        { "glColor4x", (uintptr_t)&glColor4x },
        { "glColorMask", (uintptr_t)&glColorMask },
        { "glColorPointer", (uintptr_t)&glColorPointer },
        { "glCompileShader", (uintptr_t)&glCompileShader_soloader },
        { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
        { "glCompressedTexSubImage2D", (uintptr_t)&ret0 },
        { "glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D },
        { "glCopyTexSubImage2D", (uintptr_t)&glCopyTexSubImage2D },
        { "glCreateProgram", (uintptr_t)&glCreateProgram },
        { "glCreateShader", (uintptr_t)&glCreateShader },
        { "glCullFace", (uintptr_t)&glCullFace },
        { "glCurrentPaletteMatrixOES", (uintptr_t)&ret0 },
        { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
        { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
        { "glDeleteFramebuffersOES", (uintptr_t)&glDeleteFramebuffers },
        { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
        { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers },
        { "glDeleteRenderbuffersOES", (uintptr_t)&glDeleteRenderbuffers },
        { "glDeleteShader", (uintptr_t)&glDeleteShader },
        { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
        { "glDepthFunc", (uintptr_t)&glDepthFunc },
        { "glDepthMask", (uintptr_t)&glDepthMask },
        { "glDepthRangef", (uintptr_t)&glDepthRangef },
        { "glDepthRangex", (uintptr_t)&glDepthRangex },
        { "glDetachShader", (uintptr_t)&ret0 },
        { "glDisable", (uintptr_t)&glDisable },
        { "glDisableClientState", (uintptr_t)&glDisableClientState },
        { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
        { "glDrawArrays", (uintptr_t)&glDrawArrays },
        { "glDrawElements", (uintptr_t)&glDrawElements },
        { "glDrawTexfOES", (uintptr_t)&ret0 },
        { "glDrawTexfvOES", (uintptr_t)&ret0 },
        { "glDrawTexiOES", (uintptr_t)&ret0 },
        { "glDrawTexivOES", (uintptr_t)&ret0 },
        { "glDrawTexsOES", (uintptr_t)&ret0 },
        { "glDrawTexsvOES", (uintptr_t)&ret0 },
        { "glDrawTexxOES", (uintptr_t)&ret0 },
        { "glDrawTexxvOES", (uintptr_t)&ret0 },
        { "glEGLImageTargetRenderbufferStorageOES", (uintptr_t)&ret0 },
        { "glEGLImageTargetTexture2DOES", (uintptr_t)&ret0 },
        { "glEnable", (uintptr_t)&glEnable },
        { "glEnableClientState", (uintptr_t)&glEnableClientState },
        { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
        { "glFinish", (uintptr_t)&glFinish },
        { "glFlush", (uintptr_t)&glFlush },
        { "glFogf", (uintptr_t)&glFogf },
        { "glFogfv", (uintptr_t)&glFogfv },
        { "glFogx", (uintptr_t)&glFogx },
        { "glFogxv", (uintptr_t)&glFogxv },
        { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
        { "glFramebufferRenderbufferOES", (uintptr_t)&glFramebufferRenderbuffer },
        { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
        { "glFramebufferTexture2DOES", (uintptr_t)&glFramebufferTexture2D },
        { "glFrontFace", (uintptr_t)&glFrontFace },
        { "glFrustumf", (uintptr_t)&glFrustumf },
        { "glFrustumx", (uintptr_t)&glFrustumx },
        { "glGenBuffers", (uintptr_t)&glGenBuffers },
        { "glGenerateMipmap", (uintptr_t)&glGenerateMipmap },
        { "glGenerateMipmapOES", (uintptr_t)&glGenerateMipmap },
        { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
        { "glGenFramebuffersOES", (uintptr_t)&glGenFramebuffers },
        { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
        { "glGenRenderbuffersOES", (uintptr_t)&glGenRenderbuffers },
        { "glGenTextures", (uintptr_t)&glGenTextures },
        { "glGetActiveAttrib", (uintptr_t)&glGetActiveAttrib },
        { "glGetActiveUniform", (uintptr_t)&glGetActiveUniform },
        { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation_soloader },
        { "glGetBooleanv", (uintptr_t)&glGetBooleanv },
        { "glGetBufferParameteriv", (uintptr_t)&glGetBufferParameteriv },
        { "glGetBufferPointervOES", (uintptr_t)&ret0 },
        { "glGetClipPlanef", (uintptr_t)&ret0 },
        { "glGetClipPlanex", (uintptr_t)&ret0 },
        { "glGetError", (uintptr_t)&glGetError },
        { "glGetFixedv", (uintptr_t)&ret0 },
        { "glGetFloatv", (uintptr_t)&glGetFloatv },
        { "glGetFramebufferAttachmentParameterivOES", (uintptr_t)&glGetFramebufferAttachmentParameteriv },
        { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
        { "glGetLightfv", (uintptr_t)&ret0 },
        { "glGetLightxv", (uintptr_t)&ret0 },
        { "glGetMaterialfv", (uintptr_t)&ret0 },
        { "glGetMaterialxv", (uintptr_t)&ret0 },
        { "glGetPointerv", (uintptr_t)&ret0 },
        { "glGetRenderbufferParameterivOES", (uintptr_t)&ret0 },
        { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
        { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
        { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
        { "glGetShaderSource", (uintptr_t)&glGetShaderSource },
        { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
        { "glGetString", (uintptr_t)&glGetString_soloader },
        { "glGetTexEnvfv", (uintptr_t)&ret0 },
        { "glGetTexEnviv", (uintptr_t)&glGetTexEnviv },
        { "glGetTexEnvxv", (uintptr_t)&ret0 },
        { "glGetTexGenfvOES", (uintptr_t)&ret0 },
        { "glGetTexGenivOES", (uintptr_t)&ret0 },
        { "glGetTexGenxvOES", (uintptr_t)&ret0 },
        { "glGetTexParameterfv", (uintptr_t)&ret0 },
        { "glGetTexParameteriv", (uintptr_t)&ret0 },
        { "glGetTexParameterxv", (uintptr_t)&ret0 },
        { "glGetUniformfv", (uintptr_t)&glGetUniformfv_soloader },
        { "glGetUniformiv", (uintptr_t)&glGetUniformiv_soloader },
        { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation_soloader },
        { "glHint", (uintptr_t)&glHint },
        { "glIsBuffer", (uintptr_t)&ret0 },
        { "glIsRenderbuffer", (uintptr_t)&glIsRenderbuffer },
        { "glIsEnabled", (uintptr_t)&glIsEnabled },
        { "glIsFramebufferOES", (uintptr_t)&glIsFramebuffer },
        { "glIsRenderbufferOES", (uintptr_t)&glIsRenderbuffer },
        { "glIsTexture", (uintptr_t)&glIsTexture },
        { "glLightf", (uintptr_t)&ret0 },
        { "glLightfv", (uintptr_t)&glLightfv },
        { "glLightModelf", (uintptr_t)&ret0 },
        { "glLightModelfv", (uintptr_t)&glLightModelfv },
        { "glLightModelx", (uintptr_t)&ret0 },
        { "glLightModelxv", (uintptr_t)&glLightModelxv },
        { "glLightx", (uintptr_t)&ret0 },
        { "glLightxv", (uintptr_t)&glLightxv },
        { "glLineWidth", (uintptr_t)&glLineWidth },
        { "glLineWidthx", (uintptr_t)&glLineWidthx },
        { "glLinkProgram", (uintptr_t)&glLinkProgram_soloader },
        { "glLoadIdentity", (uintptr_t)&glLoadIdentity },
        { "glLoadMatrixf", (uintptr_t)&glLoadMatrixf },
        { "glLoadMatrixx", (uintptr_t)&glLoadMatrixx },
        { "glLoadPaletteFromModelViewMatrixOES", (uintptr_t)&ret0 },
        { "glLogicOp", (uintptr_t)&ret0 },
        { "glMapBuffer", (uintptr_t)&glMapBuffer },
        { "glMapBufferOES", (uintptr_t)&glMapBuffer },
        { "glMaterialf", (uintptr_t)&glMaterialf },
        { "glMaterialfv", (uintptr_t)&glMaterialfv },
        { "glMaterialx", (uintptr_t)&glMaterialx },
        { "glMaterialxv", (uintptr_t)&glMaterialxv },
        { "glMatrixIndexPointerOES", (uintptr_t)&ret0 },
        { "glMatrixMode", (uintptr_t)&glMatrixMode },
        { "glMultiTexCoord4f", (uintptr_t)&ret0 },
        { "glMultiTexCoord4x", (uintptr_t)&ret0},
        { "glMultMatrixf", (uintptr_t)&glMultMatrixf },
        { "glMultMatrixx", (uintptr_t)&glMultMatrixx },
        { "glNormal3f", (uintptr_t)&glNormal3f },
        { "glNormal3x", (uintptr_t)&glNormal3x },
        { "glNormalPointer", (uintptr_t)&glNormalPointer },
        { "glOrthof", (uintptr_t)&glOrthof },
        { "glOrthox", (uintptr_t)&glOrthox },
        { "glPixelStorei", (uintptr_t)&glPixelStorei_soloader },
        { "glPointParameterf", (uintptr_t)&ret0 },
        { "glPointParameterfv", (uintptr_t)&ret0 },
        { "glPointParameterx", (uintptr_t)&ret0 },
        { "glPointParameterxv", (uintptr_t)&ret0 },
        { "glPointSize", (uintptr_t)&glPointSize },
        { "glPointSizePointerOES", (uintptr_t)&ret0 },
        { "glPointSizex", (uintptr_t)&glPointSizex },
        { "glPolygonOffset", (uintptr_t)&glPolygonOffset },
        { "glPolygonOffsetx", (uintptr_t)&glPolygonOffsetx },
        { "glPopMatrix", (uintptr_t)&glPopMatrix },
        { "glPushMatrix", (uintptr_t)&glPushMatrix },
        { "glQueryMatrixxOES", (uintptr_t)&ret0 },
        { "glReadPixels", (uintptr_t)&glReadPixels },
        { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage },
        { "glRenderbufferStorageOES", (uintptr_t)&glRenderbufferStorage },
        { "glRotatef", (uintptr_t)&glRotatef },
        { "glRotatex", (uintptr_t)&glRotatex },
        { "glSampleCoverage", (uintptr_t)&ret0 },
        { "glSampleCoveragex", (uintptr_t)&ret0 },
        { "glScalef", (uintptr_t)&glScalef },
        { "glScalex", (uintptr_t)&glScalex },
        { "glScissor", (uintptr_t)&glScissor },
        { "glShadeModel", (uintptr_t)&glShadeModel },
        { "glShaderBinary", (uintptr_t)&glShaderBinary },
        { "glShaderSource", (uintptr_t)&glShaderSource_soloader },
        { "glStencilFunc", (uintptr_t)&glStencilFunc },
        { "glStencilFuncSeparate", (uintptr_t)&glStencilFuncSeparate },
        { "glStencilMask", (uintptr_t)&glStencilMask },
        { "glStencilOp", (uintptr_t)&glStencilOp },
        { "glStencilOpSeparate", (uintptr_t)&glStencilOpSeparate },
        { "glTexCoordPointer", (uintptr_t)&glTexCoordPointer },
        { "glTexEnvf", (uintptr_t)&glTexEnvf },
        { "glTexEnvfv", (uintptr_t)&glTexEnvfv },
        { "glTexEnvi", (uintptr_t)&glTexEnvi },
        { "glTexEnviv", (uintptr_t)&ret0 },
        { "glTexEnvx", (uintptr_t)&glTexEnvx },
        { "glTexEnvxv", (uintptr_t)&glTexEnvxv },
        { "glTexGenfOES", (uintptr_t)&ret0 },
        { "glTexGenfvOES", (uintptr_t)&ret0 },
        { "glTexGeniOES", (uintptr_t)&ret0 },
        { "glTexGenivOES", (uintptr_t)&ret0 },
        { "glTexGenxOES", (uintptr_t)&ret0 },
        { "glTexGenxvOES", (uintptr_t)&ret0 },
        { "glTexImage2D", (uintptr_t)&glTexImage2D_soloader },
        { "glTexParameterf", (uintptr_t)&glTexParameterf },
        { "glTexParameterfv", (uintptr_t)&ret0 },
        { "glTexParameteri", (uintptr_t)&glTexParameteri },
        { "glTexParameteriv", (uintptr_t)&glTexParameteriv },
        { "glTexParameterx", (uintptr_t)&glTexParameterx },
        { "glTexParameterxv", (uintptr_t)&ret0 },
        { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D_soloader },
        { "glTranslatef", (uintptr_t)&glTranslatef },
        { "glTranslatex", (uintptr_t)&glTranslatex },
        { "glUniform1f", (uintptr_t)&glUniform1f_soloader },
        { "glUniform1fv", (uintptr_t)&glUniform1fv_soloader },
        { "glUniform1i", (uintptr_t)&glUniform1i_soloader },
        { "glUniform1iv", (uintptr_t)&glUniform1iv },
        { "glUniform2i", (uintptr_t)&glUniform2i },
        { "glUniform2f", (uintptr_t)&glUniform2f },
        { "glUniform2fv", (uintptr_t)&glUniform2fv_soloader },
        { "glUniform2iv", (uintptr_t)&glUniform2iv },
        { "glUniform3i", (uintptr_t)&glUniform3i },
        { "glUniform3f", (uintptr_t)&glUniform3f },
        { "glUniform3fv", (uintptr_t)&glUniform3fv },
        { "glUniform3iv", (uintptr_t)&glUniform3iv },
        { "glUniform4i", (uintptr_t)&glUniform4i },
        { "glUniform4f", (uintptr_t)&glUniform4f },
        { "glUniform4fv", (uintptr_t)&glUniform4fv_soloader },
        { "glUniform4iv", (uintptr_t)&glUniform4iv },
        { "glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fv },
        { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv },
        { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv_soloader },
        { "glUnmapBuffer", (uintptr_t)&glUnmapBuffer },
        { "glUnmapBufferOES", (uintptr_t)&glUnmapBuffer },
        { "glUseProgram", (uintptr_t)&glUseProgram_soloader },
        { "glValidateProgram", (uintptr_t)&ret0 },
        { "glVertexAttrib1f", (uintptr_t)&glVertexAttrib1f },
        { "glVertexAttrib2f", (uintptr_t)&glVertexAttrib2f },
        { "glVertexAttrib3f", (uintptr_t)&glVertexAttrib3f },
        { "glVertexAttrib4f", (uintptr_t)&glVertexAttrib4f },
        { "glVertexAttrib4fv", (uintptr_t)&glVertexAttrib4fv },
        { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
        { "glVertexPointer", (uintptr_t)&glVertexPointer },
        { "glViewport", (uintptr_t)&glViewport },
        { "glWeightPointerOES", (uintptr_t)&ret0 },


        // OpenSLES
        { "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
        { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE },
        { "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE },
        { "SL_IID_METADATAEXTRACTION", (uintptr_t)&SL_IID_METADATAEXTRACTION },
        { "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
        { "SL_IID_PREFETCHSTATUS", (uintptr_t)&SL_IID_PREFETCHSTATUS },
        { "SL_IID_SEEK", (uintptr_t)&SL_IID_SEEK },
        { "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME },
        { "slCreateEngine", (uintptr_t)&slCreateEngine },


        // Pthread
        { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
        { "pthread_attr_init", (uintptr_t) &pthread_attr_init_soloader },
        { "pthread_attr_setdetachstate", (uintptr_t) &pthread_attr_setdetachstate_soloader },
        { "pthread_attr_setstacksize", (uintptr_t) &pthread_attr_setstacksize_soloader },
        { "pthread_attr_setschedparam", (uintptr_t) &ret0 },

        { "pthread_cond_broadcast", (uintptr_t) &pthread_cond_broadcast_soloader },
        { "pthread_cond_destroy", (uintptr_t) &pthread_cond_destroy_soloader },
        { "pthread_cond_init", (uintptr_t) &pthread_cond_init_soloader },
        { "pthread_cond_signal", (uintptr_t) &pthread_cond_signal_soloader },
        { "pthread_cond_timedwait", (uintptr_t) &pthread_cond_timedwait_soloader },
        { "pthread_cond_wait", (uintptr_t) &pthread_cond_wait_soloader },

        { "pthread_create", (uintptr_t) &pthread_create_soloader },
        { "pthread_detach", (uintptr_t) &pthread_detach_soloader },
        { "pthread_equal", (uintptr_t) &pthread_equal_soloader },
        { "pthread_exit", (uintptr_t)&pthread_exit },
        { "pthread_getschedparam", (uintptr_t) &pthread_getschedparam_soloader },
        { "pthread_getspecific", (uintptr_t)&pthread_getspecific_soloader },
        { "pthread_join", (uintptr_t) &pthread_join_soloader },
        { "pthread_key_create", (uintptr_t)&pthread_key_create_soloader },
        { "pthread_key_delete", (uintptr_t)&pthread_key_delete_soloader },
        { "pthread_kill", (uintptr_t)&pthread_kill_soloader },

        { "pthread_mutex_destroy", (uintptr_t) &pthread_mutex_destroy_soloader },
        { "pthread_mutex_init", (uintptr_t) &pthread_mutex_init_soloader },
        { "pthread_mutex_lock", (uintptr_t) &pthread_mutex_lock_soloader },
        { "pthread_mutex_trylock", (uintptr_t) &pthread_mutex_trylock_soloader },
        { "pthread_mutex_unlock", (uintptr_t) &pthread_mutex_unlock_soloader },
        { "pthread_mutexattr_destroy", (uintptr_t) &pthread_mutexattr_destroy_soloader },
        { "pthread_mutexattr_init", (uintptr_t) &pthread_mutexattr_init_soloader },
        { "pthread_mutexattr_settype", (uintptr_t) &pthread_mutexattr_settype_soloader },
        { "pthread_mutexattr_setpshared", (uintptr_t) &ret0 },
        { "pthread_once", (uintptr_t)&pthread_once_soloader },

        { "pthread_self", (uintptr_t) &pthread_self_soloader },
        { "pthread_setname_np", (uintptr_t) &pthread_setname_np_soloader },
        { "pthread_setschedparam", (uintptr_t) &pthread_setschedparam_soloader },
        { "pthread_setspecific", (uintptr_t)&pthread_setspecific_soloader },
        { "pthread_sigmask", (uintptr_t)&ret0 },

        { "sem_destroy", (uintptr_t) &sem_destroy_soloader },
        { "sem_getvalue", (uintptr_t) &sem_getvalue_soloader },
        { "sem_init", (uintptr_t) &sem_init_soloader },
        { "sem_post", (uintptr_t) &sem_post_soloader },
        { "sem_timedwait", (uintptr_t) &sem_timedwait_soloader },
        { "sem_trywait", (uintptr_t) &sem_trywait_soloader },
        { "sem_wait", (uintptr_t) &sem_wait_soloader },

        { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max },
        { "sched_get_priority_min", (uintptr_t)&sched_get_priority_min },
        { "sched_yield", (uintptr_t)&sched_yield_soloader },


        // wchar, wctype
        { "btowc", (uintptr_t)&btowc },
        { "iswalpha", (uintptr_t)&iswalpha },
        { "iswcntrl", (uintptr_t)&iswcntrl },
        { "iswctype", (uintptr_t)&iswctype },
        { "iswdigit", (uintptr_t)&iswdigit },
        { "iswdigit", (uintptr_t)&iswdigit },
        { "iswlower", (uintptr_t)&iswlower },
        { "iswprint", (uintptr_t)&iswprint },
        { "iswpunct", (uintptr_t)&iswpunct },
        { "iswspace", (uintptr_t)&iswspace },
        { "iswupper", (uintptr_t)&iswupper },
        { "iswxdigit", (uintptr_t)&iswxdigit },
        { "mbrlen", (uintptr_t)&mbrlen },
        { "mbrtowc", (uintptr_t)&mbrtowc },
        { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs },
        { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
        { "mbstowcs", (uintptr_t)&mbstowcs },
        { "mbtowc", (uintptr_t)&mbtowc },
        { "towlower", (uintptr_t)&towlower },
        { "towupper", (uintptr_t)&towupper },
        { "wcrtomb", (uintptr_t)&wcrtomb },
        { "wcscasecmp", (uintptr_t)&wcscasecmp },
        { "wcscmp", (uintptr_t)&wcscmp },
        { "wcscoll", (uintptr_t)&wcscoll },
        { "wcscpy", (uintptr_t)&wcscpy },
        { "wcsftime", (uintptr_t)&wcsftime },
        { "wcslcat", (uintptr_t)&wcslcat },
        { "wcslcpy", (uintptr_t)&wcslcpy },
        { "wcslen", (uintptr_t)&wcslen },
        { "wcsncasecmp", (uintptr_t)&wcsncasecmp },
        { "wcsncmp", (uintptr_t)&wcsncmp },
        { "wcsncpy", (uintptr_t)&wcsncpy },
        { "wcsnlen", (uintptr_t)&wcsnlen },
        { "wcsnrtombs", (uintptr_t)&wcsnrtombs },
        { "wcsstr", (uintptr_t)&wcsstr },
        { "wcstod", (uintptr_t)&wcstod },
        { "wcstof", (uintptr_t)&wcstof },
        { "wcstol", (uintptr_t)&wcstol },
        { "wcstoll", (uintptr_t)&wcstoll },
        { "wcstombs", (uintptr_t)&wcstombs },
        { "wcstoul", (uintptr_t)&wcstoul },
        { "wcstoull", (uintptr_t)&wcstoull },
        { "wcsxfrm", (uintptr_t)&wcsxfrm },
        { "wctob", (uintptr_t)&wctob },
        { "wctype", (uintptr_t)&wctype },
        { "wmemchr", (uintptr_t)&wmemchr },
        { "wmemcmp", (uintptr_t)&wmemcmp },
        { "wmemcpy", (uintptr_t)&wmemcpy },
        { "wmemmove", (uintptr_t)&wmemmove },
        { "wmemset", (uintptr_t)&wmemset },


        // libdl
        { "dladdr", (uintptr_t)&dladdr_soloader },
        { "dlclose", (uintptr_t)&ret0 },
        { "dlerror", (uintptr_t)&ret0 },
        { "dlopen", (uintptr_t)&ret1 },
        { "dlsym", (uintptr_t)&dlsym_soloader },


        // Errno
        { "__errno", (uintptr_t)&__errno_soloader },
        { "strerror", (uintptr_t)&strerror_soloader },
        { "strerror_r", (uintptr_t)&strerror_r_soloader },
        { "perror", (uintptr_t)&perror }, // TODO: errno translation


        // Strings
        { "memchr", (uintptr_t)&memchr },
        { "memrchr", (uintptr_t)&memrchr },
        { "strcasecmp", (uintptr_t)&strcasecmp },
        { "strcat", (uintptr_t)&strcat },
        { "strchr", (uintptr_t)&strchr },
        { "strcmp", (uintptr_t)&strcmp },
        { "strcoll", (uintptr_t)&strcoll },
        { "strcpy", (uintptr_t)&strcpy },
        { "strcspn", (uintptr_t)&strcspn },
        { "strdup", (uintptr_t)&strdup_soloader },
        { "strlcat", (uintptr_t)&strlcat },
        { "strlcpy", (uintptr_t)&strlcpy },
        { "strlen", (uintptr_t)&strlen },
        { "strncasecmp", (uintptr_t)&strncasecmp },
        { "strncat", (uintptr_t)&strncat },
        { "strncmp", (uintptr_t)&strncmp },
        { "strndup", (uintptr_t)&strndup_soloader },
        { "strncpy", (uintptr_t)&strncpy },
        { "strnlen", (uintptr_t)&strnlen },
        { "strpbrk", (uintptr_t)&strpbrk },
        { "strrchr", (uintptr_t)&strrchr },
        { "strspn", (uintptr_t)&strspn },
        { "strstr", (uintptr_t)&strstr },
        { "strtok", (uintptr_t)&strtok },
        { "strtok_r", (uintptr_t)&strtok_r },
        { "strxfrm", (uintptr_t)&strxfrm },


        // Syscalls
        { "fork", (uintptr_t)&fork },
        { "getpagesize", (uintptr_t)&getpagesize },
        { "getpid", (uintptr_t)&getpid },
        { "sbrk", (uintptr_t)&sbrk },
        { "syscall", (uintptr_t)&syscall },
        { "sysconf", (uintptr_t)&sysconf_soloader },
        { "system", (uintptr_t)&system },
        { "vfork", (uintptr_t)&vfork_soloader },
        { "waitpid", (uintptr_t)&ret0 },


        // Time
        { "clock", (uintptr_t)&clock_soloader },
        { "clock_getres", (uintptr_t)&clock_getres_soloader },
        { "clock_gettime", (uintptr_t)&clock_gettime_soloader },
        { "difftime", (uintptr_t)&difftime },
        { "gettimeofday", (uintptr_t)&gettimeofday },
        { "gmtime", (uintptr_t)&gmtime },
        { "gmtime64", (uintptr_t)&gmtime64 },
        { "gmtime_r", (uintptr_t)&gmtime_r },
        { "localtime", (uintptr_t)&localtime },
        { "localtime64", (uintptr_t)&localtime64 },
        { "localtime_r", (uintptr_t)&localtime_r },
        { "mktime", (uintptr_t)&mktime },
        { "mktime64", (uintptr_t)&mktime64 },
        { "nanosleep", (uintptr_t)&nanosleep },
        { "strftime", (uintptr_t)&strftime },
        { "time", (uintptr_t)&time },
        { "tzset", (uintptr_t)&tzset },


        // Temp
        { "mkstemp", (uintptr_t)&mkstemp },
        { "mktemp", (uintptr_t)&mktemp },
        { "tmpfile", (uintptr_t)&tmpfile },
        { "tmpnam", (uintptr_t)&tmpnam },


        // stdlib
        { "_exit", (uintptr_t)&exit_soloader },
        { "abort", (uintptr_t)&abort_soloader },
        { "abort_message", (uintptr_t)&abort_message_soloader },
        { "arc4random", (uintptr_t)&arc4random_soloader },
        { "atof", (uintptr_t)&atof },
        { "atoi", (uintptr_t)&atoi },
        { "atol", (uintptr_t)&atol },
        { "atoll", (uintptr_t)&atoll },
        { "bsearch", (uintptr_t)&bsearch },
        { "exit", (uintptr_t)&exit_soloader },
        { "lrand48", (uintptr_t)&lrand48 },
        { "prctl", (uintptr_t)&ret0 },
        { "sleep", (uintptr_t)&sleep },
        { "srand48", (uintptr_t)&srand48 },
        { "strtod", (uintptr_t)&strtod },
        { "strtof", (uintptr_t)&strtof },
        { "strtoimax", (uintptr_t)&strtoimax },
        { "strtol", (uintptr_t)&strtol },
        { "strtold", (uintptr_t)&strtold },
        { "strtoll", (uintptr_t)&strtoll },
        { "strtoul", (uintptr_t)&strtoul },
        { "strtoull", (uintptr_t)&strtoull },
        { "strtoumax", (uintptr_t)&strtoumax },
        { "usleep", (uintptr_t)&usleep },

        #ifdef USE_SCELIBC_IO
            { "qsort", (uintptr_t)&sceLibcBridge_qsort },
            { "rand", (uintptr_t)&sceLibcBridge_rand },
            { "srand", (uintptr_t)&sceLibcBridge_srand },
        #else
            { "qsort", (uintptr_t)&qsort },
            { "rand", (uintptr_t)&rand },
            { "srand", (uintptr_t)&srand },
        #endif


        // Env
        { "getenv", (uintptr_t)&getenv_soloader },
        { "setenv", (uintptr_t)&setenv_soloader },


        // Jmp
        { "setjmp", (uintptr_t)&setjmp }, // TODO: May have different struct size?
        { "longjmp", (uintptr_t)&longjmp }, // TODO: May have different struct size?


        // Signals
        { "bsd_signal", (uintptr_t)&signal },
        { "raise", (uintptr_t)&raise },
        { "sigaction", (uintptr_t)&sigaction },


        // Locale
        { "freelocale", (uintptr_t)&freelocale },
        { "localeconv", (uintptr_t)&localeconv },
        { "newlocale", (uintptr_t)&newlocale },
        { "setlocale", (uintptr_t)&setlocale },
        { "uselocale", (uintptr_t)&uselocale },


        // zlib
        { "adler32", (uintptr_t)&adler32 },
        { "compress", (uintptr_t)&compress },
        { "compressBound", (uintptr_t)&compressBound },
        { "crc32", (uintptr_t)&crc32 },
        { "deflate", (uintptr_t)&deflate },
        { "deflateEnd", (uintptr_t)&deflateEnd },
        { "deflateInit2_", (uintptr_t)&deflateInit2_ },
        { "deflateInit_", (uintptr_t)&deflateInit_ },
        { "deflateReset", (uintptr_t)&deflateReset },
        { "gzclose", (uintptr_t)&gzclose },
        { "gzgets", (uintptr_t)&gzgets },
        { "gzopen", (uintptr_t)&gzopen },
        { "inflate", (uintptr_t)&inflate },
        { "inflateEnd", (uintptr_t)&inflateEnd },
        { "inflateInit2_", (uintptr_t)&inflateInit2_ },
        { "inflateInit_", (uintptr_t)&inflateInit_ },
        { "inflateReset", (uintptr_t)&inflateReset },
        { "inflateReset2", (uintptr_t)&inflateReset2 },
        { "uncompress", (uintptr_t)&uncompress },
};

void *dlsym_soloader(void * handle, const char * symbol) {
    (void)handle;

    for (int i = 0; i < sizeof(default_dynlib) / sizeof(default_dynlib[0]); i++) {
        if (strcmp(symbol, default_dynlib[i].symbol) == 0) {
            return (void *)default_dynlib[i].func;
        }
    }

    uintptr_t module_symbol = so_symbol(&so_mod, symbol);
    if (module_symbol)
        return (void *)module_symbol;

    l_error("dlsym: Unknown symbol \"%s\".", symbol);
    return NULL;
}

static uintptr_t unwind_find_exidx_soloader(uintptr_t pc, int *pcount) {
    (void)pc;
    so_module *mod = g_unwind_mod;
    if (!mod || !mod->exidx_base || mod->num_exidx <= 0) {
        if (pcount)
            *pcount = 0;
        return 0;
    }

    if (pcount)
        *pcount = mod->num_exidx;
    return mod->exidx_base;
}

void resolve_imports(so_module* mod) {
    __sF_fake[0] = *stdin;
    __sF_fake[1] = *stdout;
    __sF_fake[2] = *stderr;

    g_unwind_mod = mod;
    if (mod->exidx_base && mod->num_exidx > 0) {
        l_info("ARM exidx registered: base=0x%08x count=%d",
               (unsigned)mod->exidx_base, mod->num_exidx);
    } else {
        l_warn("ARM exidx section not found for loaded module.");
    }

    so_resolve(mod, default_dynlib, sizeof(default_dynlib), 0);
}
