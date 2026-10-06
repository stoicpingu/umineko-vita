/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/*
 * Vita pad -> ONScripter-RU keyboard translation.
 *
 * The engine (DROID build) acts on SDL_KEYDOWN/KEYUP scancodes pulled from
 * the native SDL2 queue (Engine/Core/Event.cpp). Its SDL_JOYBUTTON path
 * runs through a GUID table that does not know the Vita pad and falls back
 * to a DualShock 3 layout (Cross maps to nothing, Start toggles skip), so
 * native joystick events are silenced in patch.c and the pad is translated
 * here instead. Actions fire on KEYUP (keyPressEvent via
 * translateKeyUpEvent) and KEYDOWN is stateless for every scancode we map
 * (keyDownEvent only tracks ctrl/alt/shift modifiers), so each press is
 * delivered as a complete DOWN+UP pulse at press time — the action fires
 * the moment the button goes down instead of waiting for the physical
 * release. Held directions keep pulsing at the auto-repeat rate; releases
 * send nothing (the UP already went out with the pulse).
 *
 * Touch is NOT polled here: the native SDL2 Vita touch driver already
 * delivers finger events and the engine's own gesture layer handles them
 * (1 tap = advance, 2-finger tap = menu, 2-finger drag = backlog scroll,
 * 3-finger swipes = skip/auto/hide/mute).
 */

#include "reimpl/controls.h"

#include <math.h>
#include <psp2/ctrl.h>

#include <SDL2/SDL.h>

/* controls_poll() runs at ~60 Hz (main.c sleeps 16666 us) */
#define REPEAT_DELAY_TICKS 18 /* ~300 ms before auto-repeat */
#define REPEAT_RATE_TICKS  5  /* ~83 ms between repeats     */

#define ANALOG_DEADZONE 0.16f
#define LSTICK_ON       0.50f /* digital engage threshold */
#define LSTICK_OFF      0.35f /* release hysteresis       */
#define RSTICK_ON       0.60f /* backlog paging threshold */
#define RSTICK_OFF      0.40f
#define RSTICK_RATE     15    /* ~250 ms between backlog pages */

typedef struct {
    uint32_t sce_button;
    int32_t scancode;
    int repeat;
    int held_ticks; /* -1 = not held */
    int32_t extra_scancode; /* pulsed after scancode; 0 = none */
} VitaKey;

/*
 * Scancode semantics for THIS script (umineko runs `useescspc` and arms
 * getzxc/gettab before every textbtnwait, decoded *text_cwlp):
 *   Z      -> -51 -> *text_cw_rclk  = open system/save menu (same handler
 *                                     as the 2-finger-tap right click)
 *   ESCAPE -> -10 -> *text_cw_hide  = hide/show the textbox; closes menus
 *   H      -> -2  -> *text_cw_lookback_go = open backlog / page back
 *   TAB    -> -20 -> chapter skip!  (deliberately NOT mapped on a button)
 * Circle sends PIVAS_SCANCODE_CIRCLE, which the engine turns into Z while
 * the textbox is up (open the menu) and ESCAPE otherwise (back / close).
 */
static VitaKey key_map[] = {
    { SCE_CTRL_CROSS,    SDL_SCANCODE_RETURN, 0, -1 }, /* advance/confirm  */
    { SCE_CTRL_CIRCLE,   PIVAS_SCANCODE_CIRCLE, 0, -1 }, /* menu / back    */
    { SCE_CTRL_START,    SDL_SCANCODE_Z,      0, -1 }, /* system/save menu */
    { SCE_CTRL_SQUARE,   SDL_SCANCODE_A,      0, -1 }, /* auto mode        */
    { SCE_CTRL_TRIANGLE, SDL_SCANCODE_ESCAPE, 0, -1 }, /* hide text window */
    { SCE_CTRL_SELECT,   ONS_SCANCODE_MUTE,   0, -1 }, /* mute toggle      */
    /* backlog page up / skip mode toggle; the extra code lets the Bookmarks
     * screen flip pages on the triggers without Circle (also H) doing so.
     * sceCtrlPeekBufferPositiveExt2 reports the Vita's own L/R as L1/R1
     * (LTRIGGER/RTRIGGER are L2/R2 there, i.e. only an external pad), so
     * both bits are mapped. */
    { SCE_CTRL_L1 | SCE_CTRL_LTRIGGER, SDL_SCANCODE_H,    0, -1, PIVAS_SCANCODE_LTRIGGER },
    { SCE_CTRL_R1 | SCE_CTRL_RTRIGGER, ONS_SCANCODE_SKIP, 0, -1, PIVAS_SCANCODE_RTRIGGER },
    { SCE_CTRL_UP,       SDL_SCANCODE_UP,     1, -1 },
    { SCE_CTRL_DOWN,     SDL_SCANCODE_DOWN,   1, -1 },
    { SCE_CTRL_LEFT,     SDL_SCANCODE_LEFT,   1, -1 }, /* backlog in text  */
    { SCE_CTRL_RIGHT,    SDL_SCANCODE_RIGHT,  1, -1 },
};

void controls_init() {
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
}

