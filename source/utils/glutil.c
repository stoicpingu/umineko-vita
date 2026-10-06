/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <psp2/kernel/sysmem.h>

#ifndef PIVAS_VERBOSE_GL_LOGGING
#define PIVAS_VERBOSE_GL_LOGGING 0
#endif

#ifndef PIVAS_GL_ERROR_LOG_LIMIT
#define PIVAS_GL_ERROR_LOG_LIMIT 32
#endif

#ifndef PIVAS_SHADER_DIAGNOSTICS
#define PIVAS_SHADER_DIAGNOSTICS 0
#endif

#ifndef PIVAS_SHADER_LOG_LIMIT
#define PIVAS_SHADER_LOG_LIMIT 160
#endif

#ifndef PIVAS_SHADER_PROBE
#define PIVAS_SHADER_PROBE 0
#endif

#ifndef PIVAS_SHADER_INTERFACE_DUMP
#define PIVAS_SHADER_INTERFACE_DUMP 0
#endif

#ifndef PIVAS_UNIFORM_LOG_LIMIT
#define PIVAS_UNIFORM_LOG_LIMIT 384
#endif

#ifndef PIVAS_TEXTURE_UPLOAD_DIAGNOSTICS
#define PIVAS_TEXTURE_UPLOAD_DIAGNOSTICS 0
#endif

#ifndef PIVAS_TEXTURE_ERROR_LOG_LIMIT
#define PIVAS_TEXTURE_ERROR_LOG_LIMIT 48
#endif

#ifndef GL_BOOL
#define GL_BOOL 0x8B56
#endif

#ifndef GL_BOOL_VEC2
#define GL_BOOL_VEC2 0x8B57
#endif

#ifndef GL_BOOL_VEC3
#define GL_BOOL_VEC3 0x8B58
#endif

#ifndef GL_BOOL_VEC4
#define GL_BOOL_VEC4 0x8B59
#endif

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);
static void pivas_run_shader_probe(const char *stage);

#ifdef USE_GLSL_SHADERS
static GLenum pivas_semantic_mode = VGL_MODE_SHADER_PAIR;
static const char *pivas_semantic_mode_name = "SHADER_PAIR";

static int pivas_ascii_contains_token(const char *haystack, const char *needle) {
    char normalized[96];
    size_t i = 0;

    if (!haystack || !needle)
        return 0;

    for (; haystack[i] && i + 1 < sizeof(normalized); ++i)
        normalized[i] = (char)tolower((unsigned char)haystack[i]);
    normalized[i] = '\0';

    return strstr(normalized, needle) != NULL;
}

static GLenum pivas_select_semantic_mode(const char **name, const char **source) {
    static const char *paths[] = {
        "ux0:/data/umineko/pivas_shader_mode.txt",
        "pivas_shader_mode.txt",
        NULL
    };
    char line[96] = {0};
    FILE *fp = NULL;

    if (source)
        *source = "default";

    for (int i = 0; paths[i]; ++i) {
        fp = fopen(paths[i], "r");
        if (fp) {
            if (source)
                *source = paths[i];
            break;
        }
    }

    if (fp) {
        if (!fgets(line, sizeof(line), fp))
            line[0] = '\0';
        fclose(fp);
    }

    if (pivas_ascii_contains_token(line, "pair")) {
        if (name)
            *name = "SHADER_PAIR";
        return VGL_MODE_SHADER_PAIR;
    }

    if (pivas_ascii_contains_token(line, "global")) {
        if (name)
            *name = "GLOBAL";
        return VGL_MODE_GLOBAL;
    }

    if (pivas_ascii_contains_token(line, "post")) {
        if (name)
            *name = "POSTPONED";
        return VGL_MODE_POSTPONED;
    }

    if (line[0] && !pivas_ascii_contains_token(line, "post")) {
        l_warn("[shader] unknown pivas_shader_mode.txt value '%s'; using SHADER_PAIR", line);
    }

    if (name)
        *name = "SHADER_PAIR";
    return VGL_MODE_SHADER_PAIR;
}
#endif

void gl_preload() {
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

#ifdef USE_GLSL_SHADERS
    /*
     * SHADER_PAIR is the default. pivas_shader_mode.txt can select POSTPONED
     * (which changes uniform handles), but that mode crashes while ONScripter
     * links its shader programs.
     */
    const char *mode_source = "default";
    pivas_semantic_mode = pivas_select_semantic_mode(&pivas_semantic_mode_name,
                                                     &mode_source);
    vglSetSemanticBindingMode(pivas_semantic_mode);
    l_info("[shader] vitaGL semantic binding mode=%s source=%s",
           pivas_semantic_mode_name, mode_source);
#endif
}

void gl_init() {
    pivas_log_memory_snapshot("before vglInit");
    vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, SHARK_ENABLE,
                                   SHARK_ENABLE, SHARK_ENABLE);
    vglSetupGarbageCollector(127, 0x20000);

    /*
     * The threshold arguments are memory to leave free for the app, not
     * memory to reserve for vitaGL. Engine CPU surfaces live in the newlib
     * heap (reserved before this runs), not in free USER_RW, so the threshold
     * only needs to cover loader-side allocations. A threshold larger than
     * the USER_RW free at init leaves vitaGL with no pool, spilling every
     * texture into the newlib heap where it fragments against the engine's
     * transient surfaces.
     */
    vglInitWithCustomThreshold(0, 960, 544,
                               12 * 1024 * 1024,
                               0,
                               20 * 1024 * 1024,
                               12 * 1024 * 1024,
                               SCE_GXM_MULTISAMPLE_NONE);
    pivas_log_memory_snapshot("after vglInit");

