/**
 *  AudioDecoder.cpp
 *  ONScripter-RU
 *
 *  Contains Media Engine audio decoder.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Media/Controller.hpp"

bool MediaProcController::AudioDecoder::initSwrContext(const AudioSpec &audioSpec) {
	if (av_channel_layout_check(&codecContext->ch_layout) == 0)
		av_channel_layout_default(&codecContext->ch_layout, 2);
	if (codecContext->sample_rate != audioSpec.frequency ||
	    codecContext->sample_fmt != audioSpec.format ||
	    av_channel_layout_compare(&codecContext->ch_layout, &audioSpec.channelLayout) != 0) {
		if (swr_alloc_set_opts2(&swrContext, &audioSpec.channelLayout, audioSpec.format, audioSpec.frequency,
		                       &codecContext->ch_layout, codecContext->sample_fmt, codecContext->sample_rate, 0, nullptr) < 0 ||
		    swr_init(swrContext) < 0) {
			swr_free(&swrContext);
			return false;
		}
	}
	return true;
}

void MediaProcController::AudioDecoder::processFrame(MediaFrame &vf) {
	uint8_t *output{nullptr};
	uint32_t outputSize{0};

	if (swrContext) {
		int64_t out_samples = static_cast<int64_t>(av_rescale_rnd(swr_get_delay(swrContext, codecContext->sample_rate) + frame->nb_samples,
		                                                          media.audioSpec.frequency, codecContext->sample_rate, AV_ROUND_UP));
		//Warning: further out_samples usage may loose precision
		if (av_samples_alloc(&output, nullptr, media.audioSpec.channels, static_cast<int32_t>(out_samples), media.audioSpec.format, 0) < 0) return;
		out_samples = swr_convert(swrContext, &output, static_cast<int32_t>(out_samples),
		                          const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
		if (out_samples < 0) { av_freep(&output); return; }

		outputSize = av_samples_get_buffer_size(nullptr, media.audioSpec.channels,
		                                        static_cast<int32_t>(out_samples), media.audioSpec.format, 1);
	} else {
		outputSize = av_samples_get_buffer_size(nullptr, codecContext->ch_layout.nb_channels,
		                                        frame->nb_samples, codecContext->sample_fmt, 1);
		output     = static_cast<uint8_t *>(av_malloc(outputSize));
		if (!output) return;
		std::memcpy(output, frame->data[0], outputSize);
	}
	vf.data        = output;
	vf.dataSize    = outputSize;
	vf.dataDeleter = [](uint8_t *d) {
		av_freep(&d);
	};
	vf.frameNumber = ++debugFrameNumber;
}
