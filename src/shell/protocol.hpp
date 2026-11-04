// SPDX-License-Identifier: GPL-3.0-or-later
// Contract between a payload (the host) and the audio worker it injects into SceShellUI.
// A background payload's own audio is muted by the console; ShellUI's is not.
// They talk through small files (commands, status, heartbeat) and a loopback socket (PCM).
#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <unistd.h>

namespace shell {
constexpr const char* dir = "/data/n-zik-ps5/shell";
constexpr const char* worker_dir = "/user/data/n-zik-ps5/shell"; // ShellUI sees /data as /user/data
constexpr std::uint16_t wav_port = 38578, pcm_port = 38579;
constexpr std::uint64_t magic = 0x3253484c4b5a4e31ULL;
constexpr unsigned rate = 48000, grain = 1024; // S16 stereo frames per sceAudioOutOutput

enum Fn : unsigned {
    AudioOpen, AudioOutput, AudioClose, AudioVolume, AudioInit,
    FwAdd, FwPlay, FwStop, FwRemove, FwIsPlaying, FwSetTag, FwGetTag,
    FnCount
};
inline const char* const fn_names[FnCount] = {
    "sceAudioOutOpen", "sceAudioOutOutput", "sceAudioOutClose", "sceAudioOutSetVolume", "sceAudioOutInit",
    "sceMediaFwAddSrc", "sceMediaFwPlay", "sceMediaFwStop", "sceMediaFwRemoveSrc", "sceMediaFwIsPlaying",
    "sceMediaFwSetAudioTagFeature", "sceMediaFwGetAudioTagFeature",
};
inline const char* fn_lib(unsigned i) { return i <= AudioInit ? "libSceAudioOut.sprx" : "libSceMediaFrameworkInterface.sprx"; }

// How the worker makes the console let its audio through over a game.
//   Direct: AudioOut MAIN port only.
//   Source: + a silent system-music source (librespot-ps5's ownership trick).
//   SourceTag: + AudioTag feature 0 (what librespot-ps5 needs on 13.20).
enum Ownership : int { Direct = 1, Source = 2, SourceTag = 3 };
enum Action : int { Stop = 0, Tone = 1, Pcm = 2 };

struct Request { std::uint64_t magic = shell::magic, token = 0; std::uint64_t fn[FnCount]{}; };
struct Heartbeat { std::uint64_t token = 0, at_ms = 0; };
struct Command { std::uint64_t token = 0, seq = 0; int action = Stop, ownership = Direct; };
struct Status {
    std::uint64_t token = 0, seq = 0, at_ms = 0;
    int action = Stop, ownership = Direct, playing = 0, frames_k = 0, underruns = 0, queued = 0;
    int rc_open = 0, rc_output = 0, rc_add = 0, rc_play = 0, rc_tag = 0, tag_before = -1, src_playing = -1;
};

inline std::uint64_t now_ms() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return std::uint64_t(t.tv_sec) * 1000 + std::uint64_t(t.tv_nsec) / 1000000;
}
inline void path(char* out, std::size_t cap, const char* base, const char* name) { std::snprintf(out, cap, "%s/%s", base, name); }
template <class T> bool read_file(const char* base, const char* name, T& v) {
    char p[160];
    path(p, sizeof p, base, name);
    FILE* f = std::fopen(p, "rb");
    if (!f) return false;
    const bool ok = std::fread(&v, sizeof v, 1, f) == 1;
    std::fclose(f);
    return ok;
}
// Write then rename, so the other side never reads half a record.
template <class T> bool write_file(const char* base, const char* name, const T& v) {
    char p[160], tmp[168];
    path(p, sizeof p, base, name);
    std::snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE* f = std::fopen(tmp, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(&v, sizeof v, 1, f) == 1;
    std::fclose(f);
    return ok && !std::rename(tmp, p);
}
}
