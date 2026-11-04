// SPDX-License-Identifier: GPL-3.0-or-later
// Where decoded audio goes: S16 stereo at audio::rate. write() blocks at real-time pace.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace audio {
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool write(const std::int16_t* frames, int count) = 0;
    virtual void flush() = 0;           // drop queued audio now (pause, seek, next song)
    virtual std::string describe() = 0; // state for the UI/log
    virtual void idle() {}              // nothing to play for a while: release the output
};

// Host build: a WAV file, paced like a speaker would be (tests and the Mac harness).
class FileSink : public Sink {
public:
    explicit FileSink(const char* path, bool realtime);
    ~FileSink() override;
    bool write(const std::int16_t* frames, int count) override;
    void flush() override;
    std::string describe() override;
    long long frames_written = 0, flushes = 0;

private:
    FILE* f = nullptr;
    bool realtime;
    std::uint64_t start_us = 0;
    long long paced = 0;
};
}
