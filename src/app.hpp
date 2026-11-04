// SPDX-License-Identifier: GPL-3.0-or-later
// What the threads share: settings, the bridge, the player and the pairing screen state.
#pragma once
#include "bridge.hpp"
#include "config.hpp"
#include "player.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

// Under /data/n-zik-ps5 on the console; the host build points them at a temp folder.
namespace paths {
inline std::string root, config, pairing, art, qr, log, stop;
inline void set_root(const std::string& r) {
    root = r;
    config = r + "/config.txt";
    pairing = r + "/pairing.txt";
    art = r + "/art";
    qr = r + "/art/pair-qr.png";
    log = r + "/n-zik-ps5.log";
    stop = r + "/stop"; // create it to stop the payload
}
}

struct App {
    Config config;
    Bridge& bridge;
    Player& player;
    local::Queue& local;
    std::atomic<bool> quit{false};
    std::atomic<bool> pair_requested{false};
    std::string audio_info; // firmware/worker summary for the settings tile
    std::function<void(int)> on_ownership; // the audio mode changed (PS5 build restarts its sink)

    std::mutex m;
    std::string pair_status; // "" when not pairing
    std::string pair_qr;     // PNG path while a QR is showing
    std::uint64_t pair_version = 0;

    App(Bridge& b, Player& p, local::Queue& l) : bridge(b), player(p), local(l) {}
    void set_pairing_screen(const std::string& status, const std::string& qr) {
        std::lock_guard<std::mutex> l(m);
        pair_status = status;
        pair_qr = qr;
        ++pair_version;
    }
    void save() {
        config.volume = player.volume;
        config.low_quality = player.low_quality;
        config.song_toasts = player.song_toasts;
        config.save(paths::config.c_str());
    }
};
