// SPDX-License-Identifier: GPL-3.0-or-later
// Plain-TCP HTTP/1.1 client, WebSocket client and a tiny one-shot HTTP server.
// Everything here is LAN or loopback, no TLS. Blocking sockets with timeouts.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace net {
// Connects with a timeout; -1 on failure. Read/write timeouts are set on the socket.
int connect_tcp(const std::string& host, int port, int timeout_ms, int io_timeout_ms = 10000);
bool send_all(int fd, const void* data, std::size_t size);
// Our LAN IPv4 as seen by the default route ("" if none).
std::string local_ip();
// The other hosts of our /24 that accept a TCP connection on `port`, all tried at once.
std::vector<std::string> sweep(const std::string& my_ip, int port, int timeout_ms);

struct Response {
    int status = 0;
    std::string content_type, body;
    std::uint64_t content_length = 0, total = 0;
    bool has_length = false; // total: from Content-Range when partial
};
// One request; the body is read up to max_body bytes.
bool request(const std::string& host, int port, const char* method, const std::string& path,
             const std::string& bearer, const std::string& body, Response& out, std::size_t max_body = 4 << 20,
             int timeout_ms = 5000);

// A GET whose body is streamed: open() returns after the headers.
struct Stream {
    int fd = -1;
    Response head;
    std::string pending;
    bool open(const std::string& url, std::uint64_t from, int timeout_ms = 8000);
    long read(void* out, std::size_t size); // 0 at the end, -1 on error
    void close();
    ~Stream() { close(); }
};
bool split_url(const std::string& url, std::string& host, int& port, std::string& path);

struct WebSocket {
    int fd = -1;
    std::string pending; // bytes received but not yet framed
    bool connect(const std::string& host, int port, const std::string& path, const std::string& bearer, int& http_status);
    bool send_text(const std::string& text);
    // One whole text message. 1: got one; 0: timeout; -1: closed.
    int receive(std::string& text, int timeout_ms);
    void close();
    ~WebSocket() { close(); }
};

// Accepts one connection at a time on a port. Returns the client fd, or -1 on timeout.
struct Listener {
    int fd = -1;
    bool listen_on(int port, bool loopback_only);
    int accept_one(int timeout_ms);
    void close();
    ~Listener() { close(); }
};
// Reads a request (headers + Content-Length body, bounded). false if malformed or too big.
bool read_request(int fd, std::string& method, std::string& path, std::string& body, std::size_t max_body);
void respond(int fd, int status, const char* reason, const std::string& body = "", const char* type = "application/json");

bool random_bytes(void* out, std::size_t size); // false: no randomness available
std::string base64(const unsigned char* data, std::size_t size);
}
