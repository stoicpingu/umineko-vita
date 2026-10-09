#ifndef SDL_GPU_COPY_ROWS_H
#define SDL_GPU_COPY_ROWS_H
#include <stddef.h>
#include <string.h>

/* Surface pitch can include padding that does not exist in GPU readback data. */
static int gpu_copy_rows(unsigned char *destination, size_t destination_pitch,
                         const unsigned char *source, size_t source_pitch,
                         size_t row_bytes, size_t height)
{
    size_t row;
    if (!destination || !source || row_bytes > destination_pitch || row_bytes > source_pitch)
        return 0;
    for (row = 0; row < height; ++row) {
        memcpy(destination + row * destination_pitch, source + row * source_pitch, row_bytes);
        memset(destination + row * destination_pitch + row_bytes, 0, destination_pitch - row_bytes);
    }
    return 1;
}
#endif
