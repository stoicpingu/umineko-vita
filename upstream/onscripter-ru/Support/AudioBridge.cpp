/**
 *  AudioBridge.cpp
 *  ONScripter-RU
 *
 *  SDL_Mixer external audio handler interaction.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Support/AudioBridge.hpp"
#include "Support/FileDefs.hpp"

#include <iostream>
#include <algorithm>
#include <cstring>
#include <cassert>

void AudioBridge::fillBuffers(int /*channel*/, void *stream, int len, void *udata) {
	auto &ab = *static_cast<AudioBridge *>(udata);
	auto *output = static_cast<uint8_t *>(stream);
	size_t written = 0;
	const size_t length = len > 0 ? static_cast<size_t>(len) : 0;
	while (written < length) {
		if (!ab.curBuffer) {
			ab.curBufferPos = 0;
			ab.curBufferSize = 0;
			ab.curBuffer = ab.retrieval(ab.curBufferSize);
		}
		if (!ab.curBuffer || ab.curBufferSize == 0) {
			ab.curBuffer.reset();
			std::memset(output + written, 0, length - written);
			break;
		}
		const size_t count = std::min(length - written, ab.curBufferSize - ab.curBufferPos);
		std::memcpy(output + written, ab.curBuffer.get() + ab.curBufferPos, count);
		ab.curBufferPos += count;
		written += count;
		if (ab.curBufferPos == ab.curBufferSize) ab.curBuffer.reset();
	}
	if (!ab.startedToPlay.load(std::memory_order_acquire)) {
		ab.startedTime = SDL_GetTicks();
		ab.startedToPlay.store(true, std::memory_order_release);
	}
}

bool AudioBridge::update(uint32_t &toAdd) {
	if (!startedToPlay.load(std::memory_order_acquire))
		return false;
	if (startedTime != 0) {
		toAdd       = SDL_GetTicks() - startedTime;
		startedTime = 0;
	}
	return true;
}

bool AudioBridge::startPlayback() {
	if (started.load(std::memory_order_relaxed))
		return true;

	// Call that externally to avoid issues
	if (!Mix_Playing(channelNumber)) {
		Mix_Volume(channelNumber, channelVolume);
		if (Mix_PlayChannel(channelNumber, rawChunk, -1) < 0) return false;
		started.store(true, std::memory_order_relaxed);
		return true;
	}

	return false;
}

bool AudioBridge::prepare() {
	bool ret = true;

	rawBuffer = std::make_unique<uint8_t[]>(rawBufferSize);
	std::memset(rawBuffer.get(), 0, rawBufferSize);
	rawChunk = Mix_QuickLoad_RAW(rawBuffer.get(), static_cast<uint32_t>(rawBufferSize));

	if (!rawChunk) {
		sendToLog(LogLevel::Error, "Failed to prepare an audio stream %s\n", Mix_GetError());
		ret = false;
	} else {
		if (!Mix_RegisterEffect(channelNumber, fillBuffers, nullptr, static_cast<void *>(this))) {
			sendToLog(LogLevel::Error, "Failed to prepare audio update function: %s\n", Mix_GetError());
			ret = false;
		}
	}

	// A delay is very big anyway, a chance it happens before we start to display anything... is too low
	//Mix_PlayChannel(audio_channel_number, audio_raw_chunk, -1);
	//Mix_Pause(audio_channel_number);

	return ret;
}