#if defined(USE_GLSL_SHADERS) && PIVAS_SHADER_PROBE
    pivas_run_shader_probe("post-init");
#endif
}

void gl_swap() {
    vglSwapBuffers(GL_FALSE);
}

static unsigned int pivas_tex_event_count = 0;
static unsigned int pivas_clear_event_count = 0;
static unsigned int pivas_program_event_count = 0;
static unsigned int pivas_shader_event_count = 0;
static unsigned int pivas_uniform_event_count = 0;
static unsigned int pivas_gl_error_count = 0;
static unsigned int pivas_program_dump_count = 0;
static unsigned int pivas_uniform_call_count = 0;
static unsigned int pivas_tex_error_detail_count = 0;

static int pivas_should_log_verbose_gl(void) {
    return PIVAS_VERBOSE_GL_LOGGING != 0;
}

static int pivas_should_log_shader_diagnostics(void) {
    return PIVAS_SHADER_DIAGNOSTICS != 0 &&
           pivas_shader_event_count < PIVAS_SHADER_LOG_LIMIT;
}

static const char *pivas_gl_error_name(GLenum err) {
    switch (err) {
    case GL_NO_ERROR:
        return "GL_NO_ERROR";
    case GL_INVALID_ENUM:
        return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:
        return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION:
        return "GL_INVALID_OPERATION";
    case GL_OUT_OF_MEMORY:
        return "GL_OUT_OF_MEMORY";
    default:
        return "UNKNOWN";
    }
}

static const char *pivas_gl_type_name(GLenum type) {
    switch (type) {
    case GL_FLOAT:
        return "GL_FLOAT";
    case GL_FLOAT_VEC2:
        return "GL_FLOAT_VEC2";
    case GL_FLOAT_VEC3:
        return "GL_FLOAT_VEC3";
    case GL_FLOAT_VEC4:
        return "GL_FLOAT_VEC4";
    case GL_INT:
        return "GL_INT";
    case GL_INT_VEC2:
        return "GL_INT_VEC2";
    case GL_INT_VEC3:
        return "GL_INT_VEC3";
    case GL_INT_VEC4:
        return "GL_INT_VEC4";
    case GL_BOOL:
        return "GL_BOOL";
    case GL_BOOL_VEC2:
        return "GL_BOOL_VEC2";
    case GL_BOOL_VEC3:
        return "GL_BOOL_VEC3";
    case GL_BOOL_VEC4:
        return "GL_BOOL_VEC4";
    case GL_FLOAT_MAT2:
        return "GL_FLOAT_MAT2";
    case GL_FLOAT_MAT3:
        return "GL_FLOAT_MAT3";
    case GL_FLOAT_MAT4:
        return "GL_FLOAT_MAT4";
    case GL_SAMPLER_2D:
        return "GL_SAMPLER_2D";
    case GL_SAMPLER_CUBE:
        return "GL_SAMPLER_CUBE";
    default:
        return "UNKNOWN";
    }
}

void pivas_log_memory_snapshot(const char *tag) {
    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    struct mallinfo heap = mallinfo();

    int ret = sceKernelGetFreeMemorySize(&info);
    if (ret < 0) {
        l_error("[mem] %s sceKernelGetFreeMemorySize failed: 0x%08x heap_arena=%d heap_used=%d heap_free=%d heap_keep=%d",
                tag ? tag : "(null)", ret,
                heap.arena, heap.uordblks, heap.fordblks, heap.keepcost);
        return;
    }

    l_info("[mem] %s user=%d cdram=%d phycont=%d heap_arena=%d heap_used=%d heap_free=%d heap_keep=%d",
           tag ? tag : "(null)",
           info.size_user,
           info.size_cdram,
           info.size_phycont,
           heap.arena,
           heap.uordblks,
           heap.fordblks,
           heap.keepcost);
}

static void pivas_log_gl_error(const char *op) {
    GLenum err = glGetError();
    if (err == GL_NO_ERROR)
        return;

    if (pivas_gl_error_count < PIVAS_GL_ERROR_LOG_LIMIT) {
        l_error("[gl] %s glGetError=0x%04x %s",
                op ? op : "(null)", err, pivas_gl_error_name(err));
    }
    pivas_gl_error_count++;
}

static void pivas_log_texture_error(const char *op, GLenum err, unsigned int event,
                                    GLenum target, GLint level, GLint internalformat,
                                    GLsizei width, GLsizei height, GLint border,
                                    GLenum format, GLenum type, const GLvoid *pixels,
                                    GLint xoffset, GLint yoffset, int has_offset) {
    if (pivas_tex_error_detail_count < PIVAS_TEXTURE_ERROR_LOG_LIMIT) {
        if (has_offset) {
            l_error("[gl] %s failed #%u err=0x%04x %s target=0x%04x level=%d offset=%d,%d size=%dx%d format=0x%04x type=0x%04x pixels=%p",
                    op ? op : "(null)", event, err, pivas_gl_error_name(err),
                    target, level, xoffset, yoffset, width, height, format, type, pixels);
        } else {
            l_error("[gl] %s failed #%u err=0x%04x %s target=0x%04x level=%d internal=%d size=%dx%d border=%d format=0x%04x type=0x%04x pixels=%p",
                    op ? op : "(null)", event, err, pivas_gl_error_name(err),
                    target, level, internalformat, width, height, border, format, type, pixels);
        }
    }
    pivas_tex_error_detail_count++;
    pivas_gl_error_count++;
}

