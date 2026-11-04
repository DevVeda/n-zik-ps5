// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

// Appends a line to the log file (stderr on the host build).
inline const char* log_file = nullptr;
inline void logf(const char* format, ...) {
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    FILE* f = log_file ? std::fopen(log_file, "a") : stderr;
    if (!f) return;
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    std::fprintf(f, "[%lld.%03ld] ", (long long)t.tv_sec, t.tv_nsec / 1000000);
    va_list a;
    va_start(a, format);
    std::vfprintf(f, format, a);
    va_end(a);
    std::fputc('\n', f);
    if (f != stderr) {
        if (!std::fseek(f, 0, SEEK_END) && std::ftell(f) > 2 * 1024 * 1024) { std::fclose(f); std::remove(log_file); return; }
        std::fclose(f);
    }
}
