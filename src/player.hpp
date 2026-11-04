// SPDX-License-Identifier: GPL-3.0-or-later
// Plays what the phone plays: follows the bridge state (track, play/pause, position) and
// streams that track from the phone into the sink. The phone stays the source of truth,
// except while a local queue (songs on the console) is playing: then it follows that one.
#pragma once
#include "audio/decoder.hpp"
#include "audio/sink.hpp"
#include "bridge.hpp"
#include "local.hpp"
#include <atomic>
#include <mutex>
#include <string>

class Player {
public:
    Player(Bridge& bridge, local::Queue& local, audio::Sink& sink) : bridge(bridge), local(local), sink(sink) {}
    void run(); // forever, on its own thread
    std::atomic<int> volume{100}; // 0..100, applied here (the console's own volume still applies)
    std::atomic<bool> low_quality{false};
    std::atomic<bool> song_toasts{true};
    std::string status(); // for the UI
    // Unit-test hooks: one step of the loop with an explicit clock.
    void step(std::uint64_t now);

private:
    Bridge& bridge;
    local::Queue& local;
    audio::Sink& sink;
    audio::Decoder decoder;
    PlayerState state;
    std::uint64_t seen = ~0ull, failed_at = 0, drift_checked = 0, silent_since = 0;
    std::string loaded, failed, toasted;
    std::uint64_t seen_phone = ~0ull;
    bool paused = true, ended = false, was_local = false, phone_playing = false;
    std::mutex m;
    std::string message = "En espera";
    void say(const std::string& s);
    void unload();
    void quiet(std::uint64_t now); // nothing to play: release the output after a while
};
