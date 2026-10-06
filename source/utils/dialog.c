/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      fgsfds
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/dialog.h"

#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/message_dialog.h>

#include <vitaGL.h>

#include "reimpl/controls.h"
#include "utils/logger.h"

static uint16_t ime_title_utf16[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
static uint16_t ime_initial_text_utf16[SCE_IME_DIALOG_MAX_TEXT_LENGTH];
static uint16_t ime_input_text_utf16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static uint8_t ime_input_text_utf8[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];

void _utf16_to_utf8(const uint16_t *src, uint8_t *dst) {
    for (int i = 0; src[i]; i++) {
        if ((src[i] & 0xFF80) == 0) {
            *(dst++) = src[i] & 0xFF;
        } else if((src[i] & 0xF800) == 0) {
            *(dst++) = ((src[i] >> 6) & 0xFF) | 0xC0;
            *(dst++) = (src[i] & 0x3F) | 0x80;
        } else if((src[i] & 0xFC00) == 0xD800 && (src[i + 1] & 0xFC00) == 0xDC00) {
            *(dst++) = (((src[i] + 64) >> 8) & 0x3) | 0xF0;
            *(dst++) = (((src[i] >> 2) + 16) & 0x3F) | 0x80;
            *(dst++) = ((src[i] >> 4) & 0x30) | 0x80 | ((src[i + 1] << 2) & 0xF);
            *(dst++) = (src[i + 1] & 0x3F) | 0x80;
            i += 1;
        } else {
            *(dst++) = ((src[i] >> 12) & 0xF) | 0xE0;
            *(dst++) = ((src[i] >> 6) & 0x3F) | 0x80;
            *(dst++) = (src[i] & 0x3F) | 0x80;
        }
    }

    *dst = '\0';
}

void _utf8_to_utf16(const uint8_t *src, uint16_t *dst) {
    for (int i = 0; src[i];) {
        if ((src[i] & 0xE0) == 0xE0) {
            *(dst++) = ((src[i]&0x0F) <<12) | ((src[i+1]&0x3F) <<6) | (src[i+2]&0x3F);
            i += 3;
        } else if ((src[i] & 0xC0) == 0xC0) {
            *(dst++) = ((src[i] & 0x1F) << 6) | (src[i + 1] & 0x3F);
            i += 2;
        } else {
            *(dst++) = src[i];
            i += 1;
        }
    }

    *dst = '\0';
}

int init_ime_dialog(const char *title, const char *initial_text) {
    sceClibMemset(ime_title_utf16, 0, sizeof(ime_title_utf16));
    sceClibMemset(ime_initial_text_utf16, 0, sizeof(ime_initial_text_utf16));
    sceClibMemset(ime_input_text_utf16, 0, sizeof(ime_input_text_utf16));
    sceClibMemset(ime_input_text_utf8, 0, sizeof(ime_input_text_utf8));

    _utf8_to_utf16((uint8_t *)title, ime_title_utf16);
    _utf8_to_utf16((uint8_t *)initial_text, ime_initial_text_utf16);

    SceImeDialogParam param;
    sceImeDialogParamInit(&param);

    param.supportedLanguages = 0x0001FFFF;
    param.languagesForced = SCE_TRUE;
    param.type = SCE_IME_TYPE_BASIC_LATIN;
    param.title = ime_title_utf16;
    param.maxTextLength = SCE_IME_DIALOG_MAX_TEXT_LENGTH;
    param.initialText = ime_initial_text_utf16;
    param.inputTextBuffer = ime_input_text_utf16;

    return sceImeDialogInit(&param);
}

char *get_ime_dialog_result(void) {
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return NULL;

    SceImeDialogResult result;
    sceClibMemset(&result, 0, sizeof(SceImeDialogResult));
    sceImeDialogGetResult(&result);
    if (result.button == SCE_IME_DIALOG_BUTTON_ENTER)
        _utf16_to_utf8(ime_input_text_utf16, ime_input_text_utf8);
    sceImeDialogTerm();
    // For some reason analog stick stops working after ime
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);

    return (char *)ime_input_text_utf8;
}

int init_msg_dialog(const char *msg) {
    SceMsgDialogUserMessageParam msg_param;
    sceClibMemset(&msg_param, 0, sizeof(msg_param));
    msg_param.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    msg_param.msg = (SceChar8 *)msg;

    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    _sceCommonDialogSetMagicNumber(&param.commonParam);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &msg_param;

    return sceMsgDialogInit(&param);
}

int get_msg_dialog_result(void) {
    if (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return 0;
    sceMsgDialogTerm();
    return 1;
}

/*
 * In-game message dialog for the engine (dynlib.c exports these to
 * libmain.so). The engine keeps running its own present loop while the
 * dialog is up; pivas_msg_dialog_active() makes the SDL_GL_SwapWindow hook
 * (patch.c) swap with the common-dialog flag so the dialog is composited.
 */
static volatile int s_msg_dialog_active = 0;
static char s_msg_dialog_text[512];

int pivas_msg_dialog_active(void) {
    return s_msg_dialog_active;
}

int pivas_msg_dialog_open(const char *msg) {
    if (s_msg_dialog_active || !msg)
        return -1;

    sceClibSnprintf(s_msg_dialog_text, sizeof(s_msg_dialog_text), "%s", msg);

    controls_set_suspended(1);
    int res = init_msg_dialog(s_msg_dialog_text);
    if (res < 0) {
        controls_set_suspended(0);
        l_error("[dialog] sceMsgDialogInit failed: 0x%08x", res);
        return res;
    }

    s_msg_dialog_active = 1;
    return 0;
}

int pivas_msg_dialog_running(void) {
    if (!s_msg_dialog_active)
        return 0;
    if (!get_msg_dialog_result())
        return 1;

    s_msg_dialog_active = 0;
    controls_set_suspended(0);
    return 0;
}

void fatal_error(const char *fmt, ...) {
    va_list list;
    char string[512];

    va_start(list, fmt);
    sceClibVsnprintf(string, sizeof(string), fmt, list);
    va_end(list);

    vglInit(0);

    init_msg_dialog(string);

    while (!get_msg_dialog_result())
        vglSwapBuffers(GL_TRUE);

    sceKernelExitProcess(0);

    sceKernelExitProcess(0);
    while (1);
}
