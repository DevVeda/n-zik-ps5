// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell_sink.hpp"
#include "../log.hpp"
#include "../net.hpp"
#include <unistd.h>

namespace audio {
ShellSink::~ShellSink() {
    if (fd >= 0) close(fd);
    if (started) host.command(shell::Stop, ownership);
}
bool ShellSink::write(const std::int16_t* frames, int count) {
    if (wanted != ownership) { // set the worker up again in the new mode
        ownership = wanted;
        if (fd >= 0) close(fd);
        fd = -1;
        started = false;
    }
    if (!started) { // the worker opens the port (and its ownership source) on the first audio
        host.command(shell::Pcm, ownership);
        started = true;
        usleep(300000); // the worker polls commands every 100 ms; don't catch its old listener
        for (int i = 0; i < 50 && fd < 0; ++i, usleep(100000)) fd = host.connect_pcm();
    }
    if (fd < 0) fd = host.connect_pcm();
    if (fd < 0) return false;
    // Blocks while the worker's buffer is full: that's our real-time pacing.
    if (net::send_all(fd, frames, std::size_t(count) * 4)) { failures = 0; return true; }
    close(fd);
    fd = -1;
    // The worker may have stopped its port (output error, ShellUI hiccup): after a few
    // failures, set it up again from scratch.
    if (++failures >= 3) { started = false; failures = 0; }
    return false;
}
void ShellSink::flush() {
    // A new connection makes the worker drop what it had queued.
    if (fd >= 0) close(fd);
    fd = started ? host.connect_pcm() : -1;
}
void ShellSink::idle() {
    // Close the port and the silent system-music source until there's music again.
    if (!started) return;
    if (fd >= 0) close(fd);
    fd = -1;
    started = false;
    host.command(shell::Stop, ownership);
}
std::string ShellSink::describe() {
    shell::Status s;
    if (!host.status(s)) return "worker sin respuesta";
    char text[160];
    std::snprintf(text, sizeof text, "%s, buffer %d ms, cortes %d, open=%08x", s.playing ? "sonando" : "parado", s.queued * 1000 / 48000,
                  s.underruns, s.rc_open);
    return text;
}
}
