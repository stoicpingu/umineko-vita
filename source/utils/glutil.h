/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  glutil.h
 * @brief OpenGL API initializer, related functions.
 */

#ifndef SOLOADER_GLUTIL_H
#define SOLOADER_GLUTIL_H

#include <vitaGL.h>

#ifdef __cplusplus
extern "C" {
#endif

void gl_init();

void gl_preload();

void gl_swap();

void pivas_log_memory_snapshot(const char *tag);

void glClear_soloader(GLbitfield mask);

void glClearColor_soloader(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);

void glCompileShader_soloader(GLuint shader);

void glLinkProgram_soloader(GLuint program);

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length);

void glPixelStorei_soloader(GLenum pname, GLint param);

void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, const GLvoid *pixels);

void glTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset,
                              GLint yoffset, GLsizei width, GLsizei height,
                              GLenum format, GLenum type, const GLvoid *pixels);

void glUseProgram_soloader(GLuint program);

GLint glGetUniformLocation_soloader(GLuint program, const GLchar *name);

GLint glGetAttribLocation_soloader(GLuint program, const GLchar *name);

void glUniform1i_soloader(GLint location, GLint v0);

void glUniform1f_soloader(GLint location, GLfloat v0);

void glUniform1fv_soloader(GLint location, GLsizei count, const GLfloat *value);

void glUniform2fv_soloader(GLint location, GLsizei count, const GLfloat *value);

void glUniform4fv_soloader(GLint location, GLsizei count, const GLfloat *value);

void glUniformMatrix4fv_soloader(GLint location, GLsizei count,
                                 GLboolean transpose, const GLfloat *value);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