/*
 * While a system dialog owns the pad (utils/dialog.c pivas_msg_dialog_*),
 * the app still reads every press, so the Cross that dismisses the dialog
 * would also click whatever the engine has hovered. Suspended polls keep
 * the held-state tracking in sync but emit nothing, so a button still down
 * when the dialog closes does not fire on resume either.
 */
static volatile int controls_suspended = 0;

void controls_set_suspended(int suspended) {
    controls_suspended = suspended;
}

/* Complete press: the engine acts on the UP half of the pulse. */
static void pulse_key(int32_t scancode) {
    controls_handler_key(scancode, CONTROLS_ACTION_DOWN);
    controls_handler_key(scancode, CONTROLS_ACTION_UP);
}

static void poll_buttons(uint32_t buttons, int silent) {
    for (size_t i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
        VitaKey *k = &key_map[i];
        int held = (buttons & k->sce_button) != 0;

        if (silent) {
            k->held_ticks = held ? 0 : -1;
        } else if (held && k->held_ticks < 0) {
            k->held_ticks = 0;
            pulse_key(k->scancode);
            if (k->extra_scancode)
                pulse_key(k->extra_scancode);
        } else if (held) {
            k->held_ticks++;
            if (k->repeat && k->held_ticks >= REPEAT_DELAY_TICKS &&
                (k->held_ticks - REPEAT_DELAY_TICKS) % REPEAT_RATE_TICKS == 0) {
                pulse_key(k->scancode);
                if (k->extra_scancode)
                    pulse_key(k->extra_scancode);
            }
        } else if (!held && k->held_ticks >= 0) {
            k->held_ticks = -1;
        }
    }
}

/* ---- left stick: digital arrows with hysteresis + repeat ---- */

static const int32_t dir_scancode[4] = {
    PIVAS_SCANCODE_LSTICK_UP, PIVAS_SCANCODE_LSTICK_DOWN,
    PIVAS_SCANCODE_LSTICK_LEFT, PIVAS_SCANCODE_LSTICK_RIGHT
};
static int lstick_dir = -1;
static int lstick_ticks = 0;

static int stick_direction(float x, float y) {
    if (fabsf(x) >= fabsf(y))
        return x > 0.0f ? 3 : 2;
    return y > 0.0f ? 1 : 0; /* sceCtrl: +y = down */
}

static void poll_left_stick(float x, float y) {
    float mag = sqrtf(x * x + y * y);

    if (lstick_dir < 0) {
        if (mag >= LSTICK_ON) {
            lstick_dir = stick_direction(x, y);
            lstick_ticks = 0;
            pulse_key(dir_scancode[lstick_dir]);
        }
        return;
    }

    if (mag < LSTICK_OFF) {
        lstick_dir = -1;
        return;
    }

    int dir = stick_direction(x, y);
    if (dir != lstick_dir) {
        lstick_dir = dir;
        lstick_ticks = 0;
        pulse_key(dir_scancode[dir]);
    } else {
        lstick_ticks++;
        if (lstick_ticks >= REPEAT_DELAY_TICKS &&
            (lstick_ticks - REPEAT_DELAY_TICKS) % REPEAT_RATE_TICKS == 0) {
            pulse_key(dir_scancode[dir]);
        }
    }
}

/* ---- right stick: backlog paging (up = back, down = forward) ---- */

static int rstick_dir = 0; /* -1 up, 1 down, 0 idle */
static int rstick_ticks = 0;

static void rstick_pulse(void) {
    pulse_key(rstick_dir < 0 ? PIVAS_SCANCODE_RSTICK_UP : PIVAS_SCANCODE_RSTICK_DOWN);
}

static void poll_right_stick(float y) {
    if (rstick_dir == 0) {
        if (y <= -RSTICK_ON)
            rstick_dir = -1;
        else if (y >= RSTICK_ON)
            rstick_dir = 1;
        else
            return;
        rstick_ticks = 0;
        rstick_pulse();
    } else {
        if (fabsf(y) < RSTICK_OFF) {
            rstick_dir = 0;
            return;
        }
        rstick_ticks++;
        if (rstick_ticks % RSTICK_RATE == 0)
            rstick_pulse();
    }
}

void controls_poll() {
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) < 0)
        return;

    int silent = controls_suspended;
    poll_buttons(pad.buttons, silent);

    float lx = ((float)pad.lx - 128.0f) / 128.0f;
    float ly = ((float)pad.ly - 128.0f) / 128.0f;
    float ry = ((float)pad.ry - 128.0f) / 128.0f;

    if (sqrtf(lx * lx + ly * ly) < ANALOG_DEADZONE) {
        lx = 0.0f;
        ly = 0.0f;
    }
    if (fabsf(ry) < ANALOG_DEADZONE)
        ry = 0.0f;

    if (silent) {
        /* Re-engage from scratch once the stick returns to centre. */
        lstick_dir = (sqrtf(lx * lx + ly * ly) >= LSTICK_OFF)
                         ? stick_direction(lx, ly) : -1;
        lstick_ticks = 0;
        rstick_dir = (fabsf(ry) >= RSTICK_OFF) ? (ry < 0.0f ? -1 : 1) : 0;
        rstick_ticks = 1;
        return;
    }

    poll_left_stick(lx, ly);
    poll_right_stick(ry);
}
