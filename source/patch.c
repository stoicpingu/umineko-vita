/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <stdint.h>

#include <so_util/so_util.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include <falso_jni/FalsoJNI.h>

#include <psp2/display.h>
#include <vitaGL.h>

#include "reimpl/io.h"
#include "utils/dialog.h"
#include "utils/logger.h"
#include "utils/utils.h"

extern so_module so_mod;

#define SDL_ANDROID_EXTERNAL_STORAGE_READ  0x01
#define SDL_ANDROID_EXTERNAL_STORAGE_WRITE 0x02

static int s_sdl_hooks_applied;
static char s_current_path[] = "./";

static void hook_sdl_symbol(const char *name, uintptr_t target) {
    uintptr_t symbol = so_symbol(&so_mod, name);
    if (!symbol)
        return;

    hook_addr(symbol, target);
    s_sdl_hooks_applied++;
}

#define HOOK_SDL(symbol) hook_sdl_symbol(#symbol, (uintptr_t)&symbol)
#define HOOK_SDL_AS(name, target) hook_sdl_symbol(name, (uintptr_t)&target)

/*
 * While a system message dialog is open (utils/dialog.c), swap with the
 * common-dialog flag so vitaGL composites it over the engine's frame, and
 * pace the caller to the display. sdl-gpu asks for swap interval -1, which
 * SDL2's vitaGL driver forwards verbatim (eglSwapInterval -> vsync_interval
 * = -1), so vitaGL's display-queue callback never actually waits for
 * vblank; the engine normally paces itself with its own frame timer. Its
 * dialog loop has no such timer, so unthrottled it flips several times per
 * refresh and renders into buffers that are still on screen, making the
 * popup flicker.
 */
static void pivas_gl_swap_window(SDL_Window *window) {
    if (pivas_msg_dialog_active()) {
        vglSwapBuffers(GL_TRUE);
        sceDisplayWaitVblankStart();
    } else {
        SDL_GL_SwapWindow(window);
    }
}

void SDL_AndroidBackButton(void) {
    l_info("SDL_AndroidBackButton ignored.");
}

void *SDL_AndroidGetActivity(void) {
    return NULL;
}

char *SDL_AndroidGetExternalStoragePath(void) {
    return s_current_path;
}

char *SDL_AndroidGetInternalStoragePath(void) {
    return s_current_path;
}

int SDL_AndroidGetExternalStorageState(void) {
    return SDL_ANDROID_EXTERNAL_STORAGE_READ | SDL_ANDROID_EXTERNAL_STORAGE_WRITE;
}

void *SDL_AndroidGetJNIEnv(void) {
    return &jni;
}

SDL_bool SDL_IsAndroidTV(void) {
    return SDL_FALSE;
}

SDL_bool SDL_IsChromebook(void) {
    return SDL_FALSE;
}

SDL_bool SDL_IsDeXMode(void) {
    return SDL_FALSE;
}

static char *SDL_GetBasePath_vita(void) {
    return SDL_strdup("./");
}

static char *SDL_GetPrefPath_vita(const char *org, const char *app) {
    (void)org;
    (void)app;
    return SDL_strdup("save/");
}

static SDL_RWops *SDL_RWFromFile_vita(const char *file, const char *mode) {
    return SDL_RWFromFile(file, mode);
}

/*
 * SDL_RWFromFP is the one SDL API whose signature carries a FILE* across the
 * libc boundary. The engine's FILE comes from fopen_soloader (SceLibc when
 * USE_SCELIBC_IO), but the native SDL2 linked into this loader wraps it in
 * newlib-stdio callbacks; newlib fclose then reads the SceLibc FILE through
 * the newlib __sFILE layout and jumps through garbage. These callbacks keep
 * every FILE operation in the same libc world the engine's fopen used. Field
 * layout mirrors native stdio rwops: hidden.unknown.data1 = autoclose (+24),
 * data2 = fp (+28).
 */
static Sint64 rwfp_vita_size(SDL_RWops *context) {
    FILE *fp = (FILE *)context->hidden.unknown.data2;
    long pos = ftell_soloader(fp);
    if (pos < 0)
        return -1;
    if (fseek_soloader(fp, 0, SEEK_END) != 0)
        return -1;
    Sint64 size = ftell_soloader(fp);
    fseek_soloader(fp, pos, SEEK_SET);
    return size;
}

static Sint64 rwfp_vita_seek(SDL_RWops *context, Sint64 offset, int whence) {
    FILE *fp = (FILE *)context->hidden.unknown.data2;
    int stdiowhence;

    switch (whence) {
        case RW_SEEK_SET: stdiowhence = SEEK_SET; break;
        case RW_SEEK_CUR: stdiowhence = SEEK_CUR; break;
        case RW_SEEK_END: stdiowhence = SEEK_END; break;
        default:
            return SDL_SetError("rwfp_vita_seek: unknown whence %d", whence);
    }

    if (fseek_soloader(fp, (long)offset, stdiowhence) == 0)
        return ftell_soloader(fp);
    return SDL_SetError("rwfp_vita_seek: fseek failed");
}

static size_t rwfp_vita_read(SDL_RWops *context, void *ptr, size_t size,
                             size_t maxnum) {
    return fread_soloader(ptr, size, maxnum,
                          (FILE *)context->hidden.unknown.data2);
}

static size_t rwfp_vita_write(SDL_RWops *context, const void *ptr, size_t size,
                              size_t num) {
    return fwrite_soloader(ptr, size, num,
                           (FILE *)context->hidden.unknown.data2);
}

static int rwfp_vita_close(SDL_RWops *context) {
    int status = 0;

    if (context) {
        FILE *fp = (FILE *)context->hidden.unknown.data2;
        if (context->hidden.unknown.data1 && fp &&
            fclose_soloader(fp) != 0) {
            status = SDL_SetError("rwfp_vita_close: fclose failed");
        }
        // Callers (engine SDL_gpu included) invoke close directly and never
        // free the rwops themselves; native stdio_close frees it too.
        SDL_FreeRW(context);
    }
    return status;
}

