// SPDX-License-Identifier: GPL-3.0-or-later
#include "net.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef __PROSPERO__
extern "C" int sceRandomGetRandomNumber(void*, std::size_t);
#endif

namespace net {

bool random_bytes(void* out, std::size_t size) {
#ifdef __PROSPERO__
    return sceRandomGetRandomNumber(out, size) == 0; // at most 64 bytes per call; we ask for less
#else
    arc4random_buf(out, size);
    return true;
#endif
}

// The console refuses FIONBIO and O_NONBLOCK on sockets; its own switch is SO_NBIO.
static void nonblocking(int fd, bool on) {
#ifdef __PROSPERO__
    int v = on;
    setsockopt(fd, SOL_SOCKET, 0x1200, &v, sizeof v);
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
#endif
}
static void timeouts(int fd, int ms) {
    timeval t{ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
    int yes = 1;
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
}

int connect_tcp(const std::string& host, int port, int timeout_ms, int io_timeout_ms) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) return -1; // LAN only: IPs, no DNS
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    nonblocking(fd, true);
    int rc = ::connect(fd, (sockaddr*)&a, sizeof a);
    if (rc && errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        int err = 0;
        socklen_t len = sizeof err;
        rc = poll(&p, 1, timeout_ms) == 1 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) && !err ? 0 : -1;
    }
    if (rc) { ::close(fd); return -1; }
    nonblocking(fd, false);
    timeouts(fd, io_timeout_ms);
    return fd;
}

bool send_all(int fd, const void* data, std::size_t size) {
    auto* p = static_cast<const char*>(data);
    while (size) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, p, size, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, p, size, 0);
