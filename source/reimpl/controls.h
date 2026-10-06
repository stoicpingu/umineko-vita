/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  controls.h
 * @brief Vita pad -> ONScripter-RU keyboard-event translation.
 */

#ifndef SOLOADER_CONTROLS_H
#define SOLOADER_CONTROLS_H

#include <stdint.h>

typedef enum ControlsAction {
    CONTROLS_ACTION_UP = 0,
    CONTROLS_ACTION_DOWN = 1,
    CONTROLS_ACTION_MOVE = 2
} ControlsAction;

/* ONScripter-RU virtual scancodes (Joystick.hpp: SDL_NUM_SCANCODES + n) */
#define ONS_SCANCODE_MUTE   513
#define ONS_SCANCODE_SKIP   514
#define ONS_SCANCODE_SCREEN 515
/* Engine/Components/Joystick.hpp PIVAS_SCANCODE_*: trigger identity, sent in
 * addition to the trigger's regular key (menus use it for page switching). */
#define PIVAS_SCANCODE_LTRIGGER 516
#define PIVAS_SCANCODE_RTRIGGER 517
/* Circle: the engine resolves it to Z (menu) or ESC (back) by context. */
#define PIVAS_SCANCODE_CIRCLE 518
/* Analog sticks: sent as their own codes so the engine can ignore them
 * while the textbox is up (reading) and treat them as arrows / backlog
 * paging everywhere else. */
#define PIVAS_SCANCODE_LSTICK_UP    519
#define PIVAS_SCANCODE_LSTICK_DOWN  520
#define PIVAS_SCANCODE_LSTICK_LEFT  521
#define PIVAS_SCANCODE_LSTICK_RIGHT 522
#define PIVAS_SCANCODE_RSTICK_UP    523
#define PIVAS_SCANCODE_RSTICK_DOWN  524

/* `keycode` is an SDL_SCANCODE_* / ONS_SCANCODE_* value. */
extern void controls_handler_key(int32_t keycode, ControlsAction action);

void controls_init();
void controls_poll();

/* Non-zero: keep tracking the pad but emit no key events (system dialog up). */
void controls_set_suspended(int suspended);

#endif // SOLOADER_CONTROLS_H
