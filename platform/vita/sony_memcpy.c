#include <psp2/kernel/clib.h>
#include <stddef.h>

// Intercept external calls; small compiler-inlined copies remain inline.
void *__wrap_memcpy(void *destination, const void *source, size_t bytes) {
    return bytes ? sceClibMemcpy(destination, source, bytes) : destination;
}
void __wrap___aeabi_memcpy(void *destination, const void *source, size_t bytes) {
    if (bytes) sceClibMemcpy(destination, source, bytes);
}
void __wrap___aeabi_memcpy4(void *destination, const void *source, size_t bytes) {
    if (bytes) sceClibMemcpy(destination, source, bytes);
}
void __wrap___aeabi_memcpy8(void *destination, const void *source, size_t bytes) {
    if (bytes) sceClibMemcpy(destination, source, bytes);
}