#endif
        if (n > 0) { p += n; size -= std::size_t(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static std::uint64_t mono_ms();
std::vector<std::string> sweep(const std::string& my_ip, int port, int timeout_ms) {
    std::vector<std::string> found;
    in_addr me{};
    if (inet_pton(AF_INET, my_ip.c_str(), &me) != 1) return found;
    const std::uint32_t base = ntohl(me.s_addr) & 0xFFFFFF00u;
    // ponytail: /24 only (home networks); in batches so we never hold too many sockets.
    for (int first = 1; first < 255; first += 64) {
        std::vector<pollfd> polls;
        std::vector<std::string> hosts;
        for (int last = std::min(first + 64, 255), i = first; i < last; ++i) {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            a.sin_addr.s_addr = htonl(base | std::uint32_t(i));
            if (a.sin_addr.s_addr == me.s_addr) continue;
            const int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            nonblocking(fd, true);
            const int rc = ::connect(fd, (sockaddr*)&a, sizeof a);
            if (rc && errno != EINPROGRESS) { ::close(fd); continue; }
            char text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &a.sin_addr, text, sizeof text);
            polls.push_back({fd, POLLOUT, 0});
            hosts.emplace_back(text);
        }
        const std::uint64_t until = mono_ms() + std::uint64_t(timeout_ms);
        std::vector<bool> done(polls.size(), false);
        for (std::uint64_t now = mono_ms(); now < until; now = mono_ms()) {
            if (poll(polls.data(), nfds_t(polls.size()), int(until - now)) <= 0) break;
            for (std::size_t i = 0; i < polls.size(); ++i) {
                if (done[i] || !polls[i].revents) continue;
                done[i] = true;
                int err = 0;
                socklen_t len = sizeof err;
                if (!getsockopt(polls[i].fd, SOL_SOCKET, SO_ERROR, &err, &len) && !err) found.push_back(hosts[i]);
                polls[i].fd = -polls[i].fd - 1; // poll ignores negative fds: stop watching it
            }
        }
        for (auto& p : polls) ::close(p.fd < 0 ? -p.fd - 1 : p.fd);
    }
    return found;
}

std::string local_ip() {
    // A UDP "connect" picks the route without sending anything.
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    char out[32] = "";
    if (fd >= 0 && !::connect(fd, (sockaddr*)&a, sizeof a)) {
        sockaddr_in me{};
        socklen_t len = sizeof me;
        if (!getsockname(fd, (sockaddr*)&me, &len)) inet_ntop(AF_INET, &me.sin_addr, out, sizeof out);
    }
    if (fd >= 0) ::close(fd);
    return out;
}

static std::uint64_t mono_ms() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return std::uint64_t(t.tv_sec) * 1000 + std::uint64_t(t.tv_nsec) / 1000000;
}
// Reads until the end of the headers, within `max` bytes and `deadline_ms` in total (a slow
// peer can't hold us). Anything after them stays in `rest`.
static bool read_head(int fd, std::string& head, std::string& rest, std::size_t max = 32768, int deadline_ms = 15000) {
    char buf[4096];
    std::string all;
    const std::uint64_t until = mono_ms() + std::uint64_t(deadline_ms);
    for (;;) {
        const auto end = all.find("\r\n\r\n");
        if (end != std::string::npos) {
            head = all.substr(0, end + 2);
            rest = all.substr(end + 4);
            return true;
        }
        if (all.size() > max || mono_ms() > until) return false;
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, int(until - std::min(until, mono_ms()))) != 1) return false;
        const ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) return false;
        all.append(buf, std::size_t(n));
    }
}
static std::string header(const std::string& head, const char* name) {
    const std::size_t len = std::strlen(name);
    for (std::size_t at = head.find("\r\n"); at != std::string::npos; at = head.find("\r\n", at + 2)) {
        const std::size_t line = at + 2;
        if (head.size() > line + len && !strncasecmp(head.c_str() + line, name, len) && head[line + len] == ':') {
            std::size_t v = line + len + 1;
            while (v < head.size() && head[v] == ' ') ++v;
            return head.substr(v, head.find("\r\n", v) - v);
        }
    }
    return "";
}
static bool parse_head(const std::string& head, Response& r) {
    if (std::sscanf(head.c_str(), "HTTP/1.%*d %d", &r.status) != 1) return false;
    r.content_type = header(head, "Content-Type");
    const std::string length = header(head, "Content-Length");
    r.has_length = !length.empty();
    r.content_length = std::strtoull(length.c_str(), nullptr, 10);
    const std::string range = header(head, "Content-Range"); // bytes a-b/total
    const auto slash = range.find('/');
    r.total = slash != std::string::npos ? std::strtoull(range.c_str() + slash + 1, nullptr, 10) : r.content_length;
    return true;
}
static std::string request_text(const std::string& host, int port, const char* method, const std::string& path,
                                const std::string& bearer, const std::string& body, const std::string& extra) {
    std::string r = std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) + "\r\n";
    if (!bearer.empty()) r += "Authorization: Bearer " + bearer + "\r\n";
    if (!body.empty() || !std::strcmp(method, "POST"))
        r += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    return r + extra + "\r\n" + body;
}

bool request(const std::string& host, int port, const char* method, const std::string& path, const std::string& bearer,
             const std::string& body, Response& out, std::size_t max_body, int timeout_ms) {
    out = Response{};
    const int fd = connect_tcp(host, port, timeout_ms, timeout_ms * 2);
    if (fd < 0) return false;
    const std::string text = request_text(host, port, method, path, bearer, body, "Connection: close\r\n");
    std::string head, rest;
    bool ok = send_all(fd, text.data(), text.size()) && read_head(fd, head, rest) && parse_head(head, out);
    if (ok) {
        out.body = rest;
        char buf[16384];
        // No body for 204/304; without Content-Length, read until the server closes.
        const bool none = out.status == 204 || out.status == 304;
        const std::size_t want = none ? 0 : out.has_length ? std::size_t(out.content_length) : max_body + 1;
        while (out.body.size() < want && out.body.size() <= max_body) {
            const ssize_t n = recv(fd, buf, sizeof buf, 0);
            if (n <= 0) break;
            out.body.append(buf, std::size_t(n));
        }
        if (out.body.size() > max_body || (out.has_length && out.body.size() < want)) ok = false; // too big, or cut short
    }
    ::close(fd);
    return ok;
}

