// SPDX-License-Identifier: GPL-3.0-or-later
#include "i18n.hpp"
#include "offline.hpp"
#include "library.hpp"
#include "local.hpp"
#include <dirent.h>
#include "log.hpp"
#include "net.hpp"
#include "notify.hpp"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace offline {
static std::mutex m;
static std::string running; // the song being saved (one at a time), "" when none
constexpr std::uint64_t max_bytes = 300u << 20; // no song is this big; a runaway stream is

std::string clean(const std::string& name) {
    std::string s;
    for (const unsigned char c : name) s += c < 0x20 || c == 0x7f || std::strchr("/\\:*?\"<>|", c) ? '_' : char(c);
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    std::size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '.')) ++start; // no "..", no hidden files
    s = s.substr(start);
    if (s.size() > 120) { // cut on a UTF-8 boundary
        std::size_t n = 120;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
        s.resize(n);
    }
    return s.empty() ? "Desconocido" : s;
}

std::string artist_folder(const Track& t) {
    std::string artist = t.artists;
    const auto comma = artist.find(','); // "A, B": the first artist names the folder
    if (comma != std::string::npos) artist.resize(comma);
    return local::root + "/" + clean(artist);
}

bool saved(const Track& t) {
    const std::string dir = artist_folder(t), name = "/" + clean(t.title) + ".";
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    bool found = false;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        for (const char* x : {"webm", "m4a", "mp3", "ogg", "opus", "flac"})
            if (access((dir + "/" + e->d_name + name + x).c_str(), F_OK) == 0) found = true;
    }
    closedir(d);
    return found;
}

bool busy(const std::string& id) {
    std::lock_guard<std::mutex> l(m);
    return !running.empty() && running == id;
}

// The container, from the first bytes (the phone's Content-Type is not always precise).
static const char* sniff(const unsigned char* b, long n) {
    if (n >= 4 && b[0] == 0x1A && b[1] == 0x45 && b[2] == 0xDF && b[3] == 0xA3) return "webm";
    if (n >= 8 && !std::memcmp(b + 4, "ftyp", 4)) return "m4a";
    if (n >= 4 && !std::memcmp(b, "OggS", 4)) return "ogg";
    if (n >= 4 && !std::memcmp(b, "fLaC", 4)) return "flac";
    if (n >= 3 && (!std::memcmp(b, "ID3", 3) || (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0))) return "mp3";
    return nullptr;
}

static bool download(Bridge& b, const Track& t, std::string& why) {
    const std::string url = b.audio_url(t.id, "high");
    net::Stream st;
    if (url.empty() || !st.open(url, 0, 15000)) { why = TR("the phone did not give the audio", "el telefono no dio el audio"); return false; }
    const std::string album = library::album_of(b, t);
    const std::string dir = artist_folder(t) + "/" + clean(album.empty() ? "Singles" : album);
    mkdir(local::root.c_str(), 0777);
    mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0777);
    mkdir(dir.c_str(), 0777);
    const std::string part = dir + "/.download.part";
    std::FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) { why = TR("cannot write to ", "no puedo escribir en ") + dir; return false; }
    std::vector<unsigned char> mem(64 * 1024);
    unsigned char* buf = mem.data();
    const char* ext = nullptr;
    std::uint64_t total = 0;
    long n;
    while ((n = st.read(buf, mem.size())) > 0) {
        if (!ext && !(ext = sniff(buf, n))) { why = TR("unknown format", "formato desconocido"); break; }
        if (std::fwrite(buf, 1, std::size_t(n), f) != std::size_t(n)) { why = TR("disk full?", "disco lleno?"); break; }
        if ((total += std::uint64_t(n)) > max_bytes) { why = TR("too big", "demasiado grande"); break; }
    }
    const bool complete = n == 0 && ext && (!st.head.total || total == st.head.total);
    if (std::fclose(f) != 0 || !complete) {
        if (why.empty()) why = TR("the download was cut", "la descarga se corto");
        std::remove(part.c_str());
        return false;
    }
    const std::string file = dir + "/" + clean(t.title) + "." + ext;
    if (std::rename(part.c_str(), file.c_str())) { why = TR("could not rename", "no pude renombrar"); std::remove(part.c_str()); return false; }
    logf("OFFLINE saved %s (%llu bytes)", file.c_str(), (unsigned long long)total);
    // The cover, once per folder; a song without one is still saved.
    std::string bytes, type;
    if (local::cover(dir).empty() && t.has_art && b.artwork(t.id, 544, bytes, type)) {
        const std::string cover = dir + (type.find("png") != std::string::npos ? "/cover.png" : "/cover.jpg");
        if (std::FILE* c = std::fopen(cover.c_str(), "wb")) {
            const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), c) == bytes.size();
            if (std::fclose(c) != 0 || !ok) std::remove(cover.c_str());
        }
    }
    return true;
}

struct Job {
    Bridge* bridge;
    Track track;
};

static void* run(void* p) {
    Job* job = static_cast<Job*>(p);
    std::string why;
    if (download(*job->bridge, job->track, why)) notify(TR("Saved offline: %s", "Guardada offline: %s"), job->track.title.c_str());
    else {
        logf("OFFLINE %s failed: %s", job->track.id.c_str(), why.c_str());
        notify(TR("Could not save %s: %s", "No pude guardar %s: %s"), job->track.title.c_str(), why.c_str());
    }
    {
        std::lock_guard<std::mutex> l(m);
        running.clear();
    }
    delete job;
    return nullptr;
}

void save_async(Bridge& b, const Track& t) {
    {
        std::lock_guard<std::mutex> l(m);
        // ponytail: one download at a time keeps the Wi-Fi for the song that's playing.
        if (!running.empty() || saved(t)) return;
        running = t.id;
    }
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    auto* job = new Job{&b, t};
    if (pthread_create(&th, &a, run, job)) {
        delete job;
        std::lock_guard<std::mutex> l(m);
        running.clear();
    }
    pthread_attr_destroy(&a);
}
}
