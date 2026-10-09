/* Minimal boundary to unchanged vitaGL translator globals; LGPL-3.0-or-later. */
#ifndef ONS_VITAGL_TRANSLATOR_BRIDGE_H
#define ONS_VITAGL_TRANSLATOR_BRIDGE_H
#include <stdio.h>
#include <string.h>
#ifdef VITA
#include <vitaGL.h>
#else
/* Host-only scalar declarations; native builds use the actual SDK header. */
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef int GLsizei;
typedef char GLchar;
typedef int GLint;
#define GL_TRUE 1
#define GL_FALSE 0
#define GL_VERTEX_SHADER 0x8b31
#define GL_FRAGMENT_SHADER 0x8b30
enum { VGL_TYPE_NONE, VGL_TYPE_TEXCOORD, VGL_TYPE_COLOR, VGL_TYPE_FOG, VGL_TYPE_CLIP };
#endif
typedef struct shader shader;
#include "upstream/glsl_utils.h"
#define min(a, b) ((a) < (b) ? (a) : (b))
#define vgl_fast_memcpy memcpy
#define vgl_memset memset
#define vgl_log(...) fprintf(stderr, __VA_ARGS__)
#endif
