/**
 *  Controller.cpp
 *  ONScripter-RU
 *
 *  Contains A/V controller interface.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Media/Controller.hpp"
#include "Engine/Core/ONScripter.hpp"
#include "Engine/Components/Async.hpp"

#include <SDL2/SDL.h>

#include <stdexcept>
#include <cassert>

MediaProcController media;

int MediaProcController::ownInit() {
#if LIBAVFORMAT_VERSION_MAJOR < 58
	av_register_all();
#endif
	av_log_set_level(AV_LOG_QUIET);
	av_log_set_callback(logLine);
	HardwareDecoderIFace::reg();
#if LIBAVCODEC_VERSION_MAJOR < 58
	if (av_lockmgr_register(lockManager)) return -1;
#endif
	audioSpec = AudioSpec();
	int error = audioSpec.init(ons.audio_format);

	return error;
}

int MediaProcController::ownDeinit() {
	resetState();
	return 0;
}

MediaProcController::MediaFrame::~MediaFrame() {
	if (surface) {
		if (media.imagePool) {
			media.imagePool->giveImage(surface);
		} else {
			SDL_FreeSurface(surface);
		}
	}

	dataDeleter(data);
	for (auto &p : planes) freearr(&p);

	planesCnt = 0;
	srcFormat = AV_PIX_FMT_NONE;
}

int MediaProcController::AudioSpec::init(const SDL_AudioSpec &spec) {
	// Grab the format
	switch (spec.format) {
		case AUDIO_U8:
			format = AV_SAMPLE_FMT_U8;
			break;
		case AUDIO_S16:
			format = AV_SAMPLE_FMT_S16;
			break;
		case AUDIO_S32:
			format = AV_SAMPLE_FMT_S32;
			break;
		case AUDIO_F32:
			format = AV_SAMPLE_FMT_FLT;
			break;
		default:
			sendToLog(LogLevel::Error, "Unsupported output audio format\n");
			return -1;
	}

	// Grab the channels
	av_channel_layout_uninit(&channelLayout);
	av_channel_layout_default(&channelLayout, spec.channels);
	channels      = spec.channels;

	// Grab the frequency
	frequency = spec.freq;

	return 0;
}

#if LIBAVCODEC_VERSION_MAJOR < 58
int MediaProcController::lockManager(void **mutex, AVLockOp op) {
	switch (op) {
		case AV_LOCK_CREATE:
			*mutex = SDL_CreateMutex();
			if (!*mutex)
				return 1;
			return 0;
		case AV_LOCK_OBTAIN:
			return SDL_LockMutex(static_cast<SDL_mutex *>(*mutex)) != 0;
		case AV_LOCK_RELEASE:
			return SDL_UnlockMutex(static_cast<SDL_mutex *>(*mutex)) != 0;
		case AV_LOCK_DESTROY:
			SDL_DestroyMutex(static_cast<SDL_mutex *>(*mutex));
			return 0;
	}
	return 1;
}

#endif

void MediaProcController::logLine(void *inst, int level, const char *fmt, va_list args) {
	// We seem to get called regardless of log level
	if (level > AV_LOG_ERROR)
		return;

	char msg[1024];
	std::vsnprintf(msg, sizeof(msg), fmt, args);

	LogLevel iolevel;

	switch (level) {
		case AV_LOG_PANIC:
		case AV_LOG_FATAL:
		case AV_LOG_ERROR:
			iolevel = LogLevel::Error;
			break;
		case AV_LOG_WARNING:
			iolevel = LogLevel::Warn;
			break;
		default:
			iolevel = LogLevel::Info;
			break;
	}

	sendToLog(iolevel, "[ff %d/0x%x] %s", level, inst, msg);
}

std::unique_ptr<MediaProcController::Decoder> MediaProcController::findDecoder(AVMediaType type, unsigned streamNumber, AVCodecID restrictCodecId) {
	unsigned stream = 0;

	while (streamNumber && stream < formatContext->nb_streams) {
		if (formatContext->streams[stream]->codecpar->codec_type == type &&
		    (restrictCodecId == AV_CODEC_ID_NONE || restrictCodecId == formatContext->streams[stream]->codecpar->codec_id)) {
			if (streamNumber == 1) {
				auto codecContext = avcodec_alloc_context3(nullptr);
				if (!codecContext || avcodec_parameters_to_context(codecContext, formatContext->streams[stream]->codecpar) < 0)
					throw std::runtime_error("Failed to create AVCodecContext");
				switch (type) {
					case AVMEDIA_TYPE_VIDEO:
						// Starting with 6f69f7a8bf6a0d013985578df2ef42ee6b1c7994 ffmpeg no longer sets decoding thread count to auto.
						// Specify it ourselves.
#ifdef __vita__
						// SDL's Vita worker is a kernel thread, not a pthread. FFmpeg's
						// slice/frame workers can make it enter a cancellable pthread
						// wait with no PTE thread state (MPEG-2 butterfly movies crash).
						// Decode on our existing asynchronous video worker instead.
						codecContext->thread_count = 1;
						if (nativeVideo.isOpen())
							return std::make_unique<VideoDecoder>(codecContext, nullptr, stream);
#else
						codecContext->thread_count = 0;
#endif
						return Decoder::create<VideoDecoder>(codecContext, stream);
					case AVMEDIA_TYPE_AUDIO:
						return Decoder::create<AudioDecoder>(codecContext, stream);
					case AVMEDIA_TYPE_SUBTITLE:
						return std::make_unique<SubtitleDecoder>(codecContext, nullptr, stream);
					default:
						throw std::runtime_error("Unsupported AVMediaType");
				}
			} else {
				streamNumber--;
			}
		}
		stream++;
	}

	return {};
}

bool MediaProcController::loadVideo(const char *filename, unsigned audioStream, unsigned subtitleStream) {
	if (!filename) {
		return false;
	}

#ifdef VITA
	// A Vita mount (ux0:, app0:, ...) is a filesystem path. FFmpeg otherwise
	// interprets the mount name as an unknown URL protocol and rejects every
	// movie before demuxing. AvPlayer below still receives the native path.
	const std::string inputUrl = std::string("file:") + filename;
	int err = avformat_open_input(&formatContext, inputUrl.c_str(), nullptr, nullptr);
#else
	int err = avformat_open_input(&formatContext, filename, nullptr, nullptr);
#endif

	if (err < 0) {
		sendToLog(LogLevel::Error, "Cannot open video %s (FFmpeg error %d)\n", filename, err);
		formatContext = nullptr;
		return false;
	}

	err = avformat_find_stream_info(formatContext, nullptr);
	if (err < 0) {
		//errorAndCont("ffmpeg: Unable to find stream info.");
		resetState();
		return false;
	}

#ifdef __vita__
	// The SDK FFmpeg deliberately omits H.264; keep its demux/audio/subtitle
	// pipeline and use the Vita media hardware for this video stream.
	for (unsigned i = 0; i < formatContext->nb_streams; ++i) {
		auto par = formatContext->streams[i]->codecpar;
		if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
			const int profile = par->profile & ~FF_PROFILE_H264_CONSTRAINED;
			const bool nativeProfile = profile == FF_PROFILE_H264_BASELINE || profile == FF_PROFILE_H264_MAIN || profile == FF_PROFILE_H264_HIGH;
			const bool nativeContainer = std::strstr(formatContext->iformat->name, "mov") != nullptr;
			if (hardwareDecoding && par->codec_id == AV_CODEC_ID_H264 && nativeContainer && nativeProfile &&
			    par->level <= 42 && par->width <= 1920 && par->height <= 1088)
				nativeVideo.open(filename, formatContext->duration > 0 ? formatContext->duration / 1000 : 0);
			break;
		}
	}
#endif
	decoders[VideoEntry] = findDecoder(AVMEDIA_TYPE_VIDEO);
	decoders[AudioEntry] = findDecoder(AVMEDIA_TYPE_AUDIO, audioStream);
	decoders[SubsEntry]  = findDecoder(AVMEDIA_TYPE_SUBTITLE, subtitleStream);
	if (decoders[SubsEntry] && decoders[SubsEntry]->codecContext->codec_id != AV_CODEC_ID_ASS &&
	    decoders[SubsEntry]->codecContext->codec_id != AV_CODEC_ID_SSA)
		decoders[SubsEntry].reset();

	if (!hasStream(VideoEntry)) {
		resetState();
		return false;
	}

	frameQueueSem[VideoEntry] = SDL_CreateSemaphore(VideoPacketBufferSize);
	frameQueueSem[AudioEntry] = SDL_CreateSemaphore(AudioPacketBufferSize);

	frameQueuemutex[VideoEntry] = SDL_CreateMutex();
	frameQueuemutex[AudioEntry] = SDL_CreateMutex();

	subtitleMutex = SDL_CreateMutex();

	return !hasStream(AudioEntry) || static_cast<AudioDecoder *>(decoders[AudioEntry].get())->initSwrContext(audioSpec);
}

bool MediaProcController::loadPresentation(const GPU_Rect &rect, bool loop) {
	loopVideo = loop;

	int nworkers = 0;
	for (auto i : {VideoEntry, AudioEntry})
		if (decoders[i]
#ifdef __vita__
		    && (i != VideoEntry || !nativeVideo.isOpen())
#endif
		)
			nworkers++;
	decoderWorkerCount.store(nworkers, std::memory_order_relaxed);

	demux = std::make_unique<MediaDemux>();
	demux->prepare(decoders[VideoEntry] ? decoders[VideoEntry]->stream : MediaDemux::InvalidStream,
	               decoders[AudioEntry] ? decoders[AudioEntry]->stream : MediaDemux::InvalidStream,
	               decoders[SubsEntry] ? decoders[SubsEntry]->stream : MediaDemux::InvalidStream);

	/* Prepare SW scale */
	auto vdec = static_cast<VideoDecoder *>(decoders[VideoEntry].get());
	if (
#ifdef __vita__
	    nativeVideo.isOpen() ||
#endif
	    vdec->initSwsContext(rect.w, alphaMasked ? rect.h * 2 : rect.h, nullptr, false)) {

		/* Allocate surfaces */
		imagePool         = std::make_unique<TempImagePool>();
		imagePool->size.x = rect.w;
		imagePool->size.y = alphaMasked ? rect.h * 2 : rect.h;
		#ifdef __vita__
		imagePool->addImages(nativeVideo.isOpen() ? 2 : VideoPacketBufferSize);
#else
		imagePool->addImages(VideoPacketBufferSize);
#endif

#ifdef __vita__
		if (nativeVideo.isOpen()) {
			auto stream = formatContext->streams[vdec->stream];
			double fps = av_q2d(stream->avg_frame_rate);
			if (!std::isfinite(fps) || fps <= 0) fps = av_q2d(stream->r_frame_rate);
			if (!std::isfinite(fps) || fps <= 0) fps = 30;
			vdec->nanosPerFrame = static_cast<uint64_t>(1000000000.0 / fps);
			// AvPlayer already demuxes video. With the game's separate Ogg/ASS
			// tracks, a second reader would scan the whole MP4 for no consumer.
			if (hasStream(AudioEntry) || hasStream(SubsEntry)) async.loadPacketArrays();
			else demux->demuxComplete.store(true, std::memory_order_release);
			return true;
		}
#endif

		initVideoTimecodes.fill(0);
		initVideoTimecodesLock = SDL_CreateSemaphore(0);
		async.loadPacketArrays();

		/* Get timing info */
		vdec->initTiming(formatContext->duration);

		return true;
	}

	return false;
}

