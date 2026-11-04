// SPDX-License-Identifier: GPL-3.0-or-later
#include "i18n.hpp"
#include "bridge.hpp"
#include <algorithm>
#include "json.hpp"
#include "log.hpp"
#include "net.hpp"
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

// pairing file: one "key value" per line; ips is space separated.
bool Pairing::load(const char* file) {
    FILE* f = std::fopen(file, "r");
    if (!f) return false;
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        line[std::strcspn(line, "\r\n")] = 0;
        char* v = std::strchr(line, ' ');
        if (!v) continue;
        *v++ = 0;
        if (!std::strcmp(line, "ips")) {
            ips.clear();
            for (char* ip = std::strtok(v, " "); ip; ip = std::strtok(nullptr, " ")) ips.push_back(ip);
        } else if (!std::strcmp(line, "port")) port = std::atoi(v);
        else if (!std::strcmp(line, "token")) token = v;
        else if (!std::strcmp(line, "device")) device_id = v;
        else if (!std::strcmp(line, "server")) server_name = v;
    }
    std::fclose(f);
    return valid();
}
bool Pairing::save(const char* file) const {
    const std::string tmp = std::string(file) + ".tmp"; // write then rename: never half a token
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "ips");
    for (auto& ip : ips) std::fprintf(f, " %s", ip.c_str());
    // One value per line: values from the phone must not carry newlines (or add keys).
    auto line = [](std::string v) { for (char& c : v) if (c == '\n' || c == '\r') c = ' '; return v; };
    std::fprintf(f, "\nport %d\ntoken %s\ndevice %s\nserver %s\n", port, line(token).c_str(), line(device_id).c_str(), line(server_name).c_str());
    return std::fclose(f) == 0 && std::rename(tmp.c_str(), file) == 0;
}

static std::uint64_t now_ms() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return std::uint64_t(t.tv_sec) * 1000 + std::uint64_t(t.tv_nsec) / 1000000;
}

void Bridge::problem(const std::string& why) {
    std::lock_guard<std::mutex> l(m);
    if (state.problem != why || state.connected) {
        state.problem = why;
        state.connected = false;
        ++state.version;
        logf("BRIDGE %s", why.c_str());
    }
}

Track read_track(const json::Doc& d, int i) {
    Track t;
    t.id = d.str(d.get(i, "id"));
    t.title = d.str(d.get(i, "title"));
    t.artists = d.str(d.get(i, "artists"));
    t.duration_ms = d.num(d.get(i, "durationMs"), -1);
    t.has_art = d.boolean(d.get(i, "hasArtwork"));
    const std::string like = d.str(d.get(i, "like"));
    t.liked = like == "liked";
    t.disliked = like == "disliked";
    return t;
}
void read_tracks(const json::Doc& d, int arr, std::vector<Track>& out) {
    out.clear();
    for (int i : d.items(arr)) out.push_back(read_track(d, i));
}

bool Bridge::apply(const std::string& message, std::uint64_t now) {
    json::Doc d;
    if (!d.parse(message)) return false;
    const std::string type = d.str(d.get(0, "type"));
    std::lock_guard<std::mutex> l(m);
    auto& s = state;
    const long long revision = d.num(d.get(0, "revision"), -1);
    const bool revised = type != "heartbeat" && type != "pong" && type != "error" && type != "toast" && type != "serverStopped";
    if (type != "snapshot" && revised) {
        // Deltas are exactly revision + 1 (contract §7.2); anything else: ask for a snapshot.
        if (s.revision < 0 || revision != s.revision + 1) { send_snapshot_request = 1; return true; }
    }
    if (type == "heartbeat") {
        // Not revised: a light resync of the position only.
        if (revision == s.revision) {
            const bool playing = d.boolean(d.get(0, "isPlaying"), s.playing);
            const long long pos = d.num(d.get(0, "positionMs"));
            // Tell the player only when it matters: a flip, or the phone drifted from our estimate.
            if (playing != s.playing || std::llabs(pos - s.position_now(now)) > 1000) ++s.version;
            s.position_ms = pos;
            s.position_at = now;
            s.playing = playing;
        }
        return true;
    }
    if (type == "snapshot" || type == "queueChanged") {
        read_tracks(d, d.get(0, "queue"), s.queue);
        s.current_index = int(d.num(d.get(0, "currentIndex"), -1));
        s.current_id = d.str(d.get(0, "currentTrackId"));
    }
    if (type == "snapshot" || type == "trackChanged") {
        s.current_index = int(d.num(d.get(0, "currentIndex"), s.current_index));
        if (d.get(0, "currentTrackId") >= 0) s.current_id = d.str(d.get(0, "currentTrackId"));
    }
    if (type == "snapshot" || type == "playbackChanged" || type == "trackChanged") {
        s.playing = d.boolean(d.get(0, "isPlaying"), s.playing);
        s.buffering = d.boolean(d.get(0, "isBuffering"), false);
        s.position_ms = d.num(d.get(0, "positionMs"), 0);
        s.position_at = now;
        s.duration_ms = d.num(d.get(0, "durationMs"), -1); // TIME_UNSET is negative: unknown
        if (d.get(0, "speed") >= 0) s.speed = std::atof(d.raw(d.get(0, "speed")).c_str());
    }
    if (type == "snapshot" || type == "modesChanged") {
        s.repeat = d.str(d.get(0, "repeatMode"));
        s.shuffle = d.boolean(d.get(0, "shuffle"));
    }
    if (type == "snapshot" || type == "outputChanged") s.output = d.str(d.get(0, "audioOutput"));
    if (type == "error") logf("BRIDGE late error %s: %s", d.str(d.get(0, "code")).c_str(), d.str(d.get(0, "message")).c_str());
    if (type == "serverStopped") logf("BRIDGE server stopped: %s", d.str(d.get(0, "code")).c_str());
    if (revised && revision >= 0) s.revision = revision;
    ++s.version;
    return true;
}

