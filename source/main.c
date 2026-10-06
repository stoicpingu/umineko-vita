#include "utils/init.h"
#include "utils/dialog.h"
#include "utils/logger.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <psp2/kernel/threadmgr.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include <SDL2/SDL.h>

#include "reimpl/controls.h"
#include "reimpl/mem.h"

/*
 * Sized to cover the engine's peak usage (~184MB) while leaving enough
 * USER_RW free for vitaGL's own texture pool. A larger arena leaves vitaGL
 * no pool, so GL textures spill into this heap and fragment it against the
 * engine's transient surfaces.
 */
int _newlib_heap_size_user = 280 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

#define SDL_THREAD_STACK_SIZE (4 * 1024 * 1024)

typedef int (*SDLMainFn)(int argc, char **argv);
typedef int (*JNIOnLoadFn)(JavaVM *jvm, void *reserved);

static SDLMainFn s_sdl_main;
static JNIOnLoadFn s_jni_on_load;
static volatile bool s_game_running;

/*
 * hwconvert defaults to off: the on path (planar LUMINANCE textures +
 * colourConversion.frag) does not produce visible frames on the Vita,
 * while the off path converts on the CPU (sws, NEON) and uploads through
 * the regular sprite path. Both the native SceAvPlayer branch and the
 * ffmpeg pipeline honor this flag. Override at runtime via
 * ux0:data/umineko/pivas_flags.txt ("hwconvert=on"/"hwconvert=off").
 */
static char s_hwconvert_value[4] = "off";

/*
 * The render scale MUST match the factor the on-card image assets were
 * pre-scaled by (extras/scripts/build_vita_assets.py) — the engine derives
 * every image's logical footprint as pixels / render_scale. The asset tree
 * is therefore the single source of truth: the build script stamps the
 * factor into <game root>/render_scale.txt, which is read at boot. Without
 * the marker, a 2/3 tree is assumed.
 */
static char s_render_scale_value[16] = "0.6666667";

static char *umineko_argv[] = {
    "onscripter-ru",
    "--root", "./",
	"--save", "save/",
	"--prefer-renderer", "GLES2",
	"--audioformat", "s16",
	"--audiobuffer", "4",
	"--ramlimit", "64",
	"--chunklimit", "8388608",
	"--texlimit", "2048",
	"--breakup", "old",
	"--render-self", "no",
	"--no-glclear",
	"--force-png-alpha",
	"--render-scale", s_render_scale_value,
	"--reduce-motion",
	"--fullscreen",
    "--scale",
    "--hwdecoder", "off",
    "--hwconvert", s_hwconvert_value,
    NULL,
};

static void load_runtime_flags(void) {
    FILE *f = fopen(DATA_PATH "pivas_flags.txt", "r");
    if (!f)
        return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "hwconvert=off", 13) == 0) {
            strcpy(s_hwconvert_value, "off");
            l_info("[flags] hwconvert=off (pivas_flags.txt)");
        } else if (strncmp(line, "hwconvert=on", 12) == 0) {
            strcpy(s_hwconvert_value, "on");
            l_info("[flags] hwconvert=on (pivas_flags.txt)");
        }
    }
    fclose(f);
}

static void load_render_scale(void) {
    FILE *f = fopen(DATA_PATH "render_scale.txt", "r");
    if (!f) {
        l_info("[flags] render_scale.txt absent; render-scale=%s",
               s_render_scale_value);
        return;
    }

    char line[32] = {0};
    if (fgets(line, sizeof(line), f)) {
        char *end = NULL;
        double scale = strtod(line, &end);
        if (end != line && scale >= 0.25 && scale <= 1.0) {
            snprintf(s_render_scale_value, sizeof(s_render_scale_value),
                     "%.7f", scale);
            l_info("[flags] render-scale=%s (render_scale.txt)",
                   s_render_scale_value);
        } else {
            l_error("[flags] render_scale.txt unparsable ('%s'); keeping %s",
                    line, s_render_scale_value);
        }
    }
    fclose(f);
}