bool MediaProcController::addSubtitles(const char *filename, int frameWidth, int frameHeight) {
	// No subtitles are supported in case of alpha-masked video
	if (alphaMasked) {
		if (hasStream(SubsEntry)) {
			decoders[SubsEntry].reset();
		} else if (!filename) {
			return true;
		}

		sendToLog(LogLevel::Error, "Cannot use subtitles on alphamasked videos\n");
		return false;
	}

	if (filename && decoders[SubsEntry]) {
		sendToLog(LogLevel::Error, "Subtitles had already been loaded\n");
		filename = nullptr;
	} else if (filename) {
		decoders[SubsEntry] = Decoder::create<SubtitleDecoder>();
	} else if (!decoders[SubsEntry]) {
		return true;
	}

	SDL_LockMutex(subtitleMutex);
	bool prepared = static_cast<SubtitleDecoder *>(decoders[SubsEntry].get())->prepare(filename, frameWidth, frameHeight);
	SDL_UnlockMutex(subtitleMutex);
	return prepared;
}

void MediaProcController::frameSize(const SDL_Rect &rect, int &width, float &wFactor, int &height, float &hFactor, bool alpha) {
	alphaMasked = alpha;

	auto context = decoders[VideoEntry]->codecContext;

	if (context->width < rect.w) {
		width   = context->width;
		wFactor = rect.w / static_cast<float>(context->width);
	} else {
		width   = rect.w;
		wFactor = 1;
	}

	if (context->height < rect.h) {
		if (!alphaMasked) {
			height  = context->height;
			hFactor = rect.h / static_cast<float>(context->height);
		} else if (context->height / 2 < rect.h) {
			height  = context->height / 2;
			hFactor = 2.0f * rect.h / context->height;
		}
	} else {
		height  = rect.h;
		hFactor = 1;
	}

	//sendToLog(LogLevel::Info, "Frame size is %dx%d, out frame size is %dx%d\n", context->width, context->height, width, height);
}