static SDL_RWops *SDL_RWFromFP_vita(FILE *fp, SDL_bool autoclose) {
    if (!fp) {
        SDL_SetError("SDL_RWFromFP_vita: NULL FILE*");
        return NULL;
    }

    SDL_RWops *rwops = SDL_AllocRW();
    if (!rwops)
        return NULL;

    rwops->size = rwfp_vita_size;
    rwops->seek = rwfp_vita_seek;
    rwops->read = rwfp_vita_read;
    rwops->write = rwfp_vita_write;
    rwops->close = rwfp_vita_close;
    rwops->hidden.unknown.data1 = (void *)(uintptr_t)autoclose;
    rwops->hidden.unknown.data2 = fp;
    rwops->type = SDL_RWOPS_STDFILE;
    return rwops;
}

static SDL_Window *SDL_CreateWindow_vita(const char *title, int x, int y,
                                         int w, int h, Uint32 flags) {
    if (w <= 0)
        w = 960;
    if (h <= 0)
        h = 544;
    if (!title)
        title = "Umineko";

    return SDL_CreateWindow(title, x, y, w, h, flags | SDL_WINDOW_SHOWN);
}

static int SDL_Init_vita(Uint32 flags) {
    SDL_SetMainReady();
    SDL_setenv("VITA_DISABLE_TOUCH_BACK", "1", 1);
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");

    int result = SDL_Init(flags);
    if (result < 0)
        l_error("Native SDL_Init failed: %s", SDL_GetError());
    else
        l_info("Native SDL_Init passed with flags 0x%x.", flags);

    /*
     * Pad input is synthesized as keyboard events by reimpl/controls.c.
     * Silence native joystick events: the engine routes SDL_JOYBUTTON*
     * through a GUID table (Engine/Components/Joystick.cpp) and falls
     * back to a DualShock 3 layout for the unknown Vita GUID, firing
     * junk (Start=skip toggle, Select=auto mode, dpad-right=ctrl-skip,
     * Cross/Circle=nothing). Event-state flags persist; the engine's
     * later SDL_InitSubSystem(JOYSTICK) does not re-enable them.
     */
    SDL_JoystickEventState(SDL_IGNORE);

    return result;
}

