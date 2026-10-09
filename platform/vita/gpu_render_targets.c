#include "gpu_render_targets.h"
#include <vitaGL.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Reviewed vitaGL framebuffer prefix. GL names are pointers to its static FBO
 * slots. gpu_translator_archive.py pins the two SDK objects that define/use
 * this layout; this is not a portable OpenGL assumption. Never free a texture,
 * depth surface, GL framebuffer, or engine GPU_Target to reclaim a GXM target.
 */
typedef struct {
    GLboolean active, is_float, is_depth_hidden;
    SceGxmRenderTarget *target;
    SceGxmColorSurface colorbuffer;
    SceGxmDepthStencilSurface depthbuffer;
    SceGxmDepthStencilSurface *depthbuffer_ptr;
    int width, height, stride;
    void *data;
} FramebufferPrefix;
#ifdef VITA
_Static_assert(offsetof(FramebufferPrefix, target) == 4, "vitaGL framebuffer ABI");
_Static_assert(offsetof(FramebufferPrefix, colorbuffer) == 8, "vitaGL color surface ABI");
_Static_assert(offsetof(FramebufferPrefix, data) == 92, "vitaGL storage pointer ABI");
#endif
extern SceGxmContext *gxm_context;
extern FramebufferPrefix *active_write_fb;
extern FramebufferPrefix *in_use_framebuffer;
extern uint8_t dirty_framebuffer;
/* Exposed only in our local SDK archive, checked against the pinned gxm.o. */
extern GLboolean needs_end_scene, needs_scene_reset;
extern void sceneEnd(void);
extern GLboolean is_rendering_display;
extern float *scissor_test_vertices;
extern float x_port, x_scale, y_port, y_scale, z_port, z_scale;
extern int DISPLAY_HEIGHT;
extern void sceGxmSetViewport_sfp(SceGxmContext *, float, float, float, float, float, float);

void __real_glViewport(GLint, GLint, GLsizei, GLsizei);
void __wrap_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    __real_glViewport(x, y, width, height);
    if (width < 0 || height < 0 || !((width | height) & 1)) return;
    /* The pinned SDK uses integer >>1 before conversion to float. An odd
     * target (including the 611px game canvas) loses a row/column and breaks
     * texel-aligned textbox joins. Retain its GL state updates, then install
     * the exact half extents through the SDK's verified softfp bridge. */
    x_scale = width * .5f;
    x_port = x + x_scale;
    y_scale = height * .5f;
    y_port = y + y_scale;
    if (is_rendering_display) {
        y_port = DISPLAY_HEIGHT - y - y_scale;
        y_scale = -y_scale;
    }
    sceGxmSetViewport_sfp(gxm_context, x_port, x_scale, y_port, y_scale, z_port, z_scale);
}

void __real_vector4f_convert_to_local_space(float *, int, int, int, int);
void __wrap_vector4f_convert_to_local_space(float *out, int x, int y, int w, int h) {
    __real_vector4f_convert_to_local_space(out, x, y, w, h);
    /* The pinned SDK uses a positive viewport Y scale for texture targets,
     * but builds its scissor mask with the display's negative-Y convention.
     * Consequently the mask and the hardware region clip cover opposite rows;
     * a textbox near the bottom can be rejected completely. Correct only the
     * scissor mask, preserving the driver's clear and framebuffer-blit paths. */
    if (!is_rendering_display && out == scissor_test_vertices) {
        out[2] = -out[2];
        out[3] = -out[3];
    }
}

enum { FRAMEBUFFER_CAPACITY = 256 };
typedef struct { FramebufferPrefix *framebuffer; uint64_t used; int pending_write; } TrackedFramebuffer;
static TrackedFramebuffer tracked[FRAMEBUFFER_CAPACITY];
static uint64_t use_clock;

/* Keep a visible call boundary for the final-ELF lifetime audit. */
__attribute__((noinline)) void ons_vita_finish_readback(void) {
    if (!gxm_context) return;
    /* The pinned vitaGL glReadPixels only copies CPU memory; glFinish does
     * not close a scene either. Submit before waiting or readback observes
     * the previous contents of the framebuffer. Keep GL bindings intact. */
    if (needs_end_scene) {
        sceneEnd();
        needs_end_scene = 0;
        needs_scene_reset = 1;
    }
    sceGxmFinish(gxm_context);
}

void ons_vita_note_target_scene(const SceGxmRenderTarget *target) {
    if (!target) return;
    for (size_t i = 0; i < FRAMEBUFFER_CAPACITY; ++i) {
        FramebufferPrefix *fb = tracked[i].framebuffer;
        if (fb && fb->active && fb->target == target) tracked[i].pending_write = 1;
    }
}

void ons_vita_prepare_texture_upload(void *storage) {
    if (!storage || !gxm_context) return;
    int attached = 0;
    for (size_t i = 0; i < FRAMEBUFFER_CAPACITY; ++i) {
        FramebufferPrefix *fb = tracked[i].framebuffer;
        if (fb && fb->active && fb->target && tracked[i].pending_write &&
            fb->data == storage) { attached = 1; break; }
    }
    if (!attached) return; /* Sampling-only textures use vitaGL copy-on-write. */
    if (needs_end_scene && in_use_framebuffer && in_use_framebuffer->data == storage) {
        /* glFinish alone cannot execute an open scene. End it before a CPU
         * upload or the pending GPU clear/draw can overwrite the new pixels.
         * Preserve GL bindings; sceneReset lazily resumes the same target. */
        sceneEnd();
        needs_end_scene = 0;
        needs_scene_reset = 1;
    }
    sceGxmFinish(gxm_context);
    for (size_t i = 0; i < FRAMEBUFFER_CAPACITY; ++i) {
        FramebufferPrefix *fb = tracked[i].framebuffer;
        if (fb && fb->data == storage) tracked[i].pending_write = 0;
    }
}