static void *require_symbol(const char *name) {
    void *symbol = (void *)so_symbol(&so_mod, name);
    if (!symbol)
        fatal_error("Required Umineko symbol missing: %s", name);
    return symbol;
}

static void resolve_entry_symbols(void) {
    s_jni_on_load = require_symbol("JNI_OnLoad");
    s_sdl_main = require_symbol("SDL_main");
}

static void *umineko_thread_main(void *arg) {
    (void)arg;
    int argc = (int)(sizeof(umineko_argv) / sizeof(umineko_argv[0])) - 1;

    s_jni_on_load(&jvm, NULL);
    l_info("JNI_OnLoad passed.");

    int status = s_sdl_main(argc, umineko_argv);
    l_info("SDL_main exited with status %i", status);
    s_game_running = false;
    return NULL;
}

/*
 * SDL's default log output on the Vita appends every SDL_Log message to
 * ux0:/data/SDL_Log.txt (fopen/fprintf/fclose per message). Release builds
 * discard SDL's messages instead.
 */
static void pivas_sdl_log_discard(void *userdata, int category,
                                  SDL_LogPriority priority,
                                  const char *message) {
    (void)userdata;
    (void)category;
    (void)priority;
    (void)message;
}

int main() {
    SDL_LogSetOutputFunction(pivas_sdl_log_discard, NULL);

    /*
     * Route the loader's Vita SDL2/SDL2_mixer allocations through the
     * guarded, quarantined allocator so its heap canaries cover SDL's
     * chunks too. Must run before any SDL alloc.
     */
    SDL_SetMemoryFunctions(malloc_soloader, calloc_soloader,
                           realloc_soloader, free_soloader);

    l_info("[loader] build vidfix-2 (" __DATE__ " " __TIME__ ")");

    soloader_init_all();
    resolve_entry_symbols();
    load_runtime_flags();
    load_render_scale();
    mkdir(DATA_PATH "save", 0777);
    if (chdir(DATA_PATH) == 0)
        l_info("Working directory set to %s", DATA_PATH);
    else
        l_error("Failed to set working directory to %s", DATA_PATH);

    pthread_t game_thread;
    pthread_attr_t attr;
    int pthread_status = pthread_attr_init(&attr);
    if (pthread_status != 0)
        fatal_error("pthread_attr_init failed: %i", pthread_status);

    pthread_status = pthread_attr_setstacksize(&attr, SDL_THREAD_STACK_SIZE);
    if (pthread_status != 0)
        fatal_error("pthread_attr_setstacksize failed: %i", pthread_status);

    l_info("Starting SDL_main thread with %u byte stack.",
           SDL_THREAD_STACK_SIZE);

    s_game_running = true;
    pthread_status = pthread_create(&game_thread, &attr, umineko_thread_main,
                                    NULL);
    pthread_attr_destroy(&attr);
    if (pthread_status != 0) {
        s_game_running = false;
        fatal_error("pthread_create failed: %i", pthread_status);
    }

    while (s_game_running) {
        controls_poll();
        sceKernelDelayThread(16666);
    }

    pthread_join(game_thread, NULL);

    sceKernelExitDeleteThread(0);
    return 0;
}

void controls_handler_key(int32_t scancode, ControlsAction action) {
    /*
     * The engine drains the native queue from its async event thread
     * (Engine/Core/Event.cpp fetchEventsToQueue); SDL_PushEvent is
     * thread-safe. Drop input until SDL is up. The engine matches
     * scancodes only (incl. the ONS_* virtual codes 513-515), so sym
     * stays SDLK_UNKNOWN.
     */
    if (!SDL_WasInit(SDL_INIT_VIDEO))
        return;

    SDL_Event ev;
    SDL_zero(ev);
    ev.type = (action == CONTROLS_ACTION_DOWN) ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.timestamp = SDL_GetTicks();
    ev.key.state = (action == CONTROLS_ACTION_DOWN) ? SDL_PRESSED
                                                    : SDL_RELEASED;
    ev.key.keysym.scancode = (SDL_Scancode)scancode;
    ev.key.keysym.sym = SDLK_UNKNOWN;
    SDL_PushEvent(&ev);
}
