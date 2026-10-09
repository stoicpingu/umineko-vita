#include <vitaGL.h>
#include <stdio.h>

/* SDL2 initializes vitaGL before the engine opens any video. The default
 * vglInitExtended threshold consumes the available PHYCONT pool, preventing
 * AvPlayer from allocating its decoder frames later. The working Vita port
 * measured an 11,534,336-byte request for 960x544 H.264; retain its 14 MiB
 * reserve. This keeps the deployed video files and native decoder unchanged.
 *
 * The last threshold is vitaGL's own vglInitExtended common-dialog default.
 * Use the public header signature: all parameters use the normal ARM integer
 * calling convention, and GLboolean is the public one-byte return type.
 */
#define ONS_AVPLAYER_PHYCONT_RESERVE (14 * 1024 * 1024)
#define ONS_COMMON_DIALOG_RESERVE 0x8C6000

GLboolean __wrap_vglInitExtended(int pool_size, int width, int height,
                                int ram_threshold, SceGxmMultisampleMode msaa) {
    return vglInitWithCustomThreshold(pool_size, width, height, ram_threshold,
                                      0, ONS_AVPLAYER_PHYCONT_RESERVE,
                                      ONS_COMMON_DIALOG_RESERVE, msaa);
}