bool split_url(const std::string& url, std::string& host, int& port, std::string& path) {
    if (url.compare(0, 7, "http://")) return false;
    const auto slash = url.find('/', 7);
    const std::string authority = url.substr(7, slash == std::string::npos ? std::string::npos : slash - 7);
    path = slash == std::string::npos ? "/" : url.substr(slash);
    const auto colon = authority.rfind(':');
    host = colon == std::string::npos ? authority : authority.substr(0, colon);
    port = colon == std::string::npos ? 80 : std::atoi(authority.c_str() + colon + 1);
    return !host.empty() && port > 0;
}

bool Stream::open(const std::string& url, std::uint64_t from, int timeout_ms) {
    close();
    std::string host, path;
    int port;
    if (!split_url(url, host, port, path)) return false;
    fd = connect_tcp(host, port, timeout_ms, 15000);
    if (fd < 0) return false;
    const std::string text = request_text(host, port, "GET", path, "", "",
                                          "Range: bytes=" + std::to_string(from) + "-\r\nConnection: close\r\n");
    std::string headers, rest;
    // From an offset we need a 206: a server that ignored Range would feed us the wrong bytes.
    if (!send_all(fd, text.data(), text.size()) || !read_head(fd, headers, rest) || !parse_head(headers, head) ||
        (head.status != 206 && (head.status != 200 || from > 0))) {
        close();
        return false;
    }
    pending = rest;
    return true;
}
long Stream::read(void* out, std::size_t size) {
    if (fd < 0) return -1;
    if (!pending.empty()) {
        const std::size_t n = pending.size() < size ? pending.size() : size;
        std::memcpy(out, pending.data(), n);
        pending.erase(0, n);
        return long(n);
    }
    const ssize_t n = recv(fd, out, size, 0);
    return n < 0 ? -1 : long(n);
}
void Stream::close() {
    if (fd >= 0) ::close(fd);
    fd = -1;
    pending.clear();
}

std::string base64(const unsigned char* d, std::size_t n) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < n; i += 3) {
        const unsigned v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        out += t[v >> 18 & 63];
        out += t[v >> 12 & 63];
        out += i + 1 < n ? t[v >> 6 & 63] : '=';
        out += i + 2 < n ? t[v & 63] : '=';
    }
    return out;
}