void MediaProcController::startProcessing() {
#ifdef __vita__
	if (nativeVideo.isOpen()) {
		if (!nativeVideo.start(loopVideo)) sendToLog(LogLevel::Error, "Native video start failed: %s\n", nativeVideo.error());
		else sendToLog(LogLevel::Info, "Native video: AvPlayer auto-start, duration %llu ms\n",
		               static_cast<unsigned long long>(nativeVideo.duration()));
	}
	else
#endif
	async.loadVideoFrames();
	if (hasStream(AudioEntry))
		async.loadAudioFrames();
}

bool MediaProcController::finish(bool needLastFrame) {
	//TODO: implement skip to last frame

	int value = decoderWorkerCount.load(std::memory_order_acquire);

	if (value > 0) { // Decoders are not done
		for (auto &decoder : decoders) {
			if (decoder)
				decoder->shouldFinish.store(true, std::memory_order_relaxed);
		}
		std::atomic_thread_fence(std::memory_order_release);

		// Let them finish
		SDL_Delay(1);

		value = decoderWorkerCount.load(std::memory_order_acquire);
	}

	// Decoders are done?
	if (value == 0) {
		// Signal demux
		if (demux)
			demux->shouldFinish.store(true, std::memory_order_release);

		if (demux && !demux->demuxComplete.load(std::memory_order_acquire)) return false;

		resetDecoders();

		if (needLastFrame) {
			resetFrameQueues(0, 1);
		} else {
			resetFrameQueues(1, 0);
		}

		// Demux is done?
		if (demux && demux->demuxComplete.load(std::memory_order_acquire)) {
			resetDemuxer();
		}

		if (!demux) {
			return true;
		}
	}

	return false;
}

