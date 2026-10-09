#include "shader_cache.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#ifndef ONS_SHADER_ABI
#error The shader corpus and driver fingerprint must be supplied by CMake
#endif
#define MAX_BINARY (1024u * 1024u)
#define HEADER_WORDS 8

static uint64_t source_hash(const char *source) {
    uint64_t hash = UINT64_C(14695981039346656037);
    while (*source) { hash ^= (unsigned char)*source++; hash *= UINT64_C(1099511628211); }
    return hash;
}

static void shader_path(char *path, size_t size, const char *root, GLenum type, uint64_t hash) {
    snprintf(path, size, "%s/" ONS_SHADER_ABI "/%u-%016llx.bin", root,
             (unsigned)type, (unsigned long long)hash);
}

int ons_vita_shader_load(GLuint shader, GLenum type, const char *source) {
    char path[256];
    uint64_t hash = source_hash(source);
    shader_path(path, sizeof(path), "app0:shaders", type, hash);
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Missing bundled native shader: %s\n", path);
        return 0;
    }
    uint32_t header[HEADER_WORDS];
    if (fread(header, sizeof(header), 1, file) != 1 ||
        header[0] != 0x31534756 || header[1] != 1 || header[2] != type ||
        header[3] != (uint32_t)hash || header[4] != (uint32_t)(hash >> 32) ||
        header[5] < 8 || header[5] > MAX_BINARY || header[7] != 0) {
        fclose(file);
        fprintf(stderr, "Invalid bundled shader header: %s\n", path);
        return 0;
    }
    uint32_t *binary = malloc(header[5]);
    if (!binary) { fclose(file); return 0; }
    int valid = fread(binary, 1, header[5], file) == header[5] && fgetc(file) == EOF;
    fclose(file);
    valid = valid && crc32(0, (const Bytef *)binary, header[5]) == header[6];
    /* vitaGL's binary extension includes a matrix-parameter index table.
     * Keep it: raw GXP alone loses the GLSL matrix transpose metadata. */
    if (valid) {
        uint32_t count = binary[0];
        valid = count < header[5] / 4 - 1;
        if (valid) {
            const SceGxmProgram *program = (const SceGxmProgram *)(binary + count + 1);
            unsigned available = header[5] - (count + 1) * 4;
            valid = available >= 32 && !memcmp(program, "GXP", 3);
            if (valid) valid = sceGxmProgramGetSize(program) == available && sceGxmProgramCheck(program) == 0;
            if (valid) {
                unsigned parameters = sceGxmProgramGetParameterCount(program);
                for (unsigned i = 0; i < count; ++i)
                    if (binary[i + 1] >= parameters) valid = 0;
            }
        }
    }
    if (valid) glShaderBinary(1, &shader, 0, binary, header[5]);
    free(binary);
    if (!valid) fprintf(stderr, "Invalid bundled native shader: %s\n", path);
    return valid;
}

#ifdef ONS_VITA_EXPORT_SHADERS
static unsigned export_bound;
static int capturing;
int __real_sceGxmShaderPatcherRegisterProgram(SceGxmShaderPatcher *, const SceGxmProgram *, SceGxmShaderPatcherId *);
int __wrap_sceGxmShaderPatcherRegisterProgram(SceGxmShaderPatcher *patcher,
    const SceGxmProgram *program, SceGxmShaderPatcherId *id) {
    int result = __real_sceGxmShaderPatcherRegisterProgram(patcher, program, id);
    if (capturing && !result) {
        unsigned size = sceGxmProgramGetSize(program), count = sceGxmProgramGetParameterCount(program);
        if (count < MAX_BINARY / 4 && size <= MAX_BINARY - 4 * (count + 1))
            export_bound = size + 4 * (count + 1);
    }
    return result;
}
void ons_vita_shader_export_begin(void) { export_bound = 0; capturing = 1; }
int ons_vita_shader_export_end(GLuint shader, GLenum type, const char *source) {
    capturing = 0;
    if (!export_bound) return 0;
    void *binary = malloc(export_bound);
    if (!binary) return 0;
    GLsizei size = 0;
    /* Reviewed SDK ignores bufSize: bound it from the registered GXP's size
     * plus one table entry per parameter before calling its serializer. */
    vglGetShaderBinary(shader, export_bound, &size, binary);
    if (size <= 0 || (unsigned)size > export_bound) { free(binary); return 0; }
    uint64_t hash = source_hash(source);
    uint32_t header[HEADER_WORDS] = {0x31534756, 1, type, (uint32_t)hash,
        (uint32_t)(hash >> 32), (uint32_t)size, crc32(0, binary, size), 0};
    mkdir("ux0:data/umineko-native/shaders", 0777);
    mkdir("ux0:data/umineko-native/shaders/" ONS_SHADER_ABI, 0777);
    char path[256];
    shader_path(path, sizeof(path), "ux0:data/umineko-native/shaders", type, hash);
    FILE *file = fopen(path, "wb");
    int ok = file != NULL;
    if (ok) ok = fwrite(header, sizeof(header), 1, file) == 1 && fwrite(binary, 1, size, file) == (size_t)size;
    if (file && fclose(file)) ok = 0;
    free(binary);
    fprintf(stdout, "Native shader export %s: %s (%d bytes)\n", ok ? "ready" : "FAILED", path, size);
    return ok;
}
#endif
