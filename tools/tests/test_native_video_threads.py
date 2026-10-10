"""Exercise production decoder selection at the FFmpeg open boundary."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class VideoThreadTests(unittest.TestCase):
    def test_software_decode_avoids_pthread_waits_on_sdl_vita_worker(self):
        source = (ROOT / 'upstream/onscripter-ru/Engine/Media/Controller.cpp').read_text()
        start = source.index('std::unique_ptr<MediaProcController::Decoder> MediaProcController::findDecoder(')
        method = source[start:source.index('\n}\n', start) + 3]
        harness = r'''
#include <cassert>
#include <memory>
#include <stdexcept>
enum AVMediaType { AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO, AVMEDIA_TYPE_SUBTITLE };
enum AVCodecID { AV_CODEC_ID_NONE, AV_CODEC_ID_MPEG2VIDEO, AV_CODEC_ID_H264 };
struct AVCodecParameters { AVMediaType codec_type; AVCodecID codec_id; };
struct AVCodecContext { int thread_count = 1; AVCodecParameters params; };
struct Stream { AVCodecParameters *codecpar; };
struct Format { unsigned nb_streams; Stream **streams; };
AVCodecContext *avcodec_alloc_context3(void*) { return new AVCodecContext; }
int avcodec_parameters_to_context(AVCodecContext *c, AVCodecParameters *p) {
    c->params = *p; return 0;
}
int softwareOpens = 0;
struct MediaProcController {
    Format *formatContext;
    struct { bool open = false; bool isOpen() { return open; } } nativeVideo;
    struct Decoder {
        AVCodecContext *context; bool software; unsigned stream;
        Decoder(AVCodecContext *c, const void *codec, unsigned s)
            : context(c), software(codec != nullptr), stream(s) {}
        virtual ~Decoder() { delete context; }
        template<class T> static std::unique_ptr<Decoder> create(AVCodecContext *c, unsigned s) {
            ++softwareOpens;
            // Model FFmpeg's thread activation at open, before any packets are
            // decoded. Vita's SDL caller cannot safely wait for pthread workers.
            if (c->params.codec_type == AVMEDIA_TYPE_VIDEO) {
#ifdef __vita__
                assert(c->thread_count == 1);
#else
                assert(c->thread_count == 0); // Preserve desktop auto threading.
#endif
            }
            return std::make_unique<T>(c, c, s);
        }
    };
    struct VideoDecoder : Decoder { using Decoder::Decoder; };
    struct AudioDecoder : Decoder { using Decoder::Decoder; };
    struct SubtitleDecoder : Decoder { using Decoder::Decoder; };
    std::unique_ptr<Decoder> findDecoder(AVMediaType, unsigned = 1, AVCodecID = AV_CODEC_ID_NONE);
};
'''
        harness += method + r'''
int main() {
    AVCodecParameters audio{AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_NONE};
    AVCodecParameters video{AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG2VIDEO};
    AVCodecParameters subs{AVMEDIA_TYPE_SUBTITLE, AV_CODEC_ID_NONE};
    Stream a{&audio}, v{&video}, s{&subs}; Stream *streams[]{&a, &v, &s};
    Format format{3, streams}; MediaProcController media{&format};
    for (auto codec : {AV_CODEC_ID_MPEG2VIDEO, AV_CODEC_ID_H264}) {
        video.codec_id = codec;
        // Reopening and software H.264 fallback must use the same safe policy.
        for (int n = 0; n < 3; ++n) {
            auto d = media.findDecoder(AVMEDIA_TYPE_VIDEO);
            assert(d && d->software && d->stream == 1);
        }
    }
    auto ad = media.findDecoder(AVMEDIA_TYPE_AUDIO);
    assert(ad && ad->software && ad->context->thread_count == 1 && ad->stream == 0);
    auto sd = media.findDecoder(AVMEDIA_TYPE_SUBTITLE);
    assert(sd && !sd->software && sd->stream == 2);
    assert(!media.findDecoder(AVMEDIA_TYPE_VIDEO, 2));
    assert(!media.findDecoder(AVMEDIA_TYPE_VIDEO, 1, AV_CODEC_ID_MPEG2VIDEO));
#ifdef __vita__
    media.nativeVideo.open = true;
    const int before = softwareOpens;
    auto hw = media.findDecoder(AVMEDIA_TYPE_VIDEO);
    assert(hw && !hw->software && softwareOpens == before);
#endif
}
'''
        with tempfile.TemporaryDirectory(prefix='ons-video-threads-') as directory:
            path = Path(directory)
            (path / 'test.cpp').write_text(harness)
            for defines in (['-D__vita__'], []):
                subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
                                '-fsanitize=address,undefined', *defines,
                                str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
                subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