static int pivas_is_bgra_format(GLenum format) {
    return format == 0x80E1;
}

static int pivas_is_bgr_format(GLenum format) {
    return format == 0x80E0;
}

static GLenum pivas_normalize_texture_internalformat(GLint internalformat,
                                                     GLenum format) {
    if (internalformat == 1)
        return GL_LUMINANCE;
    if (internalformat == 2)
        return GL_LUMINANCE_ALPHA;
    if (internalformat == 3)
        return GL_RGB;
    if (internalformat == 4)
        return GL_RGBA;
    if (pivas_is_bgra_format((GLenum)internalformat))
        return GL_RGBA;
    if (pivas_is_bgr_format((GLenum)internalformat))
        return GL_RGB;
    if (pivas_is_bgra_format(format))
        return GL_RGBA;
    if (pivas_is_bgr_format(format))
        return GL_RGB;
    return (GLenum)internalformat;
}

static GLvoid *pivas_swizzle_bgr_upload(const GLvoid *pixels, GLsizei width,
                                        GLsizei height, GLenum format,
                                        GLenum type) {
    if (!pixels || width <= 0 || height <= 0 || type != GL_UNSIGNED_BYTE)
        return NULL;

    int channels = 0;
    if (pivas_is_bgra_format(format))
        channels = 4;
    else if (pivas_is_bgr_format(format))
        channels = 3;
    else
        return NULL;

    size_t byte_count = (size_t)width * (size_t)height * (size_t)channels;
    uint8_t *converted = (uint8_t *)malloc(byte_count);
    if (!converted)
        return NULL;

    const uint8_t *src = (const uint8_t *)pixels;
    for (size_t i = 0; i < byte_count; i += (size_t)channels) {
        converted[i + 0] = src[i + 2];
        converted[i + 1] = src[i + 1];
        converted[i + 2] = src[i + 0];
        if (channels == 4)
            converted[i + 3] = src[i + 3];
    }

    return converted;
}

static GLint pivas_unpack_alignment = 4;

void glPixelStorei_soloader(GLenum pname, GLint param) {
    /*
     * GL_UNPACK_ALIGNMENT is tracked for the RGB-expansion stride math, not
     * forwarded: vitaGL textures are tightly packed, and a FILE_LOG vitaGL
     * build appends an INVALID_ENUM line to vitaGL.log per call (sdl-gpu
     * sets it around every upload). The vendored vitaGL also accepts it
     * silently (textures.c).
     */
    if (pname == GL_UNPACK_ALIGNMENT) {
        if (param > 0)
            pivas_unpack_alignment = param;
        return;
    }
    glPixelStorei(pname, param);
}

/*
 * GXM has no 24bpp texture format; RGB888 uploads through vitaGL fail with
 * GL_INVALID_ENUM or sample diagonally sheared. Expand GL_RGB/GL_UNSIGNED_BYTE
 * uploads to tightly-packed RGBA (alpha=255); the texture is created as
 * GL_RGBA. src_row_stride covers the caller's GL_UNPACK_ALIGNMENT row padding.
 */
static GLvoid *pivas_expand_rgb_to_rgba(const GLvoid *pixels, GLsizei width,
                                        GLsizei height, size_t src_row_stride) {
    if (!pixels || width <= 0 || height <= 0)
        return NULL;

    uint8_t *converted = (uint8_t *)malloc((size_t)width * (size_t)height * 4);
    if (!converted)
        return NULL;

    const uint8_t *src = (const uint8_t *)pixels;
    for (GLsizei y = 0; y < height; y++) {
        const uint8_t *s = src + (size_t)y * src_row_stride;
        uint8_t *d       = converted + (size_t)y * (size_t)width * 4;
        for (GLsizei x = 0; x < width; x++) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 0xFF;
            s += 3;
            d += 4;
        }
    }
    return converted;
}

static size_t pivas_rgb_src_stride(GLsizei width) {
    size_t row = (size_t)width * 3;
    size_t a   = (size_t)(pivas_unpack_alignment > 0 ? pivas_unpack_alignment : 1);
    return (row + a - 1) & ~(a - 1);
}

static GLenum pivas_drain_gl_errors(const char *op) {
	GLenum first = GL_NO_ERROR;
	int should_log = pivas_should_log_verbose_gl() || PIVAS_SHADER_DIAGNOSTICS;

	for (int i = 0; i < 8; ++i) {
		GLenum err = glGetError();
        if (err == GL_NO_ERROR)
            break;

        if (first == GL_NO_ERROR)
            first = err;

		if (should_log && pivas_gl_error_count < PIVAS_GL_ERROR_LOG_LIMIT) {
			l_warn("[gl] %s stale_error=0x%04x %s",
			       op ? op : "(null)", err, pivas_gl_error_name(err));
		}
        pivas_gl_error_count++;
    }

    return first;
}

