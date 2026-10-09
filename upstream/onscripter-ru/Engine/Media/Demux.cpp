/**
 *  Demux.cpp
 *  ONScripter-RU
 *
 *  Contains Media Engine A/V and subtitle demultiplexor.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Media/Controller.hpp"
#include "Engine/Components/Async.hpp"

bool MediaProcController::MediaDemux::prepare(int videoStream, int audioStream, int subtitleStream) {
	packetQueueSemSpaces[VideoEntry] = SDL_CreateSemaphore(VideoPacketBufferSize);
	packetQueueSemSpaces[AudioEntry] = SDL_CreateSemaphore(AudioPacketBufferSize);

	packetQueueSemData[VideoEntry] = SDL_CreateSemaphore(0);
	packetQueueSemData[AudioEntry] = SDL_CreateSemaphore(0);

	streamIds[VideoEntry] = videoStream;
	streamIds[AudioEntry] = audioStream;
	streamIds[SubsEntry]  = subtitleStream;

	return true;
}

bool MediaProcController::MediaDemux::resetPacketQueue() {
	for (auto entry : {VideoEntry, AudioEntry}) {
		while (!packetQueue[entry].empty()) {
			AVPacket *pkt = packetQueue[entry].back();
			if (pkt) {
				av_packet_unref(pkt);
				av_packet_free(&pkt);
			}
			packetQueue[entry].pop_back();
		}
	}
	return true;
}

AVPacket *MediaProcController::MediaDemux::obtainPacket(MediaEntries index, bool &cacheReadStarted) {
	Lock lock(&packetQueue[index]);
	if (packetQueue[index].empty()) return nullptr;
	AVPacket *packet = packetQueue[index].front();
	packetQueue[index].pop_front();
	SDL_SemPost(packetQueueSemSpaces[index]);
	cacheReadStarted = packet == nullptr;
	return packet;
}

void MediaProcController::MediaDemux::demultiplexStreams(long double videoTimeBase) {
	size_t counter = 0;
	bool complete = false;
	while (!complete && !shouldFinish.load(std::memory_order_acquire) && !async.threadShutdownRequested) {
		AVPacket *packet = av_packet_alloc();
		if (!packet) break;
		int result = av_read_frame(media.formatContext, packet);
		bool needTimecodes = true;
#ifdef __vita__
		needTimecodes = !media.nativeVideo.isOpen();
#endif
		if (needTimecodes && result >= 0) media.getVideoTimecodes(counter, packet, videoTimeBase);
		else if (needTimecodes && counter < VideoPacketBufferSize) {
			counter = VideoPacketBufferSize;
			SDL_SemPost(media.initVideoTimecodesLock);
		}
		MediaEntries entry = InvalidEntry;
		if (result >= 0 && !(packet->flags & AV_PKT_FLAG_CORRUPT)) {
			for (auto e : {VideoEntry, AudioEntry, SubsEntry})
				if (packet->stream_index == streamIds[e]) entry = e;
		}
		pushPacket(entry, packet, result, complete, videoTimeBase);
	}
	if (counter < VideoPacketBufferSize && media.initVideoTimecodesLock) SDL_SemPost(media.initVideoTimecodesLock);
}

void MediaProcController::MediaDemux::pushPacket(MediaEntries id, AVPacket *packet, int read_result, bool &complete, long double) {
	auto enqueue = [&](MediaEntries entry, AVPacket *p) {
		while (SDL_SemWaitTimeout(packetQueueSemSpaces[entry], 10)) {
			if (shouldFinish.load(std::memory_order_acquire) || async.threadShutdownRequested) {
				av_packet_free(&p);
				return;
			}
		}
		Lock lock(&packetQueue[entry]);
		packetQueue[entry].push_back(p);
		SDL_SemPost(packetQueueSemData[entry]);
	};
	if (read_result < 0) {
		av_packet_free(&packet);
		bool mayLoop = media.loopVideo;
#ifdef __vita__
		// AvPlayer loops its own video clock. With no FFmpeg audio stream,
		// scanning the file repeatedly would busy-loop and duplicate subtitles.
		if (media.nativeVideo.isOpen() && !media.hasStream(AudioEntry)) mayLoop = false;
#endif
		bool looping = mayLoop && av_seek_frame(media.formatContext, streamIds[VideoEntry], 0, AVSEEK_FLAG_BACKWARD) >= 0;
		for (auto entry : {VideoEntry, AudioEntry}) {
			if (streamIds[entry] == InvalidStream) continue;
#ifdef __vita__
			if (entry == VideoEntry && media.nativeVideo.isOpen()) continue;
#endif
			enqueue(entry, looping ? av_packet_alloc() : nullptr);
		}
		complete = !looping;
	} else if (id == SubsEntry) {
		if (packet->data && packet->size > 0) {
			auto timeBase = media.formatContext->streams[streamIds[SubsEntry]]->time_base;
			int64_t timestamp = packet->pts == AV_NOPTS_VALUE ? 0 : av_rescale_q(packet->pts, timeBase, AVRational{1, 1000});
			int64_t duration = av_rescale_q(packet->duration, timeBase, AVRational{1, 1000});
			media.processSubsData(reinterpret_cast<char *>(packet->data), packet->size, timestamp, duration);
		}
		av_packet_free(&packet);
	} else if (id == InvalidEntry
#ifdef __vita__
	           || (id == VideoEntry && media.nativeVideo.isOpen())
#endif
	) {
		av_packet_free(&packet);
	} else {
		enqueue(id, packet);
	}
}
