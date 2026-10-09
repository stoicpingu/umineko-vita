#pragma once

// Native Vita process services. SDL remains responsible for audio, input,
// the window and the GL context, just as in the desktop engine.
namespace VitaPlatform {
bool initialize();
const char *gameRoot();
const char *dataRoot();
const char *saveRoot();
// Asset-preparation scale, bounded to [0.25, 1.0]; defaults to original size.
float renderScale();
}
