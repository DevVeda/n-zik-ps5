// SPDX-License-Identifier: GPL-3.0-or-later
// Client of N-Zik's local "PC bridge" (contract 1.x): the phone keeps the queue and plays
// silently while we are its audio output ("pc"); we follow its state over the WebSocket.
#pragma once
#include "json.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

struct Track {
    std::string id, title, artists;
    long long duration_ms = -1;
    bool has_art = false;
    bool liked = false;
};

struct PlayerState {
    std::uint64_t version = 0; // bumped on every change
    bool connected = false;
    std::string problem;       // why we aren't connected, for the UI
    long long revision = -1;
    std::vector<Track> queue;
    int current_index = -1;
    std::string current_id;
    bool playing = false, buffering = false;
    long long position_ms = 0, duration_ms = -1;
    std::uint64_t position_at = 0; // our monotonic ms when position_ms was true
    double speed = 1;
    std::string repeat = "off", output = "phone";
    bool shuffle = false;
    // Where the phone's (silent) player is now.
    long long position_now(std::uint64_t now) const {
        return playing ? position_ms + (long long)(double(now - position_at) * speed) : position_ms;
    }
    const Track* current() const {
        return current_index >= 0 && current_index < int(queue.size()) && queue[std::size_t(current_index)].id == current_id
                   ? &queue[std::size_t(current_index)] : nullptr;
    }
};

struct Pairing {
    std::vector<std::string> ips;
    int port = 0;
    std::string token, device_id, server_name;
    bool valid() const { return !ips.empty() && port > 0 && !token.empty(); }
    bool load(const char* file);
    bool save(const char* file) const;
};

// Ids that may go in a request line (YouTube ids, local numbers): no CR/LF header injection.
bool safe_id(const std::string& id);
// TrackDto object -> track; TrackDto array -> tracks (queue, library lists).
Track read_track(const json::Doc& d, int obj);
void read_tracks(const json::Doc& d, int arr, std::vector<Track>& out);

class Bridge {
public:
    explicit Bridge(const char* pairing_file) : file(pairing_file) { pairing.load(file); }
    // Runs forever: connects, follows the state, reconnects. Returns when unpaired (token revoked).
    void run();
    bool paired() { std::lock_guard<std::mutex> l(m); return pairing.valid(); }
    // Both drop the live connection: run() notices the new generation.
    void set_pairing(const Pairing& p) { std::lock_guard<std::mutex> l(m); pairing = p; pairing.save(file); ++generation; }
    void forget() { std::lock_guard<std::mutex> l(m); pairing = Pairing{}; std::remove(file); ++generation; }
    // Leaving: stop asking for the audio output and stop reconnecting.
    void stop() { stopping = true; }
    // The phone isn't at its address and a sweep of the network didn't find it either: the UI
    // offers a new pairing (the old one is kept, and used again if the phone comes back).
    std::atomic<bool> lost{false};
    PlayerState snapshot() { std::lock_guard<std::mutex> l(m); return state; }
    std::uint64_t version() { std::lock_guard<std::mutex> l(m); return state.version; }

    bool find_phone(Pairing& p);
    // REST commands (contract §9). `body` is a JSON object. Returns the HTTP status (0: no answer).
    int command(const char* route, const std::string& body = "{}");
    // GET /api/v1/<path> (library, contract §10). Returns the HTTP status (0: no answer).
    int get(const std::string& path, std::string& body);
    // An audio URL for a track (contract §8.1), "" on failure.
    std::string audio_url(const std::string& track_id, const char* quality = "high");
    // Track artwork bytes (contract §10).
    bool artwork(const std::string& track_id, int size, std::string& bytes, std::string& type);

    // Exposed for the host test.
    bool apply(const std::string& message, std::uint64_t now);

private:
    const char* file;
    std::mutex m;
    Pairing pairing;
    PlayerState state;
    std::string host; // the IP that answered
    int send_snapshot_request = 0;
    std::uint64_t generation = 0;
    std::atomic<bool> stopping{false};
    void problem(const std::string& why);
};