void so_patch(void) {
    HOOK_SDL_AS("SDL_AndroidBackButton", SDL_AndroidBackButton);
    HOOK_SDL_AS("SDL_AndroidGetActivity", SDL_AndroidGetActivity);
    HOOK_SDL_AS("SDL_AndroidGetExternalStoragePath", SDL_AndroidGetExternalStoragePath);
    HOOK_SDL_AS("SDL_AndroidGetExternalStorageState", SDL_AndroidGetExternalStorageState);
    HOOK_SDL_AS("SDL_AndroidGetInternalStoragePath", SDL_AndroidGetInternalStoragePath);
    HOOK_SDL_AS("SDL_AndroidGetJNIEnv", SDL_AndroidGetJNIEnv);
    HOOK_SDL_AS("SDL_IsAndroidTV", SDL_IsAndroidTV);
    HOOK_SDL_AS("SDL_IsChromebook", SDL_IsChromebook);
    HOOK_SDL_AS("SDL_IsDeXMode", SDL_IsDeXMode);

    HOOK_SDL_AS("SDL_CreateWindow", SDL_CreateWindow_vita);
    HOOK_SDL_AS("SDL_GetBasePath", SDL_GetBasePath_vita);
    HOOK_SDL_AS("SDL_GetPrefPath", SDL_GetPrefPath_vita);
    HOOK_SDL_AS("SDL_Init", SDL_Init_vita);
    HOOK_SDL_AS("SDL_RWFromFile", SDL_RWFromFile_vita);

    HOOK_SDL_AS("SDL_Vulkan_CreateSurface", ret0);
    HOOK_SDL_AS("SDL_Vulkan_GetDrawableSize", ret0);
    HOOK_SDL_AS("SDL_Vulkan_GetInstanceExtensions", ret0);
    HOOK_SDL_AS("SDL_Vulkan_GetVkGetInstanceProcAddr", ret0);
    HOOK_SDL_AS("SDL_Vulkan_LoadLibrary", retminus1);
    HOOK_SDL_AS("SDL_Vulkan_UnloadLibrary", ret0);

    HOOK_SDL(SDL_AddEventWatch);
    HOOK_SDL(SDL_AddHintCallback);
    HOOK_SDL(SDL_AddTimer);
    HOOK_SDL(SDL_AllocFormat);
    HOOK_SDL(SDL_AllocPalette);
    HOOK_SDL(SDL_AllocRW);
    HOOK_SDL(SDL_AtomicAdd);
    HOOK_SDL(SDL_AtomicCAS);
    HOOK_SDL(SDL_AtomicCASPtr);
    HOOK_SDL(SDL_AtomicGet);
    HOOK_SDL(SDL_AtomicGetPtr);
    HOOK_SDL(SDL_AtomicLock);
    HOOK_SDL(SDL_AtomicSet);
    HOOK_SDL(SDL_AtomicSetPtr);
    HOOK_SDL(SDL_AtomicTryLock);
    HOOK_SDL(SDL_AtomicUnlock);
    HOOK_SDL(SDL_AudioInit);
    HOOK_SDL(SDL_AudioQuit);
    HOOK_SDL(SDL_AudioStreamAvailable);
    HOOK_SDL(SDL_AudioStreamClear);
    HOOK_SDL(SDL_AudioStreamFlush);
    HOOK_SDL(SDL_AudioStreamGet);
    HOOK_SDL(SDL_AudioStreamPut);
    HOOK_SDL(SDL_BuildAudioCVT);
    HOOK_SDL(SDL_CalculateGammaRamp);
    HOOK_SDL(SDL_CaptureMouse);
    HOOK_SDL(SDL_ClearError);
    HOOK_SDL(SDL_ClearHints);
    HOOK_SDL(SDL_ClearQueuedAudio);
    HOOK_SDL(SDL_CloseAudio);
    HOOK_SDL(SDL_CloseAudioDevice);
    HOOK_SDL(SDL_ComposeCustomBlendMode);
    HOOK_SDL(SDL_CondBroadcast);
    HOOK_SDL(SDL_CondSignal);
    HOOK_SDL(SDL_CondWait);
    HOOK_SDL(SDL_CondWaitTimeout);
    HOOK_SDL(SDL_ConvertAudio);
    HOOK_SDL(SDL_ConvertPixels);
    HOOK_SDL(SDL_ConvertSurface);
    HOOK_SDL(SDL_ConvertSurfaceFormat);
    HOOK_SDL(SDL_CreateColorCursor);
    HOOK_SDL(SDL_CreateCond);
    HOOK_SDL(SDL_CreateCursor);
    HOOK_SDL(SDL_CreateMutex);
    HOOK_SDL(SDL_CreateRGBSurface);
    HOOK_SDL(SDL_CreateRGBSurfaceFrom);
    HOOK_SDL(SDL_CreateRGBSurfaceWithFormat);
    HOOK_SDL(SDL_CreateRGBSurfaceWithFormatFrom);
    HOOK_SDL(SDL_CreateRenderer);
    HOOK_SDL(SDL_CreateSemaphore);
    HOOK_SDL(SDL_CreateShapedWindow);
    HOOK_SDL(SDL_CreateSoftwareRenderer);
    HOOK_SDL(SDL_CreateSystemCursor);
    HOOK_SDL(SDL_CreateTexture);
    HOOK_SDL(SDL_CreateTextureFromSurface);
    HOOK_SDL(SDL_CreateThread);
    HOOK_SDL(SDL_CreateThreadWithStackSize);
    HOOK_SDL(SDL_CreateWindowAndRenderer);
    HOOK_SDL(SDL_CreateWindowFrom);
    HOOK_SDL(SDL_DelEventWatch);
    HOOK_SDL(SDL_DelHintCallback);
    HOOK_SDL(SDL_Delay);
    HOOK_SDL(SDL_DequeueAudio);
    HOOK_SDL(SDL_DestroyCond);
    HOOK_SDL(SDL_DestroyMutex);
    HOOK_SDL(SDL_DestroyRenderer);
    HOOK_SDL(SDL_DestroySemaphore);
    HOOK_SDL(SDL_DestroyTexture);
    HOOK_SDL(SDL_DestroyWindow);
    HOOK_SDL(SDL_DetachThread);
    HOOK_SDL(SDL_DisableScreenSaver);
    HOOK_SDL(SDL_DuplicateSurface);
    HOOK_SDL(SDL_EnableScreenSaver);
    HOOK_SDL(SDL_EnclosePoints);
    HOOK_SDL(SDL_Error);
    HOOK_SDL(SDL_EventState);
    HOOK_SDL(SDL_FillRect);
    HOOK_SDL(SDL_FillRects);
    HOOK_SDL(SDL_FilterEvents);
    HOOK_SDL(SDL_FlushEvent);
    HOOK_SDL(SDL_FlushEvents);
    HOOK_SDL(SDL_FreeAudioStream);
    HOOK_SDL(SDL_FreeCursor);
    HOOK_SDL(SDL_FreeFormat);
    HOOK_SDL(SDL_FreePalette);
    HOOK_SDL(SDL_FreeRW);
    HOOK_SDL(SDL_FreeSurface);
    HOOK_SDL(SDL_FreeWAV);
    HOOK_SDL(SDL_GL_BindTexture);
    HOOK_SDL(SDL_GL_CreateContext);
    HOOK_SDL(SDL_GL_DeleteContext);
    HOOK_SDL(SDL_GL_ExtensionSupported);
    HOOK_SDL(SDL_GL_GetAttribute);
    HOOK_SDL(SDL_GL_GetCurrentContext);
    HOOK_SDL(SDL_GL_GetCurrentWindow);
    HOOK_SDL(SDL_GL_GetDrawableSize);
    HOOK_SDL(SDL_GL_GetProcAddress);
    HOOK_SDL(SDL_GL_GetSwapInterval);
    HOOK_SDL(SDL_GL_LoadLibrary);
    HOOK_SDL(SDL_GL_MakeCurrent);
    HOOK_SDL(SDL_GL_ResetAttributes);
    HOOK_SDL(SDL_GL_SetAttribute);
    HOOK_SDL(SDL_GL_SetSwapInterval);
    HOOK_SDL_AS("SDL_GL_SwapWindow", pivas_gl_swap_window);
    HOOK_SDL(SDL_GL_UnbindTexture);
    HOOK_SDL(SDL_GL_UnloadLibrary);
    HOOK_SDL(SDL_GameControllerAddMapping);
    HOOK_SDL(SDL_GameControllerAddMappingsFromRW);
    HOOK_SDL(SDL_GameControllerClose);
    HOOK_SDL(SDL_GameControllerEventState);
    HOOK_SDL(SDL_GameControllerFromInstanceID);
    HOOK_SDL(SDL_GameControllerGetAttached);
    HOOK_SDL(SDL_GameControllerGetAxis);
    HOOK_SDL(SDL_GameControllerGetAxisFromString);
    HOOK_SDL(SDL_GameControllerGetBindForAxis);
    HOOK_SDL(SDL_GameControllerGetBindForButton);
    HOOK_SDL(SDL_GameControllerGetButton);
    HOOK_SDL(SDL_GameControllerGetButtonFromString);
    HOOK_SDL(SDL_GameControllerGetJoystick);
    HOOK_SDL(SDL_GameControllerGetPlayerIndex);
    HOOK_SDL(SDL_GameControllerGetProduct);
    HOOK_SDL(SDL_GameControllerGetProductVersion);
    HOOK_SDL(SDL_GameControllerGetStringForAxis);
    HOOK_SDL(SDL_GameControllerGetStringForButton);
    HOOK_SDL(SDL_GameControllerGetVendor);
    HOOK_SDL(SDL_GameControllerMapping);
    HOOK_SDL(SDL_GameControllerMappingForDeviceIndex);
    HOOK_SDL(SDL_GameControllerMappingForGUID);
    HOOK_SDL(SDL_GameControllerMappingForIndex);
    HOOK_SDL(SDL_GameControllerName);
    HOOK_SDL(SDL_GameControllerNameForIndex);
    HOOK_SDL(SDL_GameControllerNumMappings);
    HOOK_SDL(SDL_GameControllerOpen);
    HOOK_SDL(SDL_GameControllerRumble);
    HOOK_SDL(SDL_GameControllerUpdate);
    HOOK_SDL(SDL_GetAssertionHandler);
    HOOK_SDL(SDL_GetAssertionReport);
    HOOK_SDL(SDL_GetAudioDeviceName);
    HOOK_SDL(SDL_GetAudioDeviceStatus);
    HOOK_SDL(SDL_GetAudioDriver);
    HOOK_SDL(SDL_GetAudioStatus);
    HOOK_SDL(SDL_GetCPUCacheLineSize);
    HOOK_SDL(SDL_GetCPUCount);
    HOOK_SDL(SDL_GetClipRect);
    HOOK_SDL(SDL_GetClipboardText);
    HOOK_SDL(SDL_GetClosestDisplayMode);
    HOOK_SDL(SDL_GetColorKey);
    HOOK_SDL(SDL_GetCurrentAudioDriver);
    HOOK_SDL(SDL_GetCurrentDisplayMode);
    HOOK_SDL(SDL_GetCurrentVideoDriver);
    HOOK_SDL(SDL_GetCursor);
    HOOK_SDL(SDL_GetDefaultAssertionHandler);
    HOOK_SDL(SDL_GetDefaultCursor);
    HOOK_SDL(SDL_GetDesktopDisplayMode);
    HOOK_SDL(SDL_GetDisplayBounds);
    HOOK_SDL(SDL_GetDisplayDPI);
    HOOK_SDL(SDL_GetDisplayMode);
    HOOK_SDL(SDL_GetDisplayName);
    HOOK_SDL(SDL_GetDisplayOrientation);
    HOOK_SDL(SDL_GetDisplayUsableBounds);
    HOOK_SDL(SDL_GetError);
    HOOK_SDL(SDL_GetEventFilter);
    HOOK_SDL(SDL_GetGlobalMouseState);
    HOOK_SDL(SDL_GetGrabbedWindow);
    HOOK_SDL(SDL_GetHint);
    HOOK_SDL(SDL_GetHintBoolean);
    HOOK_SDL(SDL_GetKeyFromName);
    HOOK_SDL(SDL_GetKeyFromScancode);
    HOOK_SDL(SDL_GetKeyName);
    HOOK_SDL(SDL_GetKeyboardFocus);
    HOOK_SDL(SDL_GetKeyboardState);
    HOOK_SDL(SDL_GetMemoryFunctions);
    HOOK_SDL(SDL_GetModState);
    HOOK_SDL(SDL_GetMouseFocus);
    HOOK_SDL(SDL_GetMouseState);
    HOOK_SDL(SDL_GetNumAllocations);
    HOOK_SDL(SDL_GetNumAudioDevices);
    HOOK_SDL(SDL_GetNumAudioDrivers);
    HOOK_SDL(SDL_GetNumDisplayModes);
    HOOK_SDL(SDL_GetNumRenderDrivers);
    HOOK_SDL(SDL_GetNumTouchDevices);
    HOOK_SDL(SDL_GetNumTouchFingers);
    HOOK_SDL(SDL_GetNumVideoDisplays);
    HOOK_SDL(SDL_GetNumVideoDrivers);
    HOOK_SDL(SDL_GetPerformanceCounter);
    HOOK_SDL(SDL_GetPerformanceFrequency);
    HOOK_SDL(SDL_GetPixelFormatName);
    HOOK_SDL(SDL_GetPlatform);
    HOOK_SDL(SDL_GetPowerInfo);
    HOOK_SDL(SDL_GetQueuedAudioSize);
    HOOK_SDL(SDL_GetRGB);
    HOOK_SDL(SDL_GetRGBA);
    HOOK_SDL(SDL_GetRelativeMouseMode);
    HOOK_SDL(SDL_GetRelativeMouseState);
    HOOK_SDL(SDL_GetRenderDrawBlendMode);
    HOOK_SDL(SDL_GetRenderDrawColor);
    HOOK_SDL(SDL_GetRenderDriverInfo);
    HOOK_SDL(SDL_GetRenderTarget);
    HOOK_SDL(SDL_GetRenderer);
    HOOK_SDL(SDL_GetRendererInfo);
    HOOK_SDL(SDL_GetRendererOutputSize);
    HOOK_SDL(SDL_GetRevision);
    HOOK_SDL(SDL_GetRevisionNumber);
    HOOK_SDL(SDL_GetScancodeFromKey);
    HOOK_SDL(SDL_GetScancodeFromName);
    HOOK_SDL(SDL_GetScancodeName);
    HOOK_SDL(SDL_GetShapedWindowMode);
    HOOK_SDL(SDL_GetSurfaceAlphaMod);
    HOOK_SDL(SDL_GetSurfaceBlendMode);
    HOOK_SDL(SDL_GetSurfaceColorMod);
    HOOK_SDL(SDL_GetSystemRAM);
    HOOK_SDL(SDL_GetTextureAlphaMod);
    HOOK_SDL(SDL_GetTextureBlendMode);
    HOOK_SDL(SDL_GetTextureColorMod);
    HOOK_SDL(SDL_GetThreadID);
    HOOK_SDL(SDL_GetThreadName);
    HOOK_SDL(SDL_GetTicks);
    HOOK_SDL(SDL_GetTouchDevice);
    HOOK_SDL(SDL_GetTouchFinger);
    HOOK_SDL(SDL_GetVersion);
    HOOK_SDL(SDL_GetVideoDriver);
    HOOK_SDL(SDL_GetWindowBordersSize);
    HOOK_SDL(SDL_GetWindowBrightness);
    HOOK_SDL(SDL_GetWindowData);
    HOOK_SDL(SDL_GetWindowDisplayIndex);
    HOOK_SDL(SDL_GetWindowDisplayMode);
    HOOK_SDL(SDL_GetWindowFlags);
    HOOK_SDL(SDL_GetWindowFromID);
    HOOK_SDL(SDL_GetWindowGammaRamp);
    HOOK_SDL(SDL_GetWindowGrab);
    HOOK_SDL(SDL_GetWindowID);
    HOOK_SDL(SDL_GetWindowMaximumSize);
    HOOK_SDL(SDL_GetWindowMinimumSize);
    HOOK_SDL(SDL_GetWindowOpacity);
    HOOK_SDL(SDL_GetWindowPixelFormat);
    HOOK_SDL(SDL_GetWindowPosition);
    HOOK_SDL(SDL_GetWindowSize);
    HOOK_SDL(SDL_GetWindowSurface);
    HOOK_SDL(SDL_GetWindowTitle);
    HOOK_SDL(SDL_GetWindowWMInfo);
    HOOK_SDL(SDL_GetYUVConversionMode);
    HOOK_SDL(SDL_GetYUVConversionModeForResolution);
    HOOK_SDL(SDL_HapticClose);
    HOOK_SDL(SDL_HapticDestroyEffect);
    HOOK_SDL(SDL_HapticEffectSupported);
    HOOK_SDL(SDL_HapticGetEffectStatus);
    HOOK_SDL(SDL_HapticIndex);
    HOOK_SDL(SDL_HapticName);
    HOOK_SDL(SDL_HapticNewEffect);
    HOOK_SDL(SDL_HapticNumAxes);
    HOOK_SDL(SDL_HapticNumEffects);
    HOOK_SDL(SDL_HapticNumEffectsPlaying);
    HOOK_SDL(SDL_HapticOpen);
    HOOK_SDL(SDL_HapticOpenFromJoystick);
    HOOK_SDL(SDL_HapticOpenFromMouse);
    HOOK_SDL(SDL_HapticOpened);
    HOOK_SDL(SDL_HapticPause);
    HOOK_SDL(SDL_HapticQuery);
    HOOK_SDL(SDL_HapticRumbleInit);
    HOOK_SDL(SDL_HapticRumblePlay);
    HOOK_SDL(SDL_HapticRumbleStop);
    HOOK_SDL(SDL_HapticRumbleSupported);
    HOOK_SDL(SDL_HapticRunEffect);
    HOOK_SDL(SDL_HapticSetAutocenter);
    HOOK_SDL(SDL_HapticSetGain);
    HOOK_SDL(SDL_HapticStopAll);
    HOOK_SDL(SDL_HapticStopEffect);
    HOOK_SDL(SDL_HapticUnpause);
    HOOK_SDL(SDL_HapticUpdateEffect);
    HOOK_SDL(SDL_Has3DNow);
    HOOK_SDL(SDL_HasAVX);
    HOOK_SDL(SDL_HasAVX2);
    HOOK_SDL(SDL_HasAVX512F);
    HOOK_SDL(SDL_HasAltiVec);
    HOOK_SDL(SDL_HasClipboardText);
    HOOK_SDL(SDL_HasColorKey);
    HOOK_SDL(SDL_HasEvent);
    HOOK_SDL(SDL_HasEvents);
    HOOK_SDL(SDL_HasIntersection);
    HOOK_SDL(SDL_HasMMX);
    HOOK_SDL(SDL_HasNEON);
    HOOK_SDL(SDL_HasRDTSC);
    HOOK_SDL(SDL_HasSSE);
    HOOK_SDL(SDL_HasSSE2);
    HOOK_SDL(SDL_HasSSE3);
    HOOK_SDL(SDL_HasSSE41);
    HOOK_SDL(SDL_HasSSE42);
    HOOK_SDL(SDL_HasScreenKeyboardSupport);
    HOOK_SDL(SDL_HideWindow);
    HOOK_SDL(SDL_InitSubSystem);
    HOOK_SDL(SDL_IntersectRect);
    HOOK_SDL(SDL_IntersectRectAndLine);
    HOOK_SDL(SDL_IsGameController);
    HOOK_SDL(SDL_IsScreenKeyboardShown);
    HOOK_SDL(SDL_IsScreenSaverEnabled);
    HOOK_SDL(SDL_IsShapedWindow);
    HOOK_SDL(SDL_IsTablet);
    HOOK_SDL(SDL_IsTextInputActive);
    HOOK_SDL(SDL_JoystickClose);
    HOOK_SDL(SDL_JoystickCurrentPowerLevel);
    HOOK_SDL(SDL_JoystickEventState);
    HOOK_SDL(SDL_JoystickFromInstanceID);
    HOOK_SDL(SDL_JoystickGetAttached);
    HOOK_SDL(SDL_JoystickGetAxis);
    HOOK_SDL(SDL_JoystickGetAxisInitialState);
    HOOK_SDL(SDL_JoystickGetBall);
    HOOK_SDL(SDL_JoystickGetButton);
    HOOK_SDL(SDL_JoystickGetDeviceGUID);
    HOOK_SDL(SDL_JoystickGetDeviceInstanceID);
    HOOK_SDL(SDL_JoystickGetDevicePlayerIndex);
    HOOK_SDL(SDL_JoystickGetDeviceProduct);
    HOOK_SDL(SDL_JoystickGetDeviceProductVersion);
    HOOK_SDL(SDL_JoystickGetDeviceType);
    HOOK_SDL(SDL_JoystickGetDeviceVendor);
    HOOK_SDL(SDL_JoystickGetGUID);
    HOOK_SDL(SDL_JoystickGetGUIDFromString);
    HOOK_SDL(SDL_JoystickGetGUIDString);
    HOOK_SDL(SDL_JoystickGetHat);
    HOOK_SDL(SDL_JoystickGetPlayerIndex);
    HOOK_SDL(SDL_JoystickGetProduct);
    HOOK_SDL(SDL_JoystickGetProductVersion);
    HOOK_SDL(SDL_JoystickGetType);
    HOOK_SDL(SDL_JoystickGetVendor);
    HOOK_SDL(SDL_JoystickInstanceID);
    HOOK_SDL(SDL_JoystickIsHaptic);
    HOOK_SDL(SDL_JoystickName);
    HOOK_SDL(SDL_JoystickNameForIndex);
    HOOK_SDL(SDL_JoystickNumAxes);
    HOOK_SDL(SDL_JoystickNumBalls);
    HOOK_SDL(SDL_JoystickNumButtons);
    HOOK_SDL(SDL_JoystickNumHats);
    HOOK_SDL(SDL_JoystickOpen);
    HOOK_SDL(SDL_JoystickRumble);
    HOOK_SDL(SDL_JoystickUpdate);
    HOOK_SDL(SDL_LoadBMP_RW);
    HOOK_SDL(SDL_LoadDollarTemplates);
    HOOK_SDL(SDL_LoadFile_RW);
    HOOK_SDL(SDL_LoadFunction);
    HOOK_SDL(SDL_LoadObject);
    HOOK_SDL(SDL_LoadWAV_RW);
    HOOK_SDL(SDL_LockAudio);
    HOOK_SDL(SDL_LockAudioDevice);
    HOOK_SDL(SDL_LockJoysticks);
    HOOK_SDL(SDL_LockMutex);
    HOOK_SDL(SDL_LockSurface);
    HOOK_SDL(SDL_LockTexture);
    HOOK_SDL(SDL_Log);
    HOOK_SDL(SDL_LogCritical);
    HOOK_SDL(SDL_LogDebug);
    HOOK_SDL(SDL_LogError);
    HOOK_SDL(SDL_LogGetOutputFunction);
    HOOK_SDL(SDL_LogGetPriority);
    HOOK_SDL(SDL_LogInfo);
    HOOK_SDL(SDL_LogMessage);
    HOOK_SDL(SDL_LogMessageV);
    HOOK_SDL(SDL_LogResetPriorities);
    HOOK_SDL(SDL_LogSetAllPriority);
    HOOK_SDL(SDL_LogSetOutputFunction);
    HOOK_SDL(SDL_LogSetPriority);
    HOOK_SDL(SDL_LogVerbose);
    HOOK_SDL(SDL_LogWarn);
    HOOK_SDL(SDL_LowerBlit);
    HOOK_SDL(SDL_LowerBlitScaled);
    HOOK_SDL(SDL_MapRGB);
    HOOK_SDL(SDL_MapRGBA);
    HOOK_SDL(SDL_MasksToPixelFormatEnum);
    HOOK_SDL(SDL_MaximizeWindow);
    HOOK_SDL(SDL_MemoryBarrierAcquireFunction);
    HOOK_SDL(SDL_MemoryBarrierReleaseFunction);
    HOOK_SDL(SDL_MinimizeWindow);
    HOOK_SDL(SDL_MixAudio);
    HOOK_SDL(SDL_MixAudioFormat);
    HOOK_SDL(SDL_MouseIsHaptic);
    HOOK_SDL(SDL_NewAudioStream);
    HOOK_SDL(SDL_NumHaptics);
    HOOK_SDL(SDL_NumJoysticks);
    HOOK_SDL(SDL_NumSensors);
    HOOK_SDL(SDL_OpenAudio);
    HOOK_SDL(SDL_OpenAudioDevice);
    HOOK_SDL(SDL_PauseAudio);
    HOOK_SDL(SDL_PauseAudioDevice);
    HOOK_SDL(SDL_PeepEvents);
    HOOK_SDL(SDL_PixelFormatEnumToMasks);
    HOOK_SDL(SDL_PollEvent);
    HOOK_SDL(SDL_PumpEvents);
    HOOK_SDL(SDL_PushEvent);
    HOOK_SDL(SDL_QueryTexture);
    HOOK_SDL(SDL_QueueAudio);
    HOOK_SDL(SDL_Quit);
    HOOK_SDL(SDL_QuitSubSystem);
    HOOK_SDL(SDL_RWFromConstMem);
    HOOK_SDL_AS("SDL_RWFromFP", SDL_RWFromFP_vita);
    HOOK_SDL(SDL_RWFromMem);
    HOOK_SDL(SDL_RaiseWindow);
    HOOK_SDL(SDL_ReadBE16);
    HOOK_SDL(SDL_ReadBE32);
    HOOK_SDL(SDL_ReadBE64);
    HOOK_SDL(SDL_ReadLE16);
    HOOK_SDL(SDL_ReadLE32);
    HOOK_SDL(SDL_ReadLE64);
    HOOK_SDL(SDL_ReadU8);
    HOOK_SDL(SDL_RecordGesture);
    HOOK_SDL(SDL_RegisterEvents);
    HOOK_SDL(SDL_RemoveTimer);
    HOOK_SDL(SDL_RenderClear);
    HOOK_SDL(SDL_RenderCopy);
    HOOK_SDL(SDL_RenderCopyEx);
    HOOK_SDL(SDL_RenderDrawLine);
    HOOK_SDL(SDL_RenderDrawLines);
    HOOK_SDL(SDL_RenderDrawPoint);
    HOOK_SDL(SDL_RenderDrawPoints);
    HOOK_SDL(SDL_RenderDrawRect);
    HOOK_SDL(SDL_RenderDrawRects);
    HOOK_SDL(SDL_RenderFillRect);
    HOOK_SDL(SDL_RenderFillRects);
    HOOK_SDL(SDL_RenderGetClipRect);
    HOOK_SDL(SDL_RenderGetIntegerScale);
    HOOK_SDL(SDL_RenderGetLogicalSize);
    HOOK_SDL(SDL_RenderGetMetalCommandEncoder);
    HOOK_SDL(SDL_RenderGetMetalLayer);
    HOOK_SDL(SDL_RenderGetScale);
    HOOK_SDL(SDL_RenderGetViewport);
    HOOK_SDL(SDL_RenderIsClipEnabled);
    HOOK_SDL(SDL_RenderPresent);
    HOOK_SDL(SDL_RenderReadPixels);
    HOOK_SDL(SDL_RenderSetClipRect);
    HOOK_SDL(SDL_RenderSetIntegerScale);
    HOOK_SDL(SDL_RenderSetLogicalSize);
    HOOK_SDL(SDL_RenderSetScale);
    HOOK_SDL(SDL_RenderSetViewport);
    HOOK_SDL(SDL_RenderTargetSupported);
    HOOK_SDL(SDL_ReportAssertion);
    HOOK_SDL(SDL_ResetAssertionReport);
    HOOK_SDL(SDL_RestoreWindow);
    HOOK_SDL(SDL_SaveAllDollarTemplates);
    HOOK_SDL(SDL_SaveBMP_RW);
    HOOK_SDL(SDL_SaveDollarTemplate);
    HOOK_SDL(SDL_SemPost);
    HOOK_SDL(SDL_SemTryWait);
    HOOK_SDL(SDL_SemValue);
    HOOK_SDL(SDL_SemWait);
    HOOK_SDL(SDL_SemWaitTimeout);
    HOOK_SDL(SDL_SensorClose);
    HOOK_SDL(SDL_SensorFromInstanceID);
    HOOK_SDL(SDL_SensorGetData);
    HOOK_SDL(SDL_SensorGetDeviceInstanceID);
    HOOK_SDL(SDL_SensorGetDeviceName);
    HOOK_SDL(SDL_SensorGetDeviceNonPortableType);
    HOOK_SDL(SDL_SensorGetDeviceType);
    HOOK_SDL(SDL_SensorGetInstanceID);
    HOOK_SDL(SDL_SensorGetName);
    HOOK_SDL(SDL_SensorGetNonPortableType);
    HOOK_SDL(SDL_SensorGetType);
    HOOK_SDL(SDL_SensorOpen);
    HOOK_SDL(SDL_SensorUpdate);
    HOOK_SDL(SDL_SetAssertionHandler);
    HOOK_SDL(SDL_SetClipRect);
    HOOK_SDL(SDL_SetClipboardText);
    HOOK_SDL(SDL_SetColorKey);
    HOOK_SDL(SDL_SetCursor);
    HOOK_SDL(SDL_SetError);
    HOOK_SDL(SDL_SetEventFilter);
    HOOK_SDL(SDL_SetHint);
    HOOK_SDL(SDL_SetHintWithPriority);
    HOOK_SDL(SDL_SetMainReady);
    HOOK_SDL(SDL_SetMemoryFunctions);
    HOOK_SDL(SDL_SetModState);
    HOOK_SDL(SDL_SetPaletteColors);
    HOOK_SDL(SDL_SetPixelFormatPalette);
    HOOK_SDL(SDL_SetRelativeMouseMode);
    HOOK_SDL(SDL_SetRenderDrawBlendMode);
    HOOK_SDL(SDL_SetRenderDrawColor);
    HOOK_SDL(SDL_SetRenderTarget);
    HOOK_SDL(SDL_SetSurfaceAlphaMod);
    HOOK_SDL(SDL_SetSurfaceBlendMode);
    HOOK_SDL(SDL_SetSurfaceColorMod);
    HOOK_SDL(SDL_SetSurfacePalette);
    HOOK_SDL(SDL_SetSurfaceRLE);
    HOOK_SDL(SDL_SetTextInputRect);
    HOOK_SDL(SDL_SetTextureAlphaMod);
    HOOK_SDL(SDL_SetTextureBlendMode);
    HOOK_SDL(SDL_SetTextureColorMod);
    HOOK_SDL(SDL_SetThreadPriority);
    HOOK_SDL(SDL_SetWindowBordered);
    HOOK_SDL(SDL_SetWindowBrightness);
    HOOK_SDL(SDL_SetWindowData);
    HOOK_SDL(SDL_SetWindowDisplayMode);
    HOOK_SDL(SDL_SetWindowFullscreen);
    HOOK_SDL(SDL_SetWindowGammaRamp);
    HOOK_SDL(SDL_SetWindowGrab);
    HOOK_SDL(SDL_SetWindowHitTest);
    HOOK_SDL(SDL_SetWindowIcon);
    HOOK_SDL(SDL_SetWindowInputFocus);
    HOOK_SDL(SDL_SetWindowMaximumSize);
    HOOK_SDL(SDL_SetWindowMinimumSize);
    HOOK_SDL(SDL_SetWindowModalFor);
    HOOK_SDL(SDL_SetWindowOpacity);
    HOOK_SDL(SDL_SetWindowPosition);
    HOOK_SDL(SDL_SetWindowResizable);
    HOOK_SDL(SDL_SetWindowShape);
    HOOK_SDL(SDL_SetWindowSize);
    HOOK_SDL(SDL_SetWindowTitle);
    HOOK_SDL(SDL_SetYUVConversionMode);
    HOOK_SDL(SDL_ShowCursor);
    HOOK_SDL(SDL_ShowMessageBox);
    HOOK_SDL(SDL_ShowSimpleMessageBox);
    HOOK_SDL(SDL_ShowWindow);
    HOOK_SDL(SDL_SoftStretch);
    HOOK_SDL(SDL_StartTextInput);
    HOOK_SDL(SDL_StopTextInput);
    HOOK_SDL(SDL_TLSCreate);
    HOOK_SDL(SDL_TLSGet);
    HOOK_SDL(SDL_TLSSet);
    HOOK_SDL(SDL_ThreadID);
    HOOK_SDL(SDL_TryLockMutex);
    HOOK_SDL(SDL_UnionRect);
    HOOK_SDL(SDL_UnloadObject);
    HOOK_SDL(SDL_UnlockAudio);
    HOOK_SDL(SDL_UnlockAudioDevice);
    HOOK_SDL(SDL_UnlockJoysticks);
    HOOK_SDL(SDL_UnlockMutex);
    HOOK_SDL(SDL_UnlockSurface);
    HOOK_SDL(SDL_UnlockTexture);
    HOOK_SDL(SDL_UpdateTexture);
    HOOK_SDL(SDL_UpdateWindowSurface);
    HOOK_SDL(SDL_UpdateWindowSurfaceRects);
    HOOK_SDL(SDL_UpdateYUVTexture);
    HOOK_SDL(SDL_UpperBlit);
    HOOK_SDL(SDL_UpperBlitScaled);
    HOOK_SDL(SDL_VideoInit);
    HOOK_SDL(SDL_VideoQuit);
    HOOK_SDL(SDL_WaitEvent);
    HOOK_SDL(SDL_WaitEventTimeout);
    HOOK_SDL(SDL_WaitThread);
    HOOK_SDL(SDL_WarpMouseGlobal);
    HOOK_SDL(SDL_WarpMouseInWindow);
    HOOK_SDL(SDL_WasInit);
    HOOK_SDL(SDL_WriteBE16);
    HOOK_SDL(SDL_WriteBE32);
    HOOK_SDL(SDL_WriteBE64);
    HOOK_SDL(SDL_WriteLE16);
    HOOK_SDL(SDL_WriteLE32);
    HOOK_SDL(SDL_WriteLE64);
    HOOK_SDL(SDL_WriteU8);
    HOOK_SDL(SDL_abs);
    HOOK_SDL(SDL_acos);
    HOOK_SDL(SDL_acosf);
    HOOK_SDL(SDL_asin);
    HOOK_SDL(SDL_asinf);
    HOOK_SDL(SDL_atan);
    HOOK_SDL(SDL_atan2);
    HOOK_SDL(SDL_atan2f);
    HOOK_SDL(SDL_atanf);
    HOOK_SDL(SDL_atof);
    HOOK_SDL(SDL_atoi);
    HOOK_SDL(SDL_calloc);
    HOOK_SDL(SDL_ceil);
    HOOK_SDL(SDL_ceilf);
    HOOK_SDL(SDL_copysign);
    HOOK_SDL(SDL_copysignf);
    HOOK_SDL(SDL_cos);
    HOOK_SDL(SDL_cosf);
    HOOK_SDL(SDL_exp);
    HOOK_SDL(SDL_expf);
    HOOK_SDL(SDL_fabs);
    HOOK_SDL(SDL_fabsf);
    HOOK_SDL(SDL_floor);
    HOOK_SDL(SDL_floorf);
    HOOK_SDL(SDL_fmod);
    HOOK_SDL(SDL_fmodf);
    HOOK_SDL(SDL_free);
    HOOK_SDL(SDL_getenv);
    HOOK_SDL(SDL_iconv);
    HOOK_SDL(SDL_iconv_close);
    HOOK_SDL(SDL_iconv_open);
    HOOK_SDL(SDL_iconv_string);
    HOOK_SDL(SDL_isdigit);
    HOOK_SDL(SDL_isspace);
    HOOK_SDL(SDL_itoa);
    HOOK_SDL(SDL_lltoa);
    HOOK_SDL(SDL_log);
    HOOK_SDL(SDL_log10);
    HOOK_SDL(SDL_log10f);
    HOOK_SDL(SDL_logf);
    HOOK_SDL(SDL_ltoa);
    HOOK_SDL(SDL_malloc);
    HOOK_SDL(SDL_memcmp);
    HOOK_SDL(SDL_memcpy);
    HOOK_SDL(SDL_memmove);
    HOOK_SDL(SDL_memset);
    HOOK_SDL(SDL_pow);
    HOOK_SDL(SDL_powf);
    HOOK_SDL(SDL_qsort);
    HOOK_SDL(SDL_realloc);
    HOOK_SDL(SDL_scalbn);
    HOOK_SDL(SDL_scalbnf);
    HOOK_SDL(SDL_setenv);
    HOOK_SDL(SDL_sin);
    HOOK_SDL(SDL_sinf);
    HOOK_SDL(SDL_snprintf);
    HOOK_SDL(SDL_sqrt);
    HOOK_SDL(SDL_sqrtf);
    HOOK_SDL(SDL_sscanf);
    HOOK_SDL(SDL_strcasecmp);
    HOOK_SDL(SDL_strchr);
    HOOK_SDL(SDL_strcmp);
    HOOK_SDL(SDL_strdup);
    HOOK_SDL(SDL_strlcat);
    HOOK_SDL(SDL_strlcpy);
    HOOK_SDL(SDL_strlen);
    HOOK_SDL(SDL_strlwr);
    HOOK_SDL(SDL_strncasecmp);
    HOOK_SDL(SDL_strncmp);
    HOOK_SDL(SDL_strrchr);
    HOOK_SDL(SDL_strrev);
    HOOK_SDL(SDL_strstr);
    HOOK_SDL(SDL_strtod);
    HOOK_SDL(SDL_strtol);
    HOOK_SDL(SDL_strtoll);
    HOOK_SDL(SDL_strtoul);
    HOOK_SDL(SDL_strtoull);
    HOOK_SDL(SDL_strupr);
    HOOK_SDL(SDL_tan);
    HOOK_SDL(SDL_tanf);
    HOOK_SDL(SDL_tolower);
    HOOK_SDL(SDL_toupper);
    HOOK_SDL(SDL_uitoa);
    HOOK_SDL(SDL_ulltoa);
    HOOK_SDL(SDL_ultoa);
    HOOK_SDL(SDL_utf8strlcpy);
    HOOK_SDL(SDL_utf8strlen);
    HOOK_SDL(SDL_vsnprintf);
    HOOK_SDL(SDL_vsscanf);
    HOOK_SDL(SDL_wcscmp);
    HOOK_SDL(SDL_wcsdup);
    HOOK_SDL(SDL_wcslcat);
    HOOK_SDL(SDL_wcslcpy);
    HOOK_SDL(SDL_wcslen);

    l_success("SDL native hook pass applied %i hooks.", s_sdl_hooks_applied);
}
