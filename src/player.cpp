// SPDX-License-Identifier: GPL-3.0-or-later
#include "i18n.hpp"
#include "player.hpp"
#include "log.hpp"
#include "notify.hpp"
#include <algorithm>
#include <cmath>
#include <unistd.h>

static std::uint64_t now_ms() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return std::uint64_t(t.tv_sec) * 1000 + std::uint64_t(t.tv_nsec) / 1000000;
}

// Our position may run ahead of the phone's by what the sink holds (~0.6 s), and the phone's
// is extrapolated between messages; beyond this we jump to where the phone is.
constexpr long long max_drift_ms = 3000;
constexpr int chunk = 1024;

void Player::say(const std::string& s) {
    std::lock_guard<std::mutex> l(m);
    if (message != s) logf("PLAYER %s", s.c_str());
    message = s;
}
std::string Player::status() {
    std::lock_guard<std::mutex> l(m);
    return message;
}
void Player::unload() {
    if (!loaded.empty()) {
        decoder.close();
        sink.flush();
        loaded.clear();
    }
    paused = true;
}

constexpr std::uint64_t idle_after_ms = 120000;
void Player::quiet(std::uint64_t now) {
    if (!silent_since) silent_since = now;
    else if (now - silent_since >= idle_after_ms && silent_since != 1) { sink.idle(); silent_since = 1; } // 1: released
}

void Player::run() {
    for (;;) step(now_ms());
}

void Player::step(std::uint64_t now) {
    // The phone starting to play takes over from local music.
    if (bridge.version() != seen_phone) {
        const PlayerState phone = bridge.snapshot();
        seen_phone = phone.version;
        if (phone.playing && !phone_playing) local.stop();
        phone_playing = phone.playing;
    }
    const bool use_local = local.active();
    const std::uint64_t ver = use_local ? local.version() : bridge.version();
    if (ver != seen || use_local != was_local) {
        state = use_local ? local.snapshot() : bridge.snapshot();
        seen = ver;
        was_local = use_local;
    }
    const Track* track = state.current();
    if (!state.connected || state.output != "pc" || !track) {
        unload();
        quiet(now);
        say(!state.connected ? TR("No phone", "Sin telefono") : state.output != "pc" ? TR("Moving the audio to the PS5...", "Pasando el audio al PS5...") : TR("Nothing in the queue", "Nada en la cola"));
        usleep(50000);
        return;
    }
    if (loaded != track->id) {
        unload();
        if (failed == track->id && now - failed_at < 5000) { usleep(50000); return; } // don't hammer a broken track
        say(TR("Loading ", "Cargando ") + track->title);
        const std::string url = use_local ? track->id : bridge.audio_url(track->id, low_quality ? "low" : "high");
        // The phone moves on while we fetch: start where it is now.
        if (url.empty() || !decoder.open(url, state.position_now(now_ms()))) {
            failed = track->id;
            failed_at = now_ms();
            say(TR("Could not open ", "No pude abrir ") + track->title);
            return;
        }
        loaded = track->id;
        ended = false;
        if (use_local) local.duration(loaded, decoder.duration_ms());
        now = now_ms(); // the open blocked: don't compare against the old clock
        drift_checked = now;
    }
    // position_at may be a hair newer than `now` (a message landed in between): clamp.
    const long long target = state.position_now(std::max(now, state.position_at));
    if (!state.playing) {
        if (!paused) { sink.flush(); paused = true; }
        // Paused at the phone, then moved: be there when it resumes.
        if (std::llabs(decoder.position_ms() - target) > 500 && decoder.seek(target)) ended = false;
        quiet(now);
        say(TR("Paused", "En pausa"));
        usleep(50000);
        return;
    }
    paused = false;
    silent_since = 0;
    if (toasted != loaded) { // once per song, when it starts sounding
        toasted = loaded;
        if (song_toasts) notify("%s - %s", track->title.c_str(), track->artists.c_str());
    }
    if (now >= drift_checked + 1000) {
        drift_checked = now;
        const long long drift = decoder.position_ms() - target;
        if (drift > max_drift_ms || drift < -max_drift_ms) {
            logf("PLAYER drift %lld ms: seek to %lld", drift, target);
            sink.flush();
            if (decoder.seek(target)) ended = false;
        }
    }
    if (ended) { usleep(50000); return; } // the phone (or local.ended) moves to the next track
    alignas(16) std::int16_t pcm[chunk * 2];
    const int n = decoder.read(pcm, chunk);
    if (n <= 0) {
        ended = true;
        if (use_local) local.ended(); // we are the clock: move on ourselves
        if (n < 0) { // reopen where the phone is, but not in a tight loop
            say(TR("Audio read error; retrying", "Error leyendo el audio; reintento"));
            unload();
            failed = track->id;
            failed_at = now_ms();
        }
        return;
    }
    const int v = volume.load();
    if (v < 100) { // perceptual curve
        const int gain = int(65536.0 * std::pow(v / 100.0, 2.0));
        for (int i = 0; i < n * 2; ++i) pcm[i] = std::int16_t(pcm[i] * gain >> 16);
    }
    if (!sink.write(pcm, n)) { say(TR("The audio output is not answering", "La salida de audio no responde")); usleep(200000); return; }
    say(TR("Playing ", "Sonando ") + track->title);
}