static int pivas_should_trace_uniform_location(GLint location) {
    return PIVAS_SHADER_DIAGNOSTICS &&
           pivas_uniform_call_count < PIVAS_UNIFORM_LOG_LIMIT &&
           (location < 0 || location > 1024 || pivas_should_log_verbose_gl());
}

static GLint pivas_current_program(void) {
    GLint current = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current);
    return current;
}

static int pivas_trace_uniform_begin(const char *op, GLint location) {
    if (!pivas_should_trace_uniform_location(location))
        return 0;

    GLint current = pivas_current_program();
    l_info("[shader] uniform-call op=%s current=%d loc=%d semantic=%s",
           op ? op : "(null)", current, location,
#ifdef USE_GLSL_SHADERS
           pivas_semantic_mode_name
#else
           "none"
#endif
           );
    pivas_drain_gl_errors(op);
    return 1;
}

static void pivas_trace_uniform_end(const char *op, GLint location, int traced) {
	if (!PIVAS_SHADER_DIAGNOSTICS && !pivas_should_log_verbose_gl())
		return;

	GLenum err = glGetError();

    if (err == GL_NO_ERROR) {
        if (traced && pivas_uniform_call_count < PIVAS_UNIFORM_LOG_LIMIT) {
            l_info("[shader] uniform-result op=%s loc=%d err=0x%04x %s",
                   op ? op : "(null)", location, err, pivas_gl_error_name(err));
            pivas_uniform_call_count++;
        }
        return;
    }

    if (pivas_uniform_call_count < PIVAS_UNIFORM_LOG_LIMIT) {
        l_error("[shader] uniform-result op=%s current=%d loc=%d err=0x%04x %s",
                op ? op : "(null)", pivas_current_program(), location, err,
                pivas_gl_error_name(err));
        pivas_uniform_call_count++;
    }
}

static void pivas_log_shader_compile_status(GLuint shader, const char *stage) {
    if (!pivas_should_log_shader_diagnostics())
        return;

    GLint status = GL_FALSE;
    GLint info_len = 0;
    GLsizei written = 0;
    char *info = NULL;

    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);

    if (info_len > 1) {
        info = malloc((size_t) info_len);
        if (info) {
            info[0] = '\0';
            glGetShaderInfoLog(shader, info_len, &written, info);
        }
    }

    l_info("[shader] %s shader=%u compile=%s info_len=%d info=%s",
           stage ? stage : "compile",
           shader,
           status == GL_TRUE ? "ok" : "fail",
           info_len,
           (info && info[0]) ? info : "(empty)");

    if (info)
        free(info);
    pivas_shader_event_count++;
}

static void pivas_log_program_interface(GLuint program, const char *stage) {
    if (!PIVAS_SHADER_DIAGNOSTICS || pivas_program_dump_count++ >= 24)
        return;

    GLint uniforms = 0;
    GLint attributes = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniforms);
    pivas_log_gl_error("glGetProgramiv ACTIVE_UNIFORMS");
    glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &attributes);
    pivas_log_gl_error("glGetProgramiv ACTIVE_ATTRIBUTES");

    l_info("[shader] interface stage=%s program=%u semantic=%s active_uniforms=%d active_attributes=%d",
           stage ? stage : "(null)", program,
#ifdef USE_GLSL_SHADERS
           pivas_semantic_mode_name,
#else
           "none",
#endif
           uniforms, attributes);

    int uniform_limit = uniforms < 24 ? uniforms : 24;
    for (int i = 0; i < uniform_limit; ++i) {
        char name[96] = {0};
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(program, (GLuint)i, sizeof(name), &length, &size,
                           &type, name);
        pivas_log_gl_error("glGetActiveUniform");
        GLint loc = name[0] ? glGetUniformLocation(program, name) : -1;
        pivas_log_gl_error("glGetUniformLocation interface");
        l_info("[shader] active-uniform program=%u index=%d name=%s size=%d type=0x%04x/%s loc=%d",
               program, i, name[0] ? name : "(empty)", size, type,
               pivas_gl_type_name(type), loc);
    }

    int attribute_limit = attributes < 16 ? attributes : 16;
    for (int i = 0; i < attribute_limit; ++i) {
        char name[96] = {0};
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveAttrib(program, (GLuint)i, sizeof(name), &length, &size,
                          &type, name);
        pivas_log_gl_error("glGetActiveAttrib");
        GLint loc = name[0] ? glGetAttribLocation(program, name) : -1;
        pivas_log_gl_error("glGetAttribLocation interface");
        l_info("[shader] active-attribute program=%u index=%d name=%s size=%d type=0x%04x/%s loc=%d",
               program, i, name[0] ? name : "(empty)", size, type,
               pivas_gl_type_name(type), loc);
    }

    static const char *uniform_queries[] = {
        "gpu_ModelViewProjectionMatrix",
        "tex",
        "color",
        "modificationType",
        "replaceSrcColor",
        "replaceDstColor",
        "multiplyAlpha",
        "faceAscender",
        "maxy",
        "height",
        NULL
    };
    for (int i = 0; uniform_queries[i]; ++i) {
        GLint loc = glGetUniformLocation(program, uniform_queries[i]);
        pivas_log_gl_error("glGetUniformLocation query");
        if (loc >= 0 || strcmp(uniform_queries[i], "gpu_ModelViewProjectionMatrix") == 0) {
            l_info("[shader] query-uniform program=%u name=%s loc=%d",
                   program, uniform_queries[i], loc);
        }
    }

    static const char *attribute_queries[] = {
        "gpu_Vertex",
        "gpu_TexCoord",
        "gpu_Color",
        NULL
    };
    for (int i = 0; attribute_queries[i]; ++i) {
        GLint loc = glGetAttribLocation(program, attribute_queries[i]);
        pivas_log_gl_error("glGetAttribLocation query");
        l_info("[shader] query-attribute program=%u name=%s loc=%d",
               program, attribute_queries[i], loc);
    }
}

