// SPDX-License-Identifier: GPL-3.0-or-later
#include "host.hpp"
#include "../log.hpp"
#include "stable_shellui_injector.hpp"
#include <ps5/kernel.h>
#include <ps5/nid.h>
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/user.h>

extern "C" {
extern const unsigned char worker_blob[];
extern const unsigned int worker_blob_size;
}

namespace shell {

// ---- silent WAV for the system-music source ----
constexpr unsigned wav_seconds = 120;
constexpr std::uint64_t wav_data = std::uint64_t(wav_seconds) * rate * 4, wav_total = 44 + wav_data;
static void wav_header(unsigned char h[44]) {
    std::memcpy(h, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\0\0\0\0\0\0\0\0\x04\0\x10\0data\0\0\0\0", 44);
    auto put = [&](int at, std::uint32_t v) { for (int i = 0; i < 4; ++i) h[at + i] = (v >> (8 * i)) & 255; };
    put(4, std::uint32_t(wav_total - 8)); put(24, rate); put(28, rate * 4); put(40, std::uint32_t(wav_data));
}
static void serve(int c) {
    char req[2048];
    int n = 0, r;
    while (n < int(sizeof req) - 1 && (r = int(recv(c, req + n, sizeof req - 1 - std::size_t(n), 0))) > 0) {
        n += r;
        req[n] = 0;
        if (std::strstr(req, "\r\n\r\n")) break;
    }
    req[n] = 0;
    const bool head = !std::strncmp(req, "HEAD /silence-", 14);
    if (!head && std::strncmp(req, "GET /silence-", 13)) {
        const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(c, nf, std::strlen(nf), 0);
        return;
    }
    std::uint64_t from = 0, to = wav_total - 1;
    bool partial = false;
    if (const char* range = strcasestr(req, "\r\nRange: bytes=")) {
        char* end = nullptr;
        from = std::strtoull(range + 15, &end, 10);
        if (end && *end == '-' && end[1] >= '0' && end[1] <= '9') to = std::strtoull(end + 1, nullptr, 10);
        to = std::min(to, wav_total - 1);
        partial = true;
        if (from > to) {
            char h[160];
            const int len = std::snprintf(h, sizeof h, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%llu\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                                          (unsigned long long)wav_total);
            send(c, h, std::size_t(len), 0);
            return;
        }
    }
    char h[320];
    int len = std::snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: audio/wav\r\nAccept-Ranges: bytes\r\nContent-Length: %llu\r\nConnection: close\r\n",
                            partial ? "206 Partial Content" : "200 OK", (unsigned long long)(to - from + 1));
    if (partial) len += std::snprintf(h + len, sizeof h - std::size_t(len), "Content-Range: bytes %llu-%llu/%llu\r\n",
                                      (unsigned long long)from, (unsigned long long)to, (unsigned long long)wav_total);
    len += std::snprintf(h + len, sizeof h - std::size_t(len), "\r\n");
    if (send(c, h, std::size_t(len), 0) != len || head) return;
    unsigned char header[44], buf[16384];
    wav_header(header);
    for (std::uint64_t at = from; at <= to;) {
        const std::size_t chunk = std::size_t(std::min<std::uint64_t>(sizeof buf, to - at + 1));
        std::memset(buf, 0, chunk);
        if (at < 44) std::memcpy(buf, header + at, std::min<std::size_t>(chunk, std::size_t(44 - at)));
        if (send(c, buf, chunk, 0) != ssize_t(chunk)) return;
        at += chunk;
    }
}
void* Host::wav_server(void* self) {
    auto* host = static_cast<Host*>(self);
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(wav_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || bind(s, (sockaddr*)&a, sizeof a) || listen(s, 4)) { logf("SHELL wav server failed errno=%d", errno); return nullptr; }
    while (!host->quitting) {
        const int c = accept(s, nullptr, nullptr);
        if (c < 0) continue;
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
        timeval t{5, 0}; // a stuck client must not wedge the only serving thread
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
        serve(c);
        close(c);
    }
    close(s);
    return nullptr;
}

void* Host::heartbeat(void* self) {
    auto* host = static_cast<Host*>(self);
    while (!host->quitting) {
        write_file(dir, "heartbeat.bin", Heartbeat{host->request.token, now_ms()});
        usleep(500000);
    }
    return nullptr;
}

static int shell_pid() {
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    std::size_t n = 0;
    if (sysctl(mib, 4, nullptr, &n, nullptr, 0) || !n) return -1;
    n += 64 * sizeof(kinfo_proc);
    auto* data = static_cast<unsigned char*>(std::malloc(n));
    int pid = -1;
    if (data && !sysctl(mib, 4, data, &n, nullptr, 0))
        for (std::size_t i = 0; i + sizeof(kinfo_proc) <= n;) {
            auto* p = reinterpret_cast<kinfo_proc*>(data + i);
            if (p->ki_structsize < int(sizeof(kinfo_proc))) break;
            if (!std::strcmp(p->ki_comm, "SceShellUI")) pid = p->ki_pid;
            i += std::size_t(p->ki_structsize);
        }
    std::free(data);
    return pid;
}

bool Host::start(std::string& info) {
    mkdir("/data/n-zik-ps5", 0777);
    mkdir(dir, 0777);
    std::uint32_t sdk = 0;
    std::size_t n = sizeof sdk;
    sysctlbyname("kern.sdk_version", &sdk, &n, nullptr, 0);
    const int pid = shell_pid();
    request.token = (now_ms() << 16) ^ std::uint64_t(getpid());
    unsigned found = 0;
    for (unsigned i = 0; i < FnCount; ++i) {
        std::uint32_t handle = 0;
        intptr_t v = 0;
        if (pid > 0 && kernel_dynlib_handle(pid, fn_lib(i), &handle) >= 0 && handle) {
            v = kernel_dynlib_dlsym(pid, handle, fn_names[i]);
            char nid[12]{};
            if (!v && nid_encode(fn_names[i], nid)) v = kernel_dynlib_resolve(pid, handle, nid);
        }
        request.fn[i] = std::uint64_t(v);
        found += v != 0;
        logf("SHELL export %s %s", fn_names[i], v ? "ok" : "MISSING");
    }
    char text[160];
    // kern.sdk_version is the SDK the system was built with; the label shows the kernel's version.
    const std::uint32_t fw = kernel_get_fw_version();
    std::snprintf(text, sizeof text, "FW %x.%02x, ShellUI %d, funciones %u/%u", (fw >> 24) & 0xff, (fw >> 16) & 0xff, pid, found, unsigned(FnCount));
    info = text;
    logf("SHELL sdk=%08x fw=%08x %s", sdk, fw, text);
    for (unsigned i = AudioOpen; i <= AudioInit; ++i)
        if (pid <= 0 || !request.fn[i]) { info += "; sin sceAudioOut"; return false; }
    pthread_t t;
    pthread_create(&t, nullptr, wav_server, this);
    pthread_detach(t);
    if (!write_file(dir, "request.bin", request) || !write_file(dir, "heartbeat.bin", Heartbeat{request.token, now_ms()})) {
        info += "; no puedo escribir " + std::string(dir);
        return false;
    }
    pthread_create(&t, nullptr, heartbeat, this);
    pthread_detach(t);
    const auto loaded = common_fps::ps5::inject_shellui_stable(pid, worker_blob, worker_blob_size);
    logf("SHELL loader mapped=%u thread=%u detached=%u auth_restored=%u missing=%u first=%s", loaded.elf_loaded,
         loaded.pthread_create_ok, loaded.detached, loaded.auth_restored, loaded.imports_unresolved,
         loaded.first_unresolved ? loaded.first_unresolved : "");
    if (!loaded.elf_loaded || !loaded.pthread_create_ok || loaded.imports_unresolved) { info += "; fallo la inyeccion"; return false; }
    // Wait for the worker to say it's there.
    for (int i = 0; i < 30; ++i) {
        Status s;
        if (status(s)) return true;
        usleep(100000);
    }
    info += "; el worker no responde";
    return false;
}

void Host::command(int action, int ownership) {
    write_file(dir, "command.bin", Command{request.token, ++seq, action, ownership});
    logf("SHELL command seq=%llu action=%d ownership=%d", (unsigned long long)seq, action, ownership);
}

bool Host::status(Status& out) const {
    return read_file(dir, "status.bin", out) && out.token == request.token && now_ms() <= out.at_ms + 3000;
}

int Host::connect_pcm() const {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(pcm_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || connect(fd, (sockaddr*)&a, sizeof a)) { if (fd >= 0) close(fd); return -1; }
    int yes = 1, small = 32 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof small); // little queued audio: quick pause/seek
    timeval t{3, 0}; // a stuck worker must not hang the player
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
    return fd;
}

Host::~Host() {
    command(Stop, Direct);
    usleep(1500000); // let the worker stop and restore before the heartbeat ends
    quitting = true;
}
}