// The phone's IP changed (DHCP): look for it on our /24, on its port and on the bridge's first
// one (42420). A host counts only if it is the paired phone (its server name) and takes our
// token. The new address goes first in the pairing; the last few stay, tried before any sweep.
bool Bridge::find_phone(Pairing& p) {
    const std::string me = net::local_ip();
    if (me.empty()) return false;
    // ponytail: the phone's port and 42420; the bridge's 42421-42429 only if a phone ever needs them.
    std::vector<int> ports{p.port};
    if (p.port != 42420) ports.push_back(42420);
    for (const int port : ports) {
        if (stopping) return false;
        for (const auto& h : net::sweep(me, port, 400)) {
            net::Response r;
            json::Doc d;
            if (!net::request(h, port, "GET", "/api/v1/meta", "", "", r, 64 << 10, 3000) || r.status != 200 || !d.parse(r.body)) continue;
            if (!p.server_name.empty() && d.str(d.get(0, "serverName")) != p.server_name) continue;
            if (!net::request(h, port, "GET", "/api/v1/library/songs?limit=1", p.token, "", r, 1 << 20, 5000) || r.status != 200) continue;
            logf("BRIDGE found the phone at %s:%d", h.c_str(), port);
            std::lock_guard<std::mutex> l(m);
            if (pairing.token != p.token) return false; // paired again meanwhile
            std::vector<std::string> ips{h}; // this one first, then the last few it had
            for (const auto& old : pairing.ips)
                if (ips.size() < 4 && std::find(ips.begin(), ips.end(), old) == ips.end()) ips.push_back(old);
            pairing.ips = ips;
            pairing.port = port;
            pairing.save(file);
            p = pairing;
            return true;
        }
    }
    return false;
}

void Bridge::run() {
    unsigned attempt = 0;
    std::uint64_t missing_since = 0; // when the phone stopped answering
    while (!stopping) {
        Pairing p;
        std::uint64_t gen;
        {
            std::lock_guard<std::mutex> l(m);
            p = pairing;
            gen = generation;
        }
        if (!p.valid()) { problem(TR("Not paired", "Sin emparejar")); return; }
        net::WebSocket ws;
        int status = 0;
        std::string ip;
        for (auto& candidate : p.ips) {
            if (ws.connect(candidate, p.port, "/api/v1/ws", p.token, status)) { ip = candidate; break; }
            // Only the phone's own address (first: the one that answered last) can revoke us;
            // another device that took an old IP must not unpair the PS5.
            if (status == 401 && &candidate == &p.ips[0]) { problem(TR("The phone revoked this PS5: pair again", "El telefono revoco este PS5: empareja de nuevo")); forget(); return; }
            if (status == 409) break;
        }
        // Nobody answered where the phone was: it may have a new IP. Look for it after a few
        // tries, then now and then (~2.5 min). Offer a new pairing only after a minute without it:
        // most of the time the phone's bridge is just not on yet.
        if (ws.fd < 0 && status == 0) {
            if (!missing_since) missing_since = now_ms();
            if (attempt == 3 || (attempt > 3 && attempt % 30 == 0)) {
                problem(TR("Looking for the phone on the network...", "Buscando el telefono en la red..."));
                if (find_phone(p)) { attempt = 0; continue; }
                logf("BRIDGE the phone is not on the network");
            }
            if (now_ms() - missing_since >= 60000) lost = true;
        }
        if (ws.fd < 0) {
            problem(status == 409 ? TR("Another device is using the N-Zik bridge", "Otro dispositivo usa el puente de N-Zik") : TR("On the phone: N-Zik > your avatar > PC Server > Start server", "En el telefono: N-Zik > tu avatar > PC Server > Start server"));
            sleep(attempt++ < 5 ? 2 : 5);
            continue;
        }
        attempt = 0;
        lost = false;
        missing_since = 0;
        {
            std::lock_guard<std::mutex> l(m);
            host = ip;
            state = PlayerState{};
            state.version = now_ms(); // never repeats an earlier version
            state.connected = true;
            send_snapshot_request = 0;
        }
        logf("BRIDGE connected to %s:%d", ip.c_str(), p.port);
        std::uint64_t pinged = now_ms(), asked_output = 0, last_rx = now_ms();
        while (!stopping) {
            std::string text;
            const int got = ws.receive(text, 1000);
            if (got < 0) break;
            const auto now = now_ms();
            if (got == 1) { apply(text, now); last_rx = now; }
            // The phone sends a heartbeat every 10 s and answers pings: silence means it's gone
            // (Wi-Fi off, sleeping) even if the socket still looks open.
            if (now - last_rx > 30000) { logf("BRIDGE no data for 30 s: reconnecting"); break; }
            {
                std::lock_guard<std::mutex> l(m);
                if (generation != gen) break; // forgotten or paired with another phone
            }
            bool want_snapshot, want_output;
            {
                std::lock_guard<std::mutex> l(m);
                want_snapshot = send_snapshot_request;
                send_snapshot_request = 0;
                want_output = state.revision >= 0 && state.output != "pc" && !stopping;
            }
            if (want_snapshot && !ws.send_text("{\"type\":\"requestSnapshot\"}")) break;
            if (now - pinged >= 15000) { // the server closes after 45 s of silence
                pinged = now;
                if (!ws.send_text("{\"type\":\"ping\",\"clientTimeMs\":" + std::to_string(now) + "}")) break;
            }
            // We are the speaker: move the phone's audio here (it keeps playing silently).
            if (want_output && now - asked_output >= 5000) {
                asked_output = now;
                logf("BRIDGE output->pc: %d", command("player/output", "{\"output\":\"pc\"}"));
            }
        }
        problem(TR("Lost the connection to the phone", "Conexion con el telefono perdida"));
        if (!stopping) sleep(1);
    }
}