static void pivas_log_program_link_status(GLuint program, const char *stage) {
    if (!pivas_should_log_shader_diagnostics())
        return;

    GLint status = GL_FALSE;
    GLint info_len = 0;
    GLsizei written = 0;
    char *info = NULL;

    glGetProgramiv(program, GL_LINK_STATUS, &status);
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &info_len);

    if (info_len > 1) {
        info = malloc((size_t) info_len);
        if (info) {
            info[0] = '\0';
            glGetProgramInfoLog(program, info_len, &written, info);
        }
    }

    l_info("[shader] %s program=%u link=%s info_len=%d info=%s",
           stage ? stage : "link",
           program,
           status == GL_TRUE ? "ok" : "fail",
           info_len,
           (info && info[0]) ? info : "(empty)");

    if (info)
        free(info);
    pivas_shader_event_count++;

#if PIVAS_SHADER_INTERFACE_DUMP
    if (status == GL_TRUE)
        pivas_log_program_interface(program, stage);
#endif
}

static GLuint pivas_compile_probe_shader(GLenum type, const char *source,
                                         const char *label) {
    GLuint shader = glCreateShader(type);
    if (!shader) {
        pivas_log_gl_error("probe glCreateShader");
        return 0;
    }

    glShaderSource(shader, 1, &source, NULL);
    pivas_log_gl_error("probe glShaderSource");
    glCompileShader(shader);
    pivas_log_shader_compile_status(shader, label);
    pivas_log_gl_error("probe glCompileShader");
    return shader;
}

static void pivas_run_shader_probe(const char *stage) {
#if defined(USE_GLSL_SHADERS) && PIVAS_SHADER_PROBE
    static const char *probe_vert =
        "attribute vec3 gpu_Vertex;\n"
        "attribute vec2 gpu_TexCoord;\n"
        "attribute vec4 gpu_Color;\n"
        "uniform mat4 gpu_ModelViewProjectionMatrix;\n"
        "varying vec2 probeTexCoord;\n"
        "varying vec4 probeColor;\n"
        "void main() {\n"
        "    probeTexCoord = gpu_TexCoord;\n"
        "    probeColor = gpu_Color;\n"
        "    gl_Position = gpu_ModelViewProjectionMatrix * vec4(gpu_Vertex, 1.0);\n"
        "}\n";
    static const char *probe_frag =
        "precision mediump float;\n"
        "uniform sampler2D tex;\n"
        "uniform vec4 color;\n"
        "varying vec2 probeTexCoord;\n"
        "varying vec4 probeColor;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(tex, probeTexCoord) * probeColor * color;\n"
        "}\n";
    static const GLfloat identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    if (!PIVAS_SHADER_DIAGNOSTICS)
        return;

    l_info("[shader-probe] begin stage=%s semantic=%s",
           stage ? stage : "(null)", pivas_semantic_mode_name);
    pivas_drain_gl_errors("shader-probe begin");

    GLint previous_program = pivas_current_program();
    GLuint vert = pivas_compile_probe_shader(GL_VERTEX_SHADER, probe_vert,
                                             "probe-vertex");
    GLuint frag = pivas_compile_probe_shader(GL_FRAGMENT_SHADER, probe_frag,
                                             "probe-fragment");
    GLuint program = glCreateProgram();
    pivas_log_gl_error("probe glCreateProgram");

    if (program && vert && frag) {
        glAttachShader(program, vert);
        pivas_log_gl_error("probe glAttachShader vert");
        glAttachShader(program, frag);
        pivas_log_gl_error("probe glAttachShader frag");
        glLinkProgram(program);
        pivas_log_program_link_status(program, "probe-link");

        GLint mvp = glGetUniformLocation(program, "gpu_ModelViewProjectionMatrix");
        GLint tex = glGetUniformLocation(program, "tex");
        GLint color = glGetUniformLocation(program, "color");
        GLint vertex = glGetAttribLocation(program, "gpu_Vertex");
        GLint uv = glGetAttribLocation(program, "gpu_TexCoord");
        GLint attr_color = glGetAttribLocation(program, "gpu_Color");
        pivas_log_gl_error("probe explicit queries");
        l_info("[shader-probe] query program=%u mvp=%d tex=%d color=%d vertex=%d uv=%d attr_color=%d",
               program, mvp, tex, color, vertex, uv, attr_color);

        pivas_drain_gl_errors("shader-probe before use");
        glUseProgram(program);
        pivas_log_gl_error("probe glUseProgram");
        pivas_drain_gl_errors("shader-probe before matrix upload");
        glUniformMatrix4fv(mvp, 1, GL_FALSE, identity);
        GLenum matrix_err = glGetError();
        l_info("[shader-probe] matrix-upload program=%u loc=%d err=0x%04x %s",
               program, mvp, matrix_err, pivas_gl_error_name(matrix_err));

        pivas_drain_gl_errors("shader-probe before scalar uploads");
        if (tex >= 0)
            glUniform1i(tex, 0);
        GLenum tex_err = glGetError();
        if (color >= 0) {
            const GLfloat rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            glUniform4fv(color, 1, rgba);
        }
        GLenum color_err = glGetError();
        l_info("[shader-probe] scalar-upload program=%u tex_loc=%d tex_err=0x%04x/%s color_loc=%d color_err=0x%04x/%s",
               program, tex, tex_err, pivas_gl_error_name(tex_err),
               color, color_err, pivas_gl_error_name(color_err));

        glUseProgram((GLuint)previous_program);
        pivas_log_gl_error("probe restore glUseProgram");
    }

    if (program)
        glDeleteProgram(program);
    if (vert)
        glDeleteShader(vert);
    if (frag)
        glDeleteShader(frag);

    pivas_drain_gl_errors("shader-probe end");
    l_info("[shader-probe] end stage=%s", stage ? stage : "(null)");
#else
    (void)stage;
#endif
}

