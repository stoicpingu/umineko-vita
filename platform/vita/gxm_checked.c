#include <psp2/gxm.h>
#include <stdio.h>
#include "gpu_render_targets.h"

/* The native renderer owns one GXM context. A failed scene or reservation must
 * never leave vitaGL using an uninitialized uniform pointer or submitting a
 * partial draw. Keep the first failure until the next successful scene begin.
 * These guards report failures; they do not replace or retry rendering work. */
static SceGxmContext *faulted_context;
static SceGxmContext *uniform_context;
static int fault_code;
static unsigned failure_count;
static unsigned resource_failure_count;

int __real_sceGxmCreateRenderTarget(const SceGxmRenderTargetParams *, SceGxmRenderTarget **);
int __wrap_sceGxmCreateRenderTarget(const SceGxmRenderTargetParams *params, SceGxmRenderTarget **target) {
    int result = __real_sceGxmCreateRenderTarget(params, target);
    /* The SDK retains one scarce GXM target per cached FBO. Recover metadata
     * from a completed inactive FBO, preserving its texture, then retry once. */
    if (result == (int)SCE_GXM_ERROR_OUT_OF_RENDER_TARGETS &&
        ons_vita_reclaim_render_target(target))
        result = __real_sceGxmCreateRenderTarget(params, target);
    if (result < 0 && resource_failure_count++ < 16)
        fprintf(stderr, "Native GXM create target failed: 0x%08x size=%ux%u scenes=%u\n",
                (unsigned)result, params ? (unsigned)params->width : 0,
                params ? (unsigned)params->height : 0, params ? (unsigned)params->scenesPerFrame : 0);
    return result;
}

int __real_sceGxmDepthStencilSurfaceInit(SceGxmDepthStencilSurface *, SceGxmDepthStencilFormat,
    SceGxmDepthStencilSurfaceType, unsigned, void *, void *);
int __wrap_sceGxmDepthStencilSurfaceInit(SceGxmDepthStencilSurface *surface,
    SceGxmDepthStencilFormat format, SceGxmDepthStencilSurfaceType type,
    unsigned stride, void *depth, void *stencil) {
    int result = __real_sceGxmDepthStencilSurfaceInit(surface, format, type, stride, depth, stencil);
    if (result < 0 && resource_failure_count++ < 16)
        fprintf(stderr, "Native GXM depth surface failed: 0x%08x stride=%u depth=%p stencil=%p\n",
                (unsigned)result, stride, depth, stencil);
    return result;
}

static int failed(const char *operation, SceGxmContext *context, int result) {
    if (failure_count++ < 16)
        fprintf(stderr, "Native GXM %s failed: 0x%08x context=%p\n",
                operation, (unsigned)result, (void *)context);
    if (context && !faulted_context) {
        faulted_context = context;
        fault_code = result;
    }
    return result;
}

int __real_sceGxmBeginScene(SceGxmContext *, unsigned, const SceGxmRenderTarget *,
    const SceGxmValidRegion *, SceGxmSyncObject *, SceGxmSyncObject *,
    const SceGxmColorSurface *, const SceGxmDepthStencilSurface *);
int __wrap_sceGxmBeginScene(SceGxmContext *context, unsigned flags,
    const SceGxmRenderTarget *target, const SceGxmValidRegion *region,
    SceGxmSyncObject *vertex_sync, SceGxmSyncObject *fragment_sync,
    const SceGxmColorSurface *color, const SceGxmDepthStencilSurface *depth) {
    int result = __real_sceGxmBeginScene(context, flags, target, region,
        vertex_sync, fragment_sync, color, depth);
    if (result < 0) {
        if (failure_count < 16)
            fprintf(stderr, "Native GXM scene surfaces: target=%p color=%p depth=%p\n",
                    (const void *)target, (const void *)color, (const void *)depth);
        return failed("begin scene", context, result);
    }
    ons_vita_note_target_scene(target);
    if (faulted_context && context == faulted_context) {
        faulted_context = NULL;
        fault_code = 0;
    }
    return result;
}

int __real_sceGxmEndScene(SceGxmContext *, const SceGxmNotification *, const SceGxmNotification *);
int __wrap_sceGxmEndScene(SceGxmContext *context,
    const SceGxmNotification *vertex, const SceGxmNotification *fragment) {
    int result = __real_sceGxmEndScene(context, vertex, fragment);
    return result < 0 ? failed("end scene", context, result) : result;
}

typedef int (*ReserveUniform)(SceGxmContext *, void **);
static int reserve(const char *operation, ReserveUniform real_reserve,
    SceGxmContext *context, void **output) {
    uniform_context = context;
    if (!output)
        return failed(operation, context, SCE_GXM_ERROR_INVALID_POINTER);
    *output = NULL;
    if (faulted_context && context == faulted_context)
        return fault_code;
    int result = real_reserve(context, output);
    if (result < 0 || !*output) {
        *output = NULL;
        return failed(operation, context, result < 0 ? result : (int)SCE_GXM_ERROR_INVALID_POINTER);
    }
    return result;
}

int __real_sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext *, void **);
int __real_sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext *, void **);
int __wrap_sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext *context, void **output) {
    return reserve("reserve vertex uniform", __real_sceGxmReserveVertexDefaultUniformBuffer,
        context, output);
}
int __wrap_sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext *context, void **output) {
    return reserve("reserve fragment uniform", __real_sceGxmReserveFragmentDefaultUniformBuffer,
        context, output);
}

int __real_sceGxmSetUniformDataF(void *, const SceGxmProgramParameter *, unsigned, unsigned, const float *);
int __wrap_sceGxmSetUniformDataF(void *buffer, const SceGxmProgramParameter *parameter,
    unsigned offset, unsigned count, const float *data) {
    if (!buffer && faulted_context)
        return fault_code;
    if (!buffer || !parameter || (count && !data))
        return failed("uniform upload", uniform_context, SCE_GXM_ERROR_INVALID_POINTER);
    int result = __real_sceGxmSetUniformDataF(buffer, parameter, offset, count, data);
    return result < 0 ? failed("uniform upload", uniform_context, result) : result;
}

int __real_sceGxmDraw(SceGxmContext *, SceGxmPrimitiveType, SceGxmIndexFormat, const void *, unsigned);
int __wrap_sceGxmDraw(SceGxmContext *context, SceGxmPrimitiveType primitive,
    SceGxmIndexFormat format, const void *indices, unsigned count) {
    if (faulted_context && context == faulted_context)
        return fault_code;
    int result = __real_sceGxmDraw(context, primitive, format, indices, count);
    return result < 0 ? failed("draw", context, result) : result;
}

int __real_sceGxmDrawInstanced(SceGxmContext *, SceGxmPrimitiveType, SceGxmIndexFormat,
    const void *, unsigned, unsigned);
int __wrap_sceGxmDrawInstanced(SceGxmContext *context, SceGxmPrimitiveType primitive,
    SceGxmIndexFormat format, const void *indices, unsigned count, unsigned wrap) {
    if (faulted_context && context == faulted_context)
        return fault_code;
    int result = __real_sceGxmDrawInstanced(context, primitive, format, indices, count, wrap);
    return result < 0 ? failed("draw instanced", context, result) : result;
}
