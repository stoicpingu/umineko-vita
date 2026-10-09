#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// Sony FILE objects stay opaque: they must never reach newlib or SDL directly.
void *onsSonyFopen(const char *, const char *);
int onsSonyFclose(void *);
size_t onsSonyFread(void *, size_t, size_t, void *);
int onsSonyFseek(void *, long, int);
long onsSonyFtell(void *);
int onsSonyFerror(void *);
#ifdef __cplusplus
}
#endif
