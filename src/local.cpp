// SPDX-License-Identifier: GPL-3.0-or-later
#include "i18n.hpp"
#include "local.hpp"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace local {
std::string root = "/user/data/music";

static std::uint64_t now_ms() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return std::uint64_t(t.tv_sec) * 1000 + std::uint64_t(t.tv_nsec) / 1000000;
}

static std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

bool is_audio(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    static const char* ext[] = {"mp3", "flac", "m4a", "mp4", "aac", "ogg", "opus", "oga", "webm", "mka", "wav"};
    const std::string e = lower(name.substr(dot + 1));
    for (const char* x : ext) if (e == x) return true;
    return false;
}

// Entries of a folder, without hidden ones, sorted. `dirs`: folders, else files.
static std::vector<std::string> list(const std::string& dir, bool dirs) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        struct stat st {};
        if (stat((dir + "/" + e->d_name).c_str(), &st)) continue;
        if (S_ISDIR(st.st_mode) == dirs && (dirs || S_ISREG(st.st_mode))) out.emplace_back(e->d_name);
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    return out;
}

static bool has_songs(const std::string& dir) {
    for (const auto& f : list(dir, false)) if (is_audio(f)) return true;
    return false;
}

std::vector<Album> albums() {
    // ponytail: two levels (artist/album, plus songs loose in an artist folder); deeper trees when someone has one.
    std::vector<Album> out;
    if (has_songs(root)) out.push_back({root, "", TR("Loose songs", "Sueltas")});
    for (const auto& artist : list(root, true)) {
        const std::string a = root + "/" + artist;
        if (has_songs(a)) out.push_back({a, artist, artist});
        for (const auto& album : list(a, true))
            if (has_songs(a + "/" + album)) out.push_back({a + "/" + album, artist, album});
    }
    return out;
}

std::vector<Track> songs(const std::string& dir) {
    std::vector<Track> out;
    // The folder name tells the artist: <root>/<artist>[/<album>].
    std::string artist;
    if (dir.size() > root.size() + 1 && dir.compare(0, root.size() + 1, root + "/") == 0) {
        artist = dir.substr(root.size() + 1);
        artist = artist.substr(0, artist.find('/'));
    }
    for (const auto& f : list(dir, false)) {
        if (!is_audio(f)) continue;
        Track t;
        t.id = dir + "/" + f;
        t.title = f.substr(0, f.rfind('.'));
        t.artists = artist;
        out.push_back(std::move(t));
    }
    return out;
}

std::string cover(const std::string& dir) {
    for (const char* n : {"cover.jpg", "folder.jpg", "cover.png", "folder.png", "cover.jpeg"}) {
        const std::string p = dir + "/" + n;
        if (access(p.c_str(), R_OK) == 0) return p;
    }
    return "";
}

bool Queue::active() { std::lock_guard<std::mutex> l(m); return on; }
PlayerState Queue::snapshot() { std::lock_guard<std::mutex> l(m); return s; }
std::uint64_t Queue::version() { std::lock_guard<std::mutex> l(m); return s.version; }

void Queue::go(int index) {
    s.current_index = index;
    s.current_id = s.queue[std::size_t(index)].id;
    s.duration_ms = s.queue[std::size_t(index)].duration_ms;
    s.position_ms = 0;
    s.position_at = now_ms();
    ++s.version;
}

void Queue::play(std::vector<Track> list, int index) {
    if (list.empty()) return;
    std::lock_guard<std::mutex> l(m);
    start(std::move(list), index);
}

void Queue::add(const Track& t, bool next) {
    std::lock_guard<std::mutex> l(m);
    if (!on || s.queue.empty()) { start({t}, 0); return; }
    s.queue.insert(next ? s.queue.begin() + s.current_index + 1 : s.queue.end(), t);
    ++s.version;
}

void Queue::jump(int index) {
    std::lock_guard<std::mutex> l(m);
    if (!on || index < 0 || index >= int(s.queue.size())) return;
    s.playing = true;
    go(index);
}

void Queue::prune() {
    std::lock_guard<std::mutex> l(m);
    if (!on) return;
    std::vector<Track> kept;
    int current = -1, before = 0; // the current song's place among the kept ones; kept songs ahead of it
    for (int i = 0; i < int(s.queue.size()); ++i) {
        if (access(s.queue[std::size_t(i)].id.c_str(), F_OK) != 0) continue;
        if (i == s.current_index) current = int(kept.size());
        if (i < s.current_index) ++before;
        kept.push_back(s.queue[std::size_t(i)]);
    }
    if (kept.size() == s.queue.size()) return;
    if (kept.empty()) { // nothing left: back to the phone
        on = false;
        s.queue.clear();
        s.current_index = -1;
        s.current_id.clear();
        s.playing = false;
        ++s.version;
        return;
    }
    s.queue = std::move(kept);
    if (current >= 0) { s.current_index = current; ++s.version; }
    else go(std::min(before, int(s.queue.size()) - 1)); // the playing song is gone: the one after it
}

void Queue::start(std::vector<Track> list, int index) {
    on = true;
    const auto version = s.version;
    s = PlayerState{};
    s.version = version;
    s.connected = true;
    s.output = "pc";
    s.repeat = "off";
    s.speed = 1;
    s.queue = std::move(list);
    s.playing = true;
    go(std::clamp(index, 0, int(s.queue.size()) - 1));
}

void Queue::stop() {
    std::lock_guard<std::mutex> l(m);
    if (!on) return;
    on = false;
    s.playing = false;
    ++s.version;
}

void Queue::toggle() {
    std::lock_guard<std::mutex> l(m);
    if (!on) return;
    const auto now = now_ms();
    s.position_ms = s.position_now(now);
    s.position_at = now;
    s.playing = !s.playing;
    ++s.version;
}

void Queue::next() {
    std::lock_guard<std::mutex> l(m);
    if (on && s.current_index + 1 < int(s.queue.size())) go(s.current_index + 1);
}

void Queue::previous() {
    std::lock_guard<std::mutex> l(m);
    if (!on) return;
    if (s.position_now(now_ms()) > 3000 || s.current_index == 0) go(s.current_index); // like the phone: restart first
    else go(s.current_index - 1);
}

void Queue::seek(long long ms) {
    std::lock_guard<std::mutex> l(m);
    if (!on) return;
    s.position_ms = std::max(0LL, ms);
    s.position_at = now_ms();
    ++s.version;
}

void Queue::ended() {
    std::lock_guard<std::mutex> l(m);
    if (!on) return;
    if (s.current_index + 1 < int(s.queue.size())) go(s.current_index + 1);
    else { // the end of the album
        s.playing = false;
        s.position_ms = s.duration_ms > 0 ? s.duration_ms : 0;
        s.position_at = now_ms();
        ++s.version;
    }
}

void Queue::duration(const std::string& id, long long ms) {
    std::lock_guard<std::mutex> l(m);
    if (!on || ms <= 0) return;
    for (auto& t : s.queue) if (t.id == id) t.duration_ms = ms;
    if (s.current_id == id && s.duration_ms != ms) { s.duration_ms = ms; ++s.version; }
}
}