void MediaProcController::resetDecoders() {
	// Destroy decoders
	SDL_mutexP(subtitleMutex);
	for (auto &decoder : decoders) {
		if (decoder)
			decoder.reset();
	}
	SDL_mutexV(subtitleMutex);

	for (auto &mutex : frameQueuemutex) {
		if (mutex) {
			SDL_DestroyMutex(mutex);
			mutex = nullptr;
		}
	}

	for (auto semArr : {&frameQueueSem}) {
		for (auto &sem : *semArr) {
			if (sem) {
				SDL_DestroySemaphore(sem);
				sem = nullptr;
			}
		}
	}
}

void MediaProcController::resetDemuxer() {
	if (demux) {
		demux->resetPacketQueue();
		demux.reset();
	}
}

void MediaProcController::resetFrameQueues(int vidStart, int vidEnd) {
	// Cleanup the queues
	auto &vidQueue = async.loadFramesQueue[VideoEntry].results;
	if (!vidQueue.empty()) {
		std::for_each(std::begin(vidQueue) + vidStart, std::end(vidQueue) - vidEnd, [](void *&elem) {
			delete static_cast<MediaFrame *>(elem);
		});
		vidQueue.erase(std::begin(vidQueue) + vidStart, std::end(vidQueue) - vidEnd); /* Leave last available frame */
	}

	for (auto &elem : async.loadFramesQueue[AudioEntry].results) {
		delete static_cast<MediaFrame *>(elem);
	}
	async.loadFramesQueue[AudioEntry].results.clear();

	// Note that SubsEntry queue is not ours
}