bool WebSocket::connect(const std::string& host, int port, const std::string& path, const std::string& bearer, int& http_status) {
    close();
    http_status = 0;
    fd = connect_tcp(host, port, 1500, 15000); // LAN: a phone that is there answers in ms
    if (fd < 0) return false;
    unsigned char key[16];
    if (!random_bytes(key, sizeof key)) { close(); return false; }
    // ponytail: the Sec-WebSocket-Accept hash isn't checked; LAN peer we just authenticated to.
    const std::string text = request_text(host, port, "GET", path, bearer, "",
                                          "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
                                              base64(key, sizeof key) + "\r\n");
    std::string head;
    Response r;
    if (!send_all(fd, text.data(), text.size()) || !read_head(fd, head, pending) || !parse_head(head, r) || r.status != 101) {
        http_status = r.status;
        close();
        return false;
    }
    http_status = 101;
    return true;
}
static bool send_frame(int fd, int opcode, const std::string& payload) {
    std::string f;
    f += char(0x80 | opcode);
    const std::size_t n = payload.size();
    if (n < 126) f += char(0x80 | n);
    else if (n < 65536) { f += char(0x80 | 126); f += char(n >> 8); f += char(n & 255); }
    else { f += char(0x80 | 127); for (int i = 7; i >= 0; --i) f += char((std::uint64_t(n) >> (8 * i)) & 255); }
    unsigned char mask[4];
    if (!random_bytes(mask, sizeof mask)) return false;
    f.append((const char*)mask, 4);
    for (std::size_t i = 0; i < n; ++i) f += char(payload[i] ^ mask[i & 3]);
    return send_all(fd, f.data(), f.size());
}
bool WebSocket::send_text(const std::string& text) { return fd >= 0 && send_frame(fd, 1, text); }
int WebSocket::receive(std::string& text, int timeout_ms) {
    std::string message;
    for (;;) {
        // Parse a frame from what we have.
        if (pending.size() >= 2) {
            const auto* b = (const unsigned char*)pending.data();
            std::size_t len = b[1] & 127, at = 2;
            if (len == 126 && pending.size() >= 4) { len = b[2] << 8 | b[3]; at = 4; }
            else if (len == 127 && pending.size() >= 10) { len = 0; for (int i = 0; i < 8; ++i) len = len << 8 | b[2 + i]; at = 10; }
            else if (len >= 126) goto more;
            if (b[1] & 0x80) at += 4; // servers don't mask, but be tolerant
            if (len > (16u << 20)) return -1;
            if (pending.size() >= at + len) {
                const int opcode = b[0] & 15;
                const bool fin = b[0] & 0x80;
                std::string payload = pending.substr(at, len);
                if (b[1] & 0x80)
                    for (std::size_t i = 0; i < len; ++i) payload[i] ^= pending[at - 4 + (i & 3)];
                pending.erase(0, at + len);
                if (opcode == 8) return -1;                                   // close
                if (opcode == 9) { send_frame(fd, 10, payload); continue; }   // ping
                if (opcode == 10) continue;                                   // pong
                message += payload;
                if (message.size() > (16u << 20)) return -1;
                if (fin) { text.swap(message); return 1; }
                continue;
            }
        }
    more:
        pollfd p{fd, POLLIN, 0};
        const int ready = poll(&p, 1, message.empty() ? timeout_ms : 5000);
        if (ready == 0) return message.empty() ? 0 : -1;
        if (ready < 0) { if (errno == EINTR) continue; return -1; }
        char buf[16384];
        const ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) return -1;
        pending.append(buf, std::size_t(n));
    }
}
void WebSocket::close() {
    if (fd >= 0) ::close(fd);
    fd = -1;
    pending.clear();
}

bool Listener::listen_on(int port, bool loopback_only) {
    close();
    fd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (fd < 0 || bind(fd, (sockaddr*)&a, sizeof a) || listen(fd, 4)) { close(); return false; }
    return true;
}
int Listener::accept_one(int timeout_ms) {
    pollfd p{fd, POLLIN, 0};
    if (fd < 0 || poll(&p, 1, timeout_ms) != 1) return -1;
    const int c = accept(fd, nullptr, nullptr);
    if (c >= 0) timeouts(c, 5000);
    return c;
}
void Listener::close() {
    if (fd >= 0) ::close(fd);
    fd = -1;
}
bool read_request(int fd, std::string& method, std::string& path, std::string& body, std::size_t max_body) {
    std::string head;
    const std::uint64_t until = mono_ms() + 4000; // headers and body, whole request
    if (!read_head(fd, head, body, 4096, 4000)) return false;
    char m[8], p[256];
    if (std::sscanf(head.c_str(), "%7s %255s HTTP/1.", m, p) != 2) return false;
    method = m;
    path = p;
    const std::size_t want = std::strtoull(header(head, "Content-Length").c_str(), nullptr, 10);
    if (want > max_body) return false;
    char buf[4096];
    while (body.size() < want) {
        pollfd p{fd, POLLIN, 0};
        if (mono_ms() >= until || poll(&p, 1, int(until - mono_ms())) != 1) return false;
        const ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) return false;
        body.append(buf, std::size_t(n));
    }
    body.resize(want);
    return true;
}
void respond(int fd, int status, const char* reason, const std::string& body, const char* type) {
    char head[256];
    const int n = std::snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                                status, reason, type, body.size());
    send_all(fd, head, std::size_t(n));
    send_all(fd, body.data(), body.size());
}
}
