// SPDX-License-Identifier: GPL-3.0-or-later
// PS5: audio through the worker injected into SceShellUI (heard over games).
#pragma once
#include "../shell/host.hpp"
#include "sink.hpp"
#include <atomic>

namespace audio {
class ShellSink : public Sink {
public:
    explicit ShellSink(shell::Host& host, int ownership) : host(host), ownership(ownership), wanted(ownership) {}
    ~ShellSink() override;
    bool write(const std::int16_t* frames, int count) override;
    void flush() override;
    std::string describe() override;
    // Takes effect on the next write: the worker sets the new mode up again.
    void set_ownership(int o) { wanted = o; } // any thread
    void idle() override;

private:
    shell::Host& host;
    int ownership, fd = -1;
    std::atomic<int> wanted;
    bool started = false;
    int failures = 0;
};
}