void MediaProcController::resetState() {
#ifdef __vita__
	nativeVideo.close(); // finishes decoder sampling and restores its placeholder
	if (nativeYuvImage) gpu.freeImage(nativeYuvImage);
	if (nativeSubtitleImage) gpu.freeImage(nativeSubtitleImage);
	nativeYuvImage = nativeSubtitleImage = nullptr;
	nativeSubtitleBounds = {};
	nativeSubtitlePixels.clear();
#endif
	resetDecoders();
	resetFrameQueues();
	resetDemuxer();

	if (subtitleMutex) {
		SDL_DestroyMutex(subtitleMutex);
		subtitleMutex = nullptr;
	}

	// This is probably no longer needed
	/*if (demux) demux->resetSpacesSem();
	while (SDL_SemValue(frameQueueSem[VideoEntry]) != VideoPacketBufferSize) SDL_SemPost(frameQueueSem[VideoEntry]);
	while (SDL_SemValue(frameQueueSem[AudioEntry]) != AudioPacketBufferSize) SDL_SemPost(frameQueueSem[AudioEntry]);
	if (demux) demux->resetDataSem();*/

	imagePool.reset();

	if (formatContext)
		avformat_close_input(&formatContext);
}

void MediaProcController::decodeFrames(MediaEntries entry) {
	if (hasStream(entry)) {
		decoders[entry]->decodeFrame(entry);
		media.decoderWorkerCount.fetch_sub(1, std::memory_order_release);
	}
}

void MediaProcController::demultiplexStreams() {
	demux->demultiplexStreams(av_q2d(formatContext->streams[decoders[VideoEntry]->stream]->time_base));
	demux->demuxComplete.store(true, std::memory_order_release);
}

void MediaProcController::getVideoTimecodes(size_t &counter, AVPacket *packet, long double videoTimeBase) {
	static int64_t initialValue = 0;
	/* For future framerate detection */
	// pts values are normally disordered, this may cause some issues, like isVFR = 1 or isCorrupted = 1
	if (counter < VideoPacketBufferSize && !(packet->flags & AV_PKT_FLAG_CORRUPT)) {
		if (packet->stream_index == decoders[VideoEntry]->stream) {
			if (packet->pts == AV_NOPTS_VALUE) {
				if (counter == 0)
					initialValue = 0;
				initVideoTimecodes[counter] = 0;
			} else {
				if (counter == 0)
					initialValue = packet->pts;
				initVideoTimecodes[counter] = videoTimeBase * (packet->pts - initialValue);
			}
			counter++;
		} else if (hasStream(AudioEntry) && packet->stream_index == decoders[AudioEntry]->stream &&
		           demux->packetQueueSpacesAvailable(AudioEntry)) {
			for (; counter < VideoPacketBufferSize; counter++) initVideoTimecodes[counter] = 0;
		}
		if (counter == VideoPacketBufferSize && initVideoTimecodesLock) {
			SDL_SemPost(initVideoTimecodesLock);
		}
	}
}

