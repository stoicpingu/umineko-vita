#pragma once
#include <vitaGL.h>

/* Release loads only bundled native programs. Export is a developer build. */
int ons_vita_shader_load(GLuint shader, GLenum type, const char *source);
void ons_vita_shader_export_begin(void);
int ons_vita_shader_export_end(GLuint shader, GLenum type, const char *source);
