#pragma once

struct GPU_Image;
#ifdef __cplusplus
extern "C" {
#endif
/* Flush and finish sampling before CPU writes to a persistent RGBA atlas.
 * The returned linear storage stays owned by vitaGL. No GL state may change
 * between this call and completing the batch of row copies. */
unsigned char *ons_vita_begin_glyph_upload(struct GPU_Image *image, unsigned *pitch);
#ifdef __cplusplus
}
#endif
