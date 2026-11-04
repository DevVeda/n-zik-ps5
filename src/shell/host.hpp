// SPDX-License-Identifier: GPL-3.0-or-later
// Host side of the ShellUI audio worker: resolves the console functions inside ShellUI,
// injects the worker, keeps it alive with a heartbeat and serves its silent WAV.
#pragma once
#include "protocol.hpp"
#include <atomic>
#include <string>

namespace shell {
class Host {
public:
    // Injects the worker. `info` describes firmware, exports and the injection, for the UI/log.
    bool start(std::string& info);
    void command(int action, int ownership);
    bool status(Status& out) const;
    bool has(Fn f) const { return request.fn[f] != 0; }
    // A fresh PCM connection to the worker (each one starts a new stream). -1 if not there.
    int connect_pcm() const;
    ~Host();

private:
    Request request;
    std::uint64_t seq = 0;
    std::atomic<bool> quitting{false};
    static void* heartbeat(void*);
    static void* wav_server(void*);
};
}