void MediaProcController::processSubsData(char *data, size_t length, int64_t timestamp, int64_t duration) {
	SDL_mutexP(subtitleMutex);
	if (hasStream(SubsEntry)) {
		static_cast<SubtitleDecoder *>(media.decoders[SubsEntry].get())->processData(data, length, timestamp, duration);
	}
	SDL_mutexV(subtitleMutex);
}

void MediaProcController::applySubtitles(MediaFrame &frame) {
	SDL_mutexP(subtitleMutex);
	if (hasStream(SubsEntry)) {
		static_cast<SubtitleDecoder *>(decoders[SubsEntry].get())->processFrame(frame);
	}
	SDL_mutexV(subtitleMutex);
}

// Decoder stuff

void MediaProcController::Decoder::decodeFrame(MediaEntries index) {
	bool draining = false, finalDrain = false;
	cmp::unique_ptr_del<AVPacket> packet(nullptr, MediaDemux::freePacket);
	while (!async.threadShutdownRequested && !shouldFinish.load(std::memory_order_acquire)) {
		av_frame_unref(frame);
		int result = avcodec_receive_frame(codecContext, frame);
		std::unique_ptr<MediaFrame> ready;
		if (result == 0) {
			ready = std::make_unique<MediaFrame>();
			processFrame(*ready);
			if (!ready->has()) continue;
			if (index == VideoEntry) media.applySubtitles(*ready);
		} else if (result == AVERROR_EOF) {
			if (!finalDrain) {
				avcodec_flush_buffers(codecContext);
				draining = false;
				continue;
			}
		} else {
			if (result != AVERROR(EAGAIN)) {
				sendToLog(LogLevel::Error, "Decoder receive failed: %d\n", result);
			}
			if (draining) break;
			if (!packet) {
				while (media.demux->waitForData(index, 10)) {
					if (async.threadShutdownRequested || shouldFinish.load(std::memory_order_acquire)) return;
				}
				bool end = false;
				packet = cmp::unique_ptr_del<AVPacket>(media.demux->obtainPacket(index, end), MediaDemux::freePacket);
				finalDrain = end;
			}
			const bool flush = !packet || (!packet->data && !packet->size);
			result = avcodec_send_packet(codecContext, flush ? nullptr : packet.get());
			if (result == AVERROR(EAGAIN)) continue; // retain the packet until accepted
			packet.reset();
			if (result < 0 && result != AVERROR_EOF) {
				sendToLog(LogLevel::Warn, "Decoder rejected packet: %d\n", result);
				continue;
			}
			draining = flush;
			continue;
		}
		while (SDL_SemWaitTimeout(media.frameQueueSem[index], 10)) {
			if (async.threadShutdownRequested || shouldFinish.load(std::memory_order_acquire)) return;
		}
		SDL_AtomicLock(&async.loadFramesQueue[index].resultsLock);
		async.loadFramesQueue[index].results.push_back(ready.release());
		SDL_AtomicUnlock(&async.loadFramesQueue[index].resultsLock);
		if (result == AVERROR_EOF) return;
	}
}

