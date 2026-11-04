// SPDX-License-Identifier: GPL-3.0-or-later
// Decodes an HTTP audio URL (webm/opus, mp4/aac, mp3, flac, ogg, wav) to S16 stereo 48 kHz.
// FFmpeg demuxes and decodes; the bytes come from our own HTTP client (Range for seeking),
// because FFmpeg's network code needs non-blocking ioctls the console refuses.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace audio {
constexpr int rate = 48000;

class Decoder {
public:
    Decoder();
    ~Decoder();
    bool open(const std::string& url, long long start_ms);
    // Up to `frames` stereo frames into out. 0: end of track; -1: error.
    int read(std::int16_t* out, int frames);
    bool seek(long long ms);
    long long position_ms() const; // of the next frame read() returns
    long long duration_ms() const;
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
}
