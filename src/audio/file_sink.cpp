// SPDX-License-Identifier: GPL-3.0-or-later
#include "decoder.hpp"
#include "sink.hpp"
#include <ctime>
#include <unistd.h>

namespace audio {
static std::uint64_t micros() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return std::uint64_t(t.tv_sec) * 1000000 + std::uint64_t(t.tv_nsec) / 1000;
}
static void header(FILE* f, std::uint32_t data) {
    unsigned char h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                           0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    auto put = [&](int at, std::uint32_t v) { for (int i = 0; i < 4; ++i) h[at + i] = (v >> (8 * i)) & 255; };
    put(4, data + 36); put(24, rate); put(28, rate * 4); put(40, data);
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(h, 1, 44, f);
    std::fseek(f, 0, SEEK_END);
}
FileSink::FileSink(const char* path, bool rt) : f(std::fopen(path, "wb")), realtime(rt) {
    if (f) header(f, 0);
}
FileSink::~FileSink() {
    if (!f) return;
    header(f, std::uint32_t(frames_written * 4));
    std::fclose(f);
}
bool FileSink::write(const std::int16_t* frames, int count) {
    if (!f || std::fwrite(frames, 4, std::size_t(count), f) != std::size_t(count)) return false;
    frames_written += count;
    if (realtime) { // like the console's speaker: a little ahead of real time, never more
        if (!start_us) start_us = micros();
        paced += count;
        const std::uint64_t due = start_us + std::uint64_t(paced) * 1000000 / rate, now = micros();
        if (due > now + 100000) usleep(unsigned(due - now - 100000));
        else if (now > due + 500000) { start_us = now; paced = 0; } // was idle
    }
    return true;
}
void FileSink::flush() { ++flushes; start_us = 0; paced = 0; }
std::string FileSink::describe() { return "archivo, " + std::to_string(frames_written / rate) + " s"; }
}