const AVCodec *MediaProcController::Decoder::findCodec(AVCodecContext *context) {
	const AVCodec *codec = nullptr;

	// Setup hw acceleration
	if (context->codec_type == AVMEDIA_TYPE_VIDEO && media.hardwareDecoding) {
		context->get_format = HardwareDecoderIFace::init;

		// Some decoders might need explicit open
		codec = HardwareDecoderIFace::findDecoder(context);
		if (codec) {
			int readWidth  = context->width;
			int readHeight = context->height;

			int err = avcodec_open2(context, codec, nullptr);
			if (!err) {
				// This is a workaround for certain hw accelerated decoders e.g. droid's MediaCodec.
				// It seems that the context is initialised with some default value (320x240) and later gets changed to the real dims.
				// Fortunately we should already have the real dimensions.
				if (context->codec_type == AVMEDIA_TYPE_VIDEO && (readWidth != context->width || readHeight != context->height)) {
					sendToLog(LogLevel::Warn, "Fixing up dimensions to %dx%d from %dx%d\n", readWidth, readHeight, context->width, context->height);
					context->width  = readWidth;
					context->height = readHeight;
				}
			} else {
				sendToLog(LogLevel::Error, "Unable to open explicit hw decoder %d\n", err);
				codec = nullptr;
			}
		}
	}

	// Fallback to implicit hardware or software decoder
	if (!codec) {
		codec = avcodec_find_decoder(context->codec_id);
		if (!codec) return nullptr;

		int err = avcodec_open2(context, codec, nullptr);
		if (err < 0) {
			sendToLog(LogLevel::Error, "Unable to open decoder %d\n", err);
			avcodec_close(context);
			return nullptr;
		}
	}

	return codec;
}

std::unique_ptr<MediaProcController::MediaFrame> MediaProcController::advanceVideoFrames(int &framesToAdvance, bool &endOfFile) {
#ifdef __vita__
	if (nativeVideo.isOpen()) {
		auto result = std::make_unique<MediaFrame>();
		VitaMediaPlayer::Frame decoded;
		if (!nativeVideo.readFrame(decoded, endOfFile)) {
			if (endOfFile)
				sendToLog(LogLevel::Info, "Native video ended at %llu / %llu ms\n",
				          static_cast<unsigned long long>(nativeVideo.timestamp()),
				          static_cast<unsigned long long>(nativeVideo.duration()));
			if (endOfFile && nativeVideo.error())
				sendToLog(LogLevel::Error, "Native video playback failed: %s\n", nativeVideo.error());
			return {};
		}
		framesToAdvance = 0;
		result->msTimeStamp = decoded.timestamp;
		if (!alphaMasked && hardwareConversion) result->nativeFrame = true;
		else {
			result->surface = imagePool->getImage();
			if (!nativeVideo.convertFrame(result->surface)) { endOfFile = true; return {}; }
			applySubtitles(*result);
		}
		return result;
	}
#endif
	std::unique_ptr<MediaFrame> frame = nullptr;

	AsyncInstructionQueue &vidQueue = async.loadFramesQueue[VideoEntry];
	bool canSkipThisFrame           = false;

	while (framesToAdvance) {
		SDL_AtomicLock(&vidQueue.resultsLock);
		if (vidQueue.results.empty()) {
			// There is nothing to render unfortunately, exit
			SDL_AtomicUnlock(&vidQueue.resultsLock);
			return nullptr;
		}
		if (vidQueue.results.size() == 1 &&
		    vidQueue.results.front() == nullptr) {
			endOfFile = true;
			SDL_AtomicUnlock(&vidQueue.resultsLock);
			return nullptr;
		}

		frame = std::unique_ptr<MediaFrame>(static_cast<MediaFrame *>(vidQueue.results.front()));
		vidQueue.results.pop_front();
		canSkipThisFrame = !vidQueue.results.empty() &&
		                   !(vidQueue.results.size() == 1 &&
		                     vidQueue.results.front() == nullptr);
		SDL_AtomicUnlock(&vidQueue.resultsLock);
		SDL_SemPost(frameQueueSem[VideoEntry]);

		framesToAdvance--;

		if (framesToAdvance == 0 || !canSkipThisFrame) {
			break;
		}
	}

	assert(frame->surface != nullptr);

	return frame;
}

