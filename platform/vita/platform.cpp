#include "platform/vita/platform.hpp"
#include "platform/vita/assets.hpp"
#include "platform/vita/startup_image.hpp"
#ifdef ONS_VITA_FIOS
#include "platform/vita/sony_io.hpp"
#endif

#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef ONS_VITA_GAME_ROOT
#define ONS_VITA_GAME_ROOT "ux0:data/umineko/"
#endif

#ifndef ONS_VITA_DATA_ROOT
#define ONS_VITA_DATA_ROOT "ux0:data/umineko-native/"
#endif

// Reserve the remainder of the application budget for vitaGL/GXM allocations.
extern "C" {
int _newlib_heap_size_user = 192 * 1024 * 1024;
}

namespace VitaPlatform {
const char *gameRoot() { return ONS_VITA_GAME_ROOT; }
const char *dataRoot() { return ONS_VITA_DATA_ROOT; }
const char *saveRoot() { return ONS_VITA_GAME_ROOT "save/"; }

float renderScale() {
	static const float scale = [] {
		float result = 1.0f;
		FILE *file = std::fopen(ONS_VITA_GAME_ROOT "render_scale.txt", "r");
		if (!file)
			return result;
		char buffer[64]{};
		if (std::fgets(buffer, sizeof(buffer), file)) {
			char *end = nullptr;
			errno = 0;
			float candidate = std::strtof(buffer, &end);
			bool valid = end != buffer && errno != ERANGE;
			while (*end && std::isspace(static_cast<unsigned char>(*end)))
				++end;
			// Reject truncated or trailing non-whitespace input as well as
			// NaN/infinity, which would corrupt target-size calculations.
			int remaining;
			while ((remaining = std::fgetc(file)) != EOF)
				if (!std::isspace(static_cast<unsigned char>(remaining)))
					valid = false;
			if (valid && *end == '\0' && std::isfinite(candidate) && candidate >= 0.25f && candidate <= 1.0f)
				result = candidate;
			else
				std::fprintf(stderr, "Invalid render_scale.txt; using original asset scale 1.0\n");
		}
		std::fclose(file);
		return result;
	}();
	return scale;
}

bool initialize() {
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

	// Native preferences belong here; game state belongs to the installation.
	// Android and native use the same upstream save format and directory.
	if (mkdir("ux0:data", 0777) != 0 && errno != EEXIST)
		return false;
	if (mkdir(dataRoot(), 0777) != 0 && errno != EEXIST)
		return false;
#ifdef ONS_VITA_FILE_LOGGING
	// Opt-in diagnostic builds only: release launches must not create/truncate
	// log files or write to storage for every warning emitted by a dependency.
	std::freopen(ONS_VITA_DATA_ROOT "out.txt", "w", stdout);
	std::freopen(ONS_VITA_DATA_ROOT "err.txt", "w", stderr);
	setvbuf(stdout, nullptr, _IOLBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
#endif
	if (chdir(gameRoot()) != 0) {
		std::fprintf(stderr, "Cannot open native game root %s: %s\n", gameRoot(), std::strerror(errno));
		return false;
	}
	showStartupImage();
#ifdef ONS_VITA_FIOS
	VitaIO::initialize(gameRoot());
#endif
	VitaAssets::initializeImageSizes();
	return true;
}
}
