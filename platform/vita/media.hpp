#pragma once
#include <SDL2/SDL.h>
#include <psp2/avplayer.h>
#include <psp2/gxm.h>
#include <cstdint>
#include <vector>
#include <string>
struct SwsContext;

// AvPlayer owns compressed H.264 decoding; the engine still owns mixing,
// stream selection, subtitles, scaling, compositing, and script timing.
class VitaMediaPlayer {
public:
    struct Frame { int width{}, height{}; uint64_t timestamp{}; };
    ~VitaMediaPlayer() { close(); }
    bool open(const char *path, uint64_t durationMs = 0);
    bool start(bool loop);
    bool readFrame(Frame &frame, bool &eof);
    bool bindFrame(unsigned texture);
    bool convertFrame(SDL_Surface *surface);
    void finishFrameUse();
    void close();
    bool isOpen() const { return handle != 0; }
    const char *error() const { return failure; }
    uint64_t timestamp() const { return current.timeStamp; }
    uint64_t duration() const { return expectedDuration; }
private:
    struct Block { void *base{}; int uid{-1}; uint32_t size{}; bool mapped{}; } blocks[8];
    static void *allocate(void *, uint32_t alignment, uint32_t size);
    static void release(void *, void *ptr);
    static void *allocateFrame(void *, uint32_t alignment, uint32_t size);
    static void releaseFrame(void *, void *ptr);
    SceAvPlayerHandle handle{};
    SDL_mutex *memoryMutex{};
    SwsContext *scaler{};
    SceAvPlayerFrameInfo current{};
    SceGxmTexture *texture{};
    SceGxmTexture savedTexture{};
    unsigned textureId{};
    bool gpuReadPending{}, swapUV{};
    std::vector<uint8_t> staging;
    void releaseTexture();
    bool frameInBlock(const void *pixels, size_t bytes, bool requireMapped);
    uint64_t expectedDuration{};
    std::string source;
    uint32_t began{}, lastProgress{};
    bool started{}, receivedFrame{};
    const char *failure{nullptr};
};