#ifdef __vita__
void MediaProcController::presentNativeFrame(GPU_Image *target, MediaFrame &frame) {
	if (!nativeYuvImage) nativeYuvImage = gpu.createImage(16, 16, 4, false, false);
	if (nativeYuvImage && nativeVideo.bindFrame(static_cast<unsigned>(GPU_GetTextureHandle(nativeYuvImage)))) {
		GPU_GetTarget(target);
		GPU_SetBlending(nativeYuvImage, false);
		gpu.copyGPUImage(nativeYuvImage, nullptr, nullptr, target->target,
		                 target->w / 2.f, target->h / 2.f,
		                 target->w / float(nativeYuvImage->w), target->h / float(nativeYuvImage->h), 0, true);
		nativeVideo.finishFrameUse(); // release decoder storage before libass work
		// Subtitles remain full libass output (including animated ASS). Only
		// changed glyph regions are uploaded; decoder pixels stay untouched.
		SDL_LockMutex(subtitleMutex);
		if (hasStream(SubsEntry)) {
			auto &driver = static_cast<SubtitleDecoder *>(decoders[SubsEntry].get())->subtitleDriver;
			if (driver.renderNativeOverlay(nativeSubtitlePixels, nativeSubtitleBounds, target->w, target->h, frame.msTimeStamp)) {
				if (nativeSubtitleImage && (nativeSubtitleImage->w != nativeSubtitleBounds.w || nativeSubtitleImage->h != nativeSubtitleBounds.h)) {
					gpu.freeImage(nativeSubtitleImage); nativeSubtitleImage = nullptr;
				}
				if (nativeSubtitleBounds.w && nativeSubtitleBounds.h) {
					if (!nativeSubtitleImage) nativeSubtitleImage = gpu.createImage(nativeSubtitleBounds.w, nativeSubtitleBounds.h, 4, false, false);
					GPU_UpdateImageBytes(nativeSubtitleImage, nullptr, nativeSubtitlePixels.data(), nativeSubtitleBounds.w * 4);
				}
			}
		}
		SDL_UnlockMutex(subtitleMutex);
		if (nativeSubtitleImage) {
			GPU_SetBlending(nativeSubtitleImage, true);
			gpu.copyGPUImage(nativeSubtitleImage, nullptr, nullptr, target->target,
			                 nativeSubtitleBounds.x + nativeSubtitleBounds.w / 2.f,
			                 nativeSubtitleBounds.y + nativeSubtitleBounds.h / 2.f, 1, 1, 0, true);
		}
	} else {
		if (!frame.surface) frame.surface = imagePool->getImage();
		if (!nativeVideo.convertFrame(frame.surface)) return;
		applySubtitles(frame);
		gpu.updateImage(target, nullptr, frame.surface, nullptr, false);
	}
}
#endif

cmp::unique_ptr_del<uint8_t[]> MediaProcController::advanceAudioChunks(size_t &buffSz) {
	MediaFrame *frame{nullptr};

	AsyncInstructionQueue &audQueue = async.loadFramesQueue[AudioEntry];
	SDL_AtomicLock(&audQueue.resultsLock);
	if (audQueue.results.empty()) {
		SDL_AtomicUnlock(&audQueue.resultsLock);
	} else if (audQueue.results.size() == 1 &&
	           audQueue.results.front() == nullptr) {
		SDL_AtomicUnlock(&audQueue.resultsLock);
	} else {
		frame = static_cast<MediaFrame *>(audQueue.results.front());
		audQueue.results.pop_front();
		SDL_AtomicUnlock(&audQueue.resultsLock);
		SDL_SemPost(media.frameQueueSem[AudioEntry]);
	}

	if (frame) {
		buffSz = frame->dataSize;
		cmp::unique_ptr_del<uint8_t[]> ret(frame->data, frame->dataDeleter);
		frame->data = nullptr;
		delete frame;
		return ret;
	}

	return {};
}