void glClear_soloader(GLbitfield mask) {
    if (pivas_should_log_verbose_gl() && pivas_clear_event_count++ < 16)
        l_info("[gl] glClear mask=0x%04x", mask);

    pivas_drain_gl_errors("before glClear");
    glClear(mask);
    pivas_log_gl_error("glClear");
}

void glClearColor_soloader(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    if (pivas_should_log_verbose_gl() && pivas_clear_event_count++ < 16)
        l_info("[gl] glClearColor rgba=%.3f,%.3f,%.3f,%.3f", red, green, blue, alpha);

    pivas_drain_gl_errors("before glClearColor");
    glClearColor(red, green, blue, alpha);
    pivas_log_gl_error("glClearColor");
}

void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, const GLvoid *pixels) {
    unsigned int event = pivas_tex_event_count++;
    GLenum upload_internalformat =
        pivas_normalize_texture_internalformat(internalformat, format);
    GLenum upload_format = format;
    GLvoid *converted_pixels = pivas_swizzle_bgr_upload(pixels, width, height,
                                                        format, type);
    if (pivas_is_bgra_format(format))
        upload_format = GL_RGBA;
    else if (pivas_is_bgr_format(format))
        upload_format = GL_RGB;

    if (upload_format == GL_RGB && type == GL_UNSIGNED_BYTE) {
        const GLvoid *rgb_src = converted_pixels ? converted_pixels : pixels;
        // swizzled buffers are tightly packed; original ones obey UNPACK_ALIGNMENT
        size_t stride = converted_pixels ? (size_t)width * 3
                                         : pivas_rgb_src_stride(width);
        GLvoid *rgba = pivas_expand_rgb_to_rgba(rgb_src, width, height, stride);
        if (rgba || !pixels) {
            if (converted_pixels)
                free(converted_pixels);
            converted_pixels      = rgba;
            upload_format         = GL_RGBA;
            upload_internalformat = GL_RGBA;
        }
    }

    int should_log = pivas_should_log_verbose_gl() ||
                     (PIVAS_TEXTURE_UPLOAD_DIAGNOSTICS &&
                      (event < 16 || width >= 1024 || height >= 1024));

    if (should_log) {
        l_info("[gl] glTexImage2D #%u target=0x%04x level=%d internal=%d->0x%04x size=%dx%d format=0x%04x->0x%04x type=0x%04x pixels=%p converted=%p",
               event, target, level, internalformat, upload_internalformat,
               width, height, format, upload_format, type, pixels,
               converted_pixels);
        pivas_log_memory_snapshot("before glTexImage2D");
    }

    pivas_drain_gl_errors("before glTexImage2D");

    glTexImage2D(target, level, upload_internalformat, width, height, border,
                 upload_format, type, converted_pixels ? converted_pixels : pixels);

    if (should_log)
        pivas_log_memory_snapshot("after glTexImage2D");
    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
        pivas_log_texture_error("glTexImage2D", err, event, target, level,
                                upload_internalformat, width, height, border,
                                upload_format, type,
                                converted_pixels ? converted_pixels : pixels,
                                0, 0, 0);

    if (converted_pixels)
        free(converted_pixels);
}

void glTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset,
                              GLint yoffset, GLsizei width, GLsizei height,
                              GLenum format, GLenum type, const GLvoid *pixels) {
    unsigned int event = pivas_tex_event_count++;
    GLenum upload_format = format;
    GLvoid *converted_pixels = pivas_swizzle_bgr_upload(pixels, width, height,
                                                        format, type);
    if (pivas_is_bgra_format(format))
        upload_format = GL_RGBA;
    else if (pivas_is_bgr_format(format))
        upload_format = GL_RGB;

    if (upload_format == GL_RGB && type == GL_UNSIGNED_BYTE) {
        const GLvoid *rgb_src = converted_pixels ? converted_pixels : pixels;
        size_t stride = converted_pixels ? (size_t)width * 3
                                         : pivas_rgb_src_stride(width);
        GLvoid *rgba = pivas_expand_rgb_to_rgba(rgb_src, width, height, stride);
        if (rgba) {
            if (converted_pixels)
                free(converted_pixels);
            converted_pixels = rgba;
            upload_format    = GL_RGBA;
        }
    }

    int should_log = pivas_should_log_verbose_gl() ||
                     (PIVAS_TEXTURE_UPLOAD_DIAGNOSTICS &&
                      (event < 16 || width >= 1024 || height >= 1024));

    if (should_log) {
        l_info("[gl] glTexSubImage2D #%u target=0x%04x level=%d offset=%d,%d size=%dx%d format=0x%04x->0x%04x type=0x%04x pixels=%p converted=%p",
               event, target, level, xoffset, yoffset, width, height,
               format, upload_format, type, pixels, converted_pixels);
        pivas_log_memory_snapshot("before glTexSubImage2D");
    }

    pivas_drain_gl_errors("before glTexSubImage2D");

    glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                    upload_format, type,
                    converted_pixels ? converted_pixels : pixels);

    if (should_log)
        pivas_log_memory_snapshot("after glTexSubImage2D");
    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
        pivas_log_texture_error("glTexSubImage2D", err, event, target, level,
                                -1, width, height, 0, upload_format, type,
                                converted_pixels ? converted_pixels : pixels,
                                xoffset, yoffset, 1);

    if (converted_pixels)
        free(converted_pixels);
}

void glUseProgram_soloader(GLuint program) {
    if ((pivas_should_log_verbose_gl() || PIVAS_SHADER_DIAGNOSTICS) && pivas_program_event_count++ < 128)
        l_info("[gl] glUseProgram program=%u", program);

    GLenum stale = pivas_drain_gl_errors("before glUseProgram");
    glUseProgram(program);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        if (pivas_gl_error_count < PIVAS_GL_ERROR_LOG_LIMIT) {
            l_error("[gl] glUseProgram failed program=%u err=0x%04x %s",
                    program, err, pivas_gl_error_name(err));
        }
        pivas_gl_error_count++;
    } else if (stale != GL_NO_ERROR && pivas_gl_error_count < PIVAS_GL_ERROR_LOG_LIMIT) {
        l_warn("[gl] glUseProgram had stale pre-error program=%u stale=0x%04x %s",
               program, stale, pivas_gl_error_name(stale));
    }
}

GLint glGetUniformLocation_soloader(GLuint program, const GLchar *name) {
    pivas_drain_gl_errors("before glGetUniformLocation");
    GLint loc = glGetUniformLocation(program, name);
    GLenum err = glGetError();

    if (PIVAS_SHADER_DIAGNOSTICS && pivas_uniform_event_count < PIVAS_UNIFORM_LOG_LIMIT &&
        name && (strcmp(name, "gpu_ModelViewProjectionMatrix") == 0 ||
                 strcmp(name, "tex") == 0 ||
                 strcmp(name, "modificationType") == 0 ||
                 strcmp(name, "replaceSrcColor") == 0 ||
                 strcmp(name, "replaceDstColor") == 0 ||
                 strcmp(name, "multiplyAlpha") == 0 ||
                 strcmp(name, "color") == 0 ||
                 strcmp(name, "faceAscender") == 0 ||
                 strcmp(name, "maxy") == 0 ||
                 strcmp(name, "height") == 0)) {
        l_info("[shader] glGetUniformLocation program=%u current=%d name=%s loc=%d err=0x%04x %s semantic=%s",
               program, pivas_current_program(), name, loc, err,
               pivas_gl_error_name(err),
#ifdef USE_GLSL_SHADERS
               pivas_semantic_mode_name
#else
               "none"
#endif
               );
        pivas_uniform_event_count++;
    }

    return loc;
}

GLint glGetAttribLocation_soloader(GLuint program, const GLchar *name) {
    pivas_drain_gl_errors("before glGetAttribLocation");
    GLint loc = glGetAttribLocation(program, name);
    GLenum err = glGetError();

    if (PIVAS_SHADER_DIAGNOSTICS && pivas_uniform_event_count < PIVAS_UNIFORM_LOG_LIMIT &&
        name && (strcmp(name, "gpu_Vertex") == 0 ||
                 strcmp(name, "gpu_TexCoord") == 0 ||
                 strcmp(name, "gpu_Color") == 0)) {
        l_info("[shader] glGetAttribLocation program=%u current=%d name=%s loc=%d err=0x%04x %s semantic=%s",
               program, pivas_current_program(), name, loc, err,
               pivas_gl_error_name(err),
#ifdef USE_GLSL_SHADERS
               pivas_semantic_mode_name
#else
               "none"
#endif
               );
        pivas_uniform_event_count++;
    }

    return loc;
}

void glUniform1i_soloader(GLint location, GLint v0) {
    int traced = pivas_trace_uniform_begin("glUniform1i", location);
    glUniform1i(location, v0);
    pivas_trace_uniform_end("glUniform1i", location, traced);
}

void glUniform1f_soloader(GLint location, GLfloat v0) {
    int traced = pivas_trace_uniform_begin("glUniform1f", location);
    glUniform1f(location, v0);
    pivas_trace_uniform_end("glUniform1f", location, traced);
}

