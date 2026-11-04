// SPDX-License-Identifier: GPL-3.0-or-later
// Audio worker, injected into SceShellUI by shell::Host. Plays a test tone or the PCM the
// host streams to 127.0.0.1:pcm_port, after setting up the ownership the host asks for.
// Built as a PIC shared object with entry elf_main; only libc/libkernel imports.
#include "protocol.hpp"
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace shell;

static void note(const char* format, ...) {
    char p[160];
    path(p, sizeof p, worker_dir, "worker.log");
    FILE* f = std::fopen(p, "a");
    if (!f) return;
    if (!std::fseek(f, 0, SEEK_END) && std::ftell(f) > 512 * 1024) { std::fclose(f); f = std::fopen(p, "w"); if (!f) return; }
    std::fprintf(f, "[%llu] ", (unsigned long long)now_ms());
    va_list a;
    va_start(a, format);
    std::vfprintf(f, format, a);
    va_end(a);
    std::fputc('\n', f);
    std::fclose(f);
}

static struct {
    int (*open)(int, int, int, unsigned, unsigned, unsigned);
    int (*output)(int, const void*);
    int (*close)(int);
    int (*volume)(int, int, const int*);
    int (*init)();
    int (*add)(const void*, const void*, std::int32_t*);
    int (*play)(std::int32_t);
    int (*stop)(std::int32_t);
    int (*remove)(std::int32_t);
    int (*is_playing)(std::int32_t, bool*);
    int (*set_tag)(int);
    int (*get_tag)(int*);
} api;

// librespot-ps5's tested source shape: a silent WAV served by the host, tagged as the
// system music player, so the console treats ShellUI as the music owner.
struct MediaSource {
    unsigned char bytes[0x2980]{};
    void fill(std::uint64_t token) {
        std::memset(bytes, 0, sizeof bytes);
        std::snprintf((char*)bytes, 0x1000, "http://127.0.0.1:%u/silence-%016llx.wav", wav_port, (unsigned long long)token);
        std::snprintf((char*)bytes + 0x1000, 0x800, "audio/wav;corename=\"SystemMusicCore\";titleid=\"NPXS40087\";trackindex=0;");
        const std::int32_t kind = 2;
        std::memcpy(bytes + 0x2948, &kind, 4);
    }
};
struct Callbacks { void (*fn)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t); void* user; std::uint64_t count; };
static void on_event(std::uint64_t kind, std::uint64_t, std::uint64_t, std::uint64_t) { note("MEDIAFW event kind=%llu", (unsigned long long)kind); }
static Callbacks callbacks{on_event, nullptr, 4}; // never freed: may be called after RemoveSrc
static MediaSource media_source;

static Request request;
static Status status;
static int port = -1, source = -1;
static bool tag_touched = false;

// ---- PCM from the host: a ring filled from one loopback connection ----
constexpr unsigned ring_frames = rate / 2, prime_frames = rate / 10; // 0.5 s buffer, 0.1 s before starting
static std::int16_t* ring;
static unsigned ring_read = 0, ring_size = 0;
static int listener = -1, client = -1;
static bool primed = false;
static unsigned char partial[4];
static unsigned partial_n = 0;

