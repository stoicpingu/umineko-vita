#pragma once
#include <cstdio>
namespace VitaIO {
void initialize(const char *gameRoot);
// Returns an ordinary newlib stream, backed by Sony stdio for immutable assets.
// Writes, saves, configs, and failed/unsupported Sony opens use normal stdio.
FILE *open(const char *path, const char *mode);
}