void ons_vita_refresh_texture_storage(void *previous, void *storage) {
    if (!previous || !storage || previous == storage) return;
    for (size_t i = 0; i < FRAMEBUFFER_CAPACITY; ++i) {
        FramebufferPrefix *fb = tracked[i].framebuffer;
        if (!fb || !fb->active || fb->data != previous) continue;
        /* glTexSubImage2D can move texture storage to protect queued sampling.
         * vitaGL's FBO attachment caches its original address. Update that
         * address without destroying depth/target resources or changing the
         * previous scene's already captured surface. */
        sceGxmColorSurfaceSetData(&fb->colorbuffer, storage);
        fb->data = storage;
        if (fb == in_use_framebuffer) dirty_framebuffer = 1;
        /* Continue: aliases can attach the same texture to multiple FBOs. */
    }
}

static void track(GLuint name) {
    if (!name) return; /* The display target is never eligible. */
    FramebufferPrefix *framebuffer = (FramebufferPrefix *)(uintptr_t)name;
    size_t empty = FRAMEBUFFER_CAPACITY;
    for (size_t i = 0; i < FRAMEBUFFER_CAPACITY; ++i) {
        if (tracked[i].framebuffer == framebuffer) {
            tracked[i].used = ++use_clock;
            return;
        }
        if (!tracked[i].framebuffer && empty == FRAMEBUFFER_CAPACITY) empty = i;
    }
    if (empty < FRAMEBUFFER_CAPACITY) {
        tracked[empty].framebuffer = framebuffer;
        tracked[empty].used = ++use_clock;
    }
}

void __real_glGenFramebuffers(GLsizei, GLuint *);
void __wrap_glGenFramebuffers(GLsizei count, GLuint *names) {
    /* The pinned SDK leaves unallocated outputs untouched at its FBO limit. */
    if (names) for (GLsizei i = 0; i < count; ++i) names[i] = 0;
    __real_glGenFramebuffers(count, names);
    if (names) for (GLsizei i = 0; i < count; ++i) track(names[i]);
}

void __real_glBindFramebuffer(GLenum, GLuint);
void __wrap_glBindFramebuffer(GLenum target, GLuint name) {
    __real_glBindFramebuffer(target, name);
    /* Only touch registered names: an invalid GL name must not become an
     * unchecked native pointer in the eviction list. */
    for (size_t i = 0; i < FRAMEBUFFER_CAPACITY; ++i)
        if (name && tracked[i].framebuffer == (FramebufferPrefix *)(uintptr_t)name)
            tracked[i].used = ++use_clock;
}

void __real_glDeleteFramebuffers(GLsizei, const GLuint *);
void __wrap_glDeleteFramebuffers(GLsizei count, const GLuint *names) {
    /* vitaGL's texture last_frame records sampling, not framebuffer writes.
     * A render-only texture can therefore be freed immediately by the SDK,
     * while the GPU still writes it. Complete those writes before detaching
     * the framebuffer and allowing glDeleteTextures to release its storage. */
    int pending = 0;
    if (names) for (GLsizei j = 0; j < count; ++j)
        for (size_t i = 0; i < FRAMEBUFFER_CAPACITY; ++i)
            if (tracked[i].framebuffer == (FramebufferPrefix *)(uintptr_t)names[j] &&
                tracked[i].pending_write) pending = 1;
    if (pending) {
        ons_vita_finish_readback();
        for (size_t i = 0; i < FRAMEBUFFER_CAPACITY; ++i) tracked[i].pending_write = 0;
    }
    /* Remove before vitaGL queues asynchronous GC. Its queued target must not
     * be destroyed a second time by reclamation or by a reused FBO slot. */
    if (names) for (GLsizei j = 0; j < count; ++j)
        for (size_t i = 0; i < FRAMEBUFFER_CAPACITY; ++i)
            if (tracked[i].framebuffer == (FramebufferPrefix *)(uintptr_t)names[j])
                tracked[i] = (TrackedFramebuffer){0};
    __real_glDeleteFramebuffers(count, names);
}

int ons_vita_reclaim_render_target(SceGxmRenderTarget **requested) {
    if (!requested || !gxm_context) return 0;
    int known_request = 0;
    size_t victim = FRAMEBUFFER_CAPACITY;
    for (size_t i = 0; i < FRAMEBUFFER_CAPACITY; ++i) {
        FramebufferPrefix *fb = tracked[i].framebuffer;
        if (!fb) continue;
        if (&fb->target == requested) known_request = 1;
        if (!fb->active || !fb->target || &fb->target == requested ||
            fb == active_write_fb || fb == in_use_framebuffer) continue;
        if (victim == FRAMEBUFFER_CAPACITY || tracked[i].used < tracked[victim].used)
            victim = i;
    }
    /* Only the pinned sceneReset path allocates a registered framebuffer's
     * target. It ends the previous scene before calling CreateRenderTarget.
     * Refuse unrelated allocations, including the default display target. */
    if (!known_request || victim == FRAMEBUFFER_CAPACITY) return 0;
    FramebufferPrefix *fb = tracked[victim].framebuffer;
    sceGxmFinish(gxm_context);
    int result = sceGxmDestroyRenderTarget(fb->target);
    if (result < 0) {
        fprintf(stderr, "Native GXM target reclaim failed: 0x%08x\n", (unsigned)result);
        return 0;
    }
    fb->target = NULL; /* sceneReset recreates metadata on the next write. */
    tracked[victim].pending_write = 0;
    return 1;
}