// Track ids are YouTube video ids or local numbers. Anything else must not reach a request
// line (a CR/LF there would inject headers).
bool safe_id(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    for (const unsigned char c : id)
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.') return false;
    return true;
}

int Bridge::command(const char* route, const std::string& body) {
    std::string h, token;
    int port;
    {
        std::lock_guard<std::mutex> l(m);
        h = host;
        token = pairing.token;
        port = pairing.port;
    }
    for (const char* c = route; *c; ++c)
        if (!std::isalnum((unsigned char)*c) && !std::strchr("/-_.", *c)) return 0; // ids end up in routes too
    if (h.empty()) return 0;
    net::Response r;
    if (!net::request(h, port, "POST", std::string("/api/v1/") + route, token, body, r, 64 << 10)) return 0;
    if (r.status != 200) logf("BRIDGE %s -> %d %s", route, r.status, r.body.c_str());
    return r.status;
}

int Bridge::get(const std::string& path, std::string& body) {
    std::string h, token;
    int port;
    {
        std::lock_guard<std::mutex> l(m);
        h = host;
        token = pairing.token;
        port = pairing.port;
    }
    for (const unsigned char c : path)
        if (!std::isalnum(c) && !std::strchr("/-_.?=&", c)) return 0;
    if (h.empty()) return 0;
    net::Response r;
    if (!net::request(h, port, "GET", "/api/v1/" + path, token, "", r, 4 << 20, 10000)) return 0;
    if (r.status != 200) logf("BRIDGE GET %s -> %d", path.c_str(), r.status);
    body.swap(r.body);
    return r.status;
}

std::string Bridge::audio_url(const std::string& track_id, const char* quality) {
    std::string h, token;
    int port;
    {
        std::lock_guard<std::mutex> l(m);
        h = host;
        token = pairing.token;
        port = pairing.port;
    }
    net::Response r;
    if (!safe_id(track_id)) { logf("AUDIO odd track id skipped"); return ""; }
    if (h.empty() || !net::request(h, port, "POST", "/api/v1/audio/" + track_id + "/url", token,
                                   std::string("{\"quality\":\"") + quality + "\"}", r, 64 << 10, 15000) || r.status != 200) {
        logf("AUDIO url %s failed: %d %s", track_id.c_str(), r.status, r.body.c_str());
        return "";
    }
    json::Doc d;
    return d.parse(r.body) ? d.str(d.get(0, "url")) : "";
}

bool Bridge::artwork(const std::string& track_id, int size, std::string& bytes, std::string& type) {
    std::string h, token;
    int port;
    {
        std::lock_guard<std::mutex> l(m);
        h = host;
        token = pairing.token;
        port = pairing.port;
    }
    net::Response r;
    if (!safe_id(track_id)) return false;
    if (h.empty() || !net::request(h, port, "GET", "/api/v1/artwork/" + track_id + "?size=" + std::to_string(size), token, "", r,
                                   2 << 20, 8000) || r.status != 200)
        return false;
    bytes.swap(r.body);
    type = r.content_type;
    return true;
}
