#pragma once

struct GPU_Target;
namespace VitaPlatform {
void showStartupImage();
bool startupImageShown();
bool startupImagePending();
void handoffStartupImage(GPU_Target *target);
void releaseStartupImage();
}