void glUniform1fv_soloader(GLint location, GLsizei count, const GLfloat *value) {
    int traced = pivas_trace_uniform_begin("glUniform1fv", location);
    glUniform1fv(location, count, value);
    pivas_trace_uniform_end("glUniform1fv", location, traced);
}

void glUniform2fv_soloader(GLint location, GLsizei count, const GLfloat *value) {
    int traced = pivas_trace_uniform_begin("glUniform2fv", location);
    glUniform2fv(location, count, value);
    pivas_trace_uniform_end("glUniform2fv", location, traced);
}

void glUniform4fv_soloader(GLint location, GLsizei count, const GLfloat *value) {
    int traced = pivas_trace_uniform_begin("glUniform4fv", location);
    glUniform4fv(location, count, value);
    pivas_trace_uniform_end("glUniform4fv", location, traced);
}

void glUniformMatrix4fv_soloader(GLint location, GLsizei count,
                                 GLboolean transpose, const GLfloat *value) {
    if (PIVAS_SHADER_DIAGNOSTICS && pivas_uniform_event_count < PIVAS_UNIFORM_LOG_LIMIT &&
        (location < 0 || location > 1024)) {
        l_warn("[shader] glUniformMatrix4fv suspicious location=%d count=%d transpose=%d value=%p m0=%.4f m5=%.4f m10=%.4f m15=%.4f",
               location, count, transpose, value,
               value ? value[0] : 0.0f,
               value ? value[5] : 0.0f,
               value ? value[10] : 0.0f,
               value ? value[15] : 0.0f);
        pivas_uniform_event_count++;
    }

    int traced = pivas_trace_uniform_begin("glUniformMatrix4fv", location);
    glUniformMatrix4fv(location, count, transpose, value);
    pivas_trace_uniform_end("glUniformMatrix4fv", location, traced);
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glShaderSource<%p>(shader: %i, count: %i, string: %p, length: %p)\n", __builtin_return_address(0), shader, count, string, _length);
#endif
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            total_length += strlen(string[i]);
        } else {
            total_length += _length[i];
        }
    }

    char * str = malloc(total_length+1);
    size_t l = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            memcpy(str + l, string[i], strlen(string[i]));
            l += strlen(string[i]);
        } else {
            memcpy(str + l, string[i], _length[i]);
            l += _length[i];
        }
    }
    str[total_length] = '\0';

    load_shader(shader, str, total_length);

    free(str);
}

void glCompileShader_soloader(GLuint shader) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glCompileShader<%p>(shader: %i)\n", __builtin_return_address(0), shader);
#endif
#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        glCompileShader(shader);
        pivas_log_shader_compile_status(shader, "compile");
        pivas_log_gl_error("glCompileShader");
#ifdef DUMP_COMPILED_SHADERS
        void *bin = vglMalloc(32 * 1024);
        GLsizei len;
        vglGetShaderBinary(shader, 32 * 1024, &len, bin);
        file_save(next_shader_fname, bin, len);
        vglFree(bin);
#endif
    }
    skip_next_compile = GL_FALSE;
#endif
}

void glLinkProgram_soloader(GLuint program) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glLinkProgram<%p>(program: %i)\n", __builtin_return_address(0), program);
#endif
    if (PIVAS_SHADER_DIAGNOSTICS && pivas_shader_event_count < PIVAS_SHADER_LOG_LIMIT) {
        l_info("[shader] link-call program=%u semantic=%s", program,
#ifdef USE_GLSL_SHADERS
               pivas_semantic_mode_name
#else
               "none"
#endif
               );
        pivas_shader_event_count++;
    }
    glLinkProgram(program);
    pivas_log_gl_error("glLinkProgram");
}

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else {
        glShaderSource(shader, 1, &string, &length);
        strcpy(next_shader_fname, gxp_path);
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    glShaderSource(shader, 1, &string, &length);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    char cg_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);
    snprintf(cg_path, sizeof(cg_path), DATA_PATH"cg/%s.cg", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else if (file_exists(cg_path)) {
        char *buffer;
        size_t size;

        file_load(cg_path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);
        strcpy(next_shader_fname, gxp_path);

        free(buffer);
        skip_next_compile = GL_FALSE;
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        // Dumping the untranslated GLSL source is disabled in release builds.
        //char glsl_path[256];
        //snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        //file_mkpath(glsl_path, 0777);
        //file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }

        skip_next_compile = GL_FALSE;
    }

    free(sha_name);
}
#elif defined(USE_CG_SHADERS) || defined(USE_GXP_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char path[256];
#ifdef USE_CG_SHADERS
    snprintf(path, sizeof(path), DATA_PATH"cg/%s.cg", sha_name);
#else
    snprintf(path, sizeof(path), DATA_PATH"gxp/%s.gxp", sha_name);
#endif

    if (file_exists(path)) {
#ifdef USE_CG_SHADERS
        char *buffer;
        size_t size;

        file_load(path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);

        free(buffer);
#else
        uint8_t *buffer;
        size_t size;

        file_load(path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
#endif
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        // Dumping the untranslated GLSL source is disabled in release builds.
        //char glsl_path[256];
        //snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        //file_mkpath(glsl_path, 0777);
        //file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#else
#error "Define one of (USE_GLSL_SHADERS, USE_CG_SHADERS, USE_GXP_SHADERS)"
#endif