static void nonblocking(int fd) { int v = 1; setsockopt(fd, SOL_SOCKET, 0x1200, &v, sizeof v); } // SO_NBIO
static void drop_client() {
    if (client >= 0) close(client);
    client = -1;
    ring_read = ring_size = partial_n = 0;
    primed = false;
}
static void pcm_close_listener() {
    if (listener >= 0) close(listener);
    listener = -1;
}
static bool pcm_listen() {
    if (listener >= 0) return true;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(pcm_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener < 0 || bind(listener, (sockaddr*)&a, sizeof a) || listen(listener, 1)) {
        note("PCM listen failed errno=%d", errno);
        pcm_close_listener();
        return false;
    }
    nonblocking(listener);
    note("PCM listening on %u", pcm_port);
    return true;
}
static void pcm_receive() {
    const int c = listener >= 0 ? accept(listener, nullptr, nullptr) : -1;
    if (c >= 0) { // a new connection means a new stream (seek, new song): drop the old audio
        drop_client();
        client = c;
        nonblocking(client);
        int small = 32 * 1024;
        setsockopt(client, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    }
    // Only read what fits: a full ring stops reading, and TCP backpressure paces the host.
    while (client >= 0 && ring_size < ring_frames) {
        const unsigned at = (ring_read + ring_size) % ring_frames;
        const unsigned room = ring_frames - ring_size, contiguous = ring_frames - at < room ? ring_frames - at : room;
        unsigned char* dst = reinterpret_cast<unsigned char*>(ring + at * 2);
        std::memcpy(dst, partial, partial_n); // finish a frame split across reads
        const ssize_t n = recv(client, dst + partial_n, contiguous * 4 - partial_n, 0);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { client = (close(client), -1); break; }
        if (n < 0) break;
        const unsigned bytes = partial_n + unsigned(n), frames = bytes / 4;
        partial_n = bytes % 4;
        std::memcpy(partial, dst + frames * 4, partial_n);
        ring_size += frames;
    }
    if (ring_size >= prime_frames) primed = true;
}
static void pcm_take(std::int16_t* out) {
    unsigned got = 0;
    if (primed)
        for (; got < grain && ring_size; ++got, --ring_size, ring_read = (ring_read + 1) % ring_frames) {
            out[2 * got] = ring[2 * ring_read];
            out[2 * got + 1] = ring[2 * ring_read + 1];
        }
    if (primed && got < grain) { ++status.underruns; primed = false; } // dry: wait for a new prime
    std::memset(out + 2 * got, 0, (grain - got) * 4);
    status.queued = int(ring_size);
}

static void tone(std::int16_t* pcm) { // 440 Hz triangle, no libm
    static unsigned phase = 0;
    const unsigned period = rate / 440;
    for (unsigned i = 0; i < grain; ++i, phase = (phase + 1) % period) {
        const int t = int(phase * 4 * 6000 / period);
        const int v = t < 6000 ? t : t < 18000 ? 12000 - t : t - 24000;
        pcm[2 * i] = pcm[2 * i + 1] = std::int16_t(v);
    }
}

static void stop_all() {
    if (port >= 0) { note("AUDIO close rc=%08x", api.close(port)); port = -1; }
    if (source > 0 && api.stop && api.remove) { note("MEDIAFW stop rc=%08x remove rc=%08x", api.stop(source), api.remove(source)); source = -1; }
    if (tag_touched) {
        if (status.tag_before >= 0 && status.tag_before <= 2) note("AUDIOTAG restore %d rc=%08x", status.tag_before, api.set_tag(status.tag_before));
        else note("AUDIOTAG original unknown; left at 0 until reboot");
        tag_touched = false;
    }
    drop_client();
    pcm_close_listener(); // a later worker (next launch) must be able to take the port
    status.playing = 0;
}

static void start(int action, int ownership) {
    stop_all();
    status.action = action;
    status.ownership = ownership;
    status.rc_open = status.rc_output = status.rc_add = status.rc_play = status.rc_tag = 0;
    status.src_playing = -1;
    status.frames_k = status.underruns = 0;
    if (action == Stop) return;
    if (ownership == SourceTag) {
        if (!api.set_tag) { status.rc_tag = -1; note("AUDIOTAG setter missing"); }
        else {
            int before = -1;
            if (api.get_tag) {
                const int rc = api.get_tag(&before);
                if (before < 0 && rc >= 0 && rc <= 2) before = rc; // value by pointer or as the return value
                note("AUDIOTAG get rc=%08x value=%d", rc, before);
            }
            status.tag_before = before;
            tag_touched = true;
            status.rc_tag = api.set_tag(0);
            note("AUDIOTAG set 0 rc=%08x", status.rc_tag);
        }
    }
    if (ownership >= Source) {
        if (!api.add || !api.play || !api.stop || !api.remove) status.rc_add = -1;
        else {
            media_source.fill(request.token);
            std::int32_t id = -1;
            status.rc_add = api.add(media_source.bytes, &callbacks, &id);
            note("MEDIAFW add rc=%08x id=%d", status.rc_add, id);
            if (!status.rc_add && id > 0) {
                source = id;
                status.rc_play = api.play(id);
                note("MEDIAFW play rc=%08x", status.rc_play);
                for (int i = 0; i < 30 && api.is_playing; ++i) { // up to 3 s to start
                    bool playing = false;
                    if (!api.is_playing(id, &playing) && playing) { status.src_playing = 1; break; }
                    status.src_playing = 0;
                    usleep(100000);
                }
            }
        }
    }
    port = api.open(0xff, 0, 0, grain, rate, 1); // system user, MAIN port, S16 stereo
    if (std::uint32_t(port) == 0x8026000fU) {    // AudioOut not initialized in ShellUI yet
        note("AUDIO init rc=%08x", api.init());
        port = api.open(0xff, 0, 0, grain, rate, 1);
    }
    status.rc_open = port;
    note("AUDIO open action=%d ownership=%d rc=%08x", action, ownership, port);
    if (port < 0) return;
    const int volume[2] = {32768, 32768};
    api.volume(port, 3, volume);
    if (action == Pcm && !pcm_listen()) { status.rc_open = -2; stop_all(); return; }
    status.playing = 1;
}

extern "C" __attribute__((visibility("default"))) void* elf_main(void*) {
    if (!read_file(worker_dir, "request.bin", request) || request.magic != magic) {
        note("REFUSED no request");
        for (;;) sleep(60);
    }
    auto* f = request.fn;
    api.open = (decltype(api.open))f[AudioOpen];
    api.output = (decltype(api.output))f[AudioOutput];
    api.close = (decltype(api.close))f[AudioClose];
    api.volume = (decltype(api.volume))f[AudioVolume];
    api.init = (decltype(api.init))f[AudioInit];
    api.add = (decltype(api.add))f[FwAdd];
    api.play = (decltype(api.play))f[FwPlay];
    api.stop = (decltype(api.stop))f[FwStop];
    api.remove = (decltype(api.remove))f[FwRemove];
    api.is_playing = (decltype(api.is_playing))f[FwIsPlaying];
    api.set_tag = (decltype(api.set_tag))f[FwSetTag];
    api.get_tag = (decltype(api.get_tag))f[FwGetTag];
    ring = static_cast<std::int16_t*>(std::malloc(ring_frames * 4));
    // The host checked these exist; refuse rather than call a null inside ShellUI.
    if (!ring || !api.open || !api.output || !api.close || !api.volume || !api.init) {
        note("REFUSED missing memory or AudioOut functions");
        for (;;) sleep(60);
    }
    status.token = request.token;
    note("READY token=%016llx", (unsigned long long)request.token);
    alignas(64) static std::int16_t pcm[grain * 2];
    std::uint64_t checked = 0, published = 0, frames = 0, start_ms = 0;
    for (;;) {
        const auto now = now_ms();
        if (now - checked >= 100) {
            checked = now;
            Heartbeat h;
            if (!read_file(worker_dir, "heartbeat.bin", h) || h.token != request.token || now > h.at_ms + 3000) {
                stop_all();
                note("HOST gone; idle");
                write_file(worker_dir, "status.bin", status);
                for (;;) sleep(60); // stay mapped: ShellUI may still call back into this code
            }
            Command c;
            if (read_file(worker_dir, "command.bin", c) && c.token == request.token && c.seq > status.seq) {
                status.seq = c.seq;
                note("COMMAND seq=%llu action=%d ownership=%d", (unsigned long long)c.seq, c.action, c.ownership);
                start(c.action, c.ownership);
                frames = 0;
            }
        }
        if (status.playing && port >= 0) {
            if (status.action == Pcm) { pcm_receive(); pcm_take(pcm); }
            else tone(pcm);
            const int rc = api.output(port, pcm);
            if (rc < 0) { status.rc_output = rc; note("AUDIO output rc=%08x", rc); stop_all(); }
            // Output may return at once: pace by deadline (as librespot-ps5 does).
            if (!frames) start_ms = now_ms();
            frames += grain;
            status.frames_k = int(frames / 1000);
            const std::uint64_t due = start_ms + (frames - grain) * 1000 / rate, t = now_ms();
            if (due > t) usleep(unsigned(due - t) * 1000);
            else if (t > due + 200) { start_ms = t; frames = grain; } // fell behind (ShellUI busy): restart the clock
        } else {
            usleep(20000);
        }
        if (now - published >= 500) {
            published = now;
            status.at_ms = now;
            write_file(worker_dir, "status.bin", status);
        }
    }
}
