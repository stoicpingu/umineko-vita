#ifndef ONS_VITA_GPU_RENDER_TARGETS_H
#define ONS_VITA_GPU_RENDER_TARGETS_H
#include <psp2/gxm.h>
#include <stdint.h>
/* Reclaim completed, inactive FBO metadata without changing texture contents. */
int ons_vita_reclaim_render_target(SceGxmRenderTarget **requested);
void ons_vita_refresh_texture_storage(void *previous, void *storage);
void ons_vita_prepare_texture_upload(void *storage);
void ons_vita_note_target_scene(const SceGxmRenderTarget *target);
/* Submit the open scene before synchronously reading its pixels on the CPU. */
void ons_vita_finish_readback(void);
#endif
