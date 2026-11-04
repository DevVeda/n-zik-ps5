// SPDX-License-Identifier: GPL-3.0-or-later
#include "i18n.hpp"
#include "pairing.hpp"
#include "json.hpp"
#include "log.hpp"
#include "net.hpp"
#include "qrcodegen.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <vector>

namespace pairing {

static std::uint32_t crc32(const unsigned char* d, std::size_t n, std::uint32_t c = 0) {
    c = ~c;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
static void be32(std::vector<unsigned char>& v, std::uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back((unsigned char)(x >> (8 * i)));
}
static void chunk(std::vector<unsigned char>& png, const char* type, const std::vector<unsigned char>& data) {
    be32(png, std::uint32_t(data.size()));
    std::vector<unsigned char> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    png.insert(png.end(), body.begin(), body.end());
    be32(png, crc32(body.data(), body.size()));
}

bool write_png(const char* path, const unsigned char* gray, int w, int h) {
    std::vector<unsigned char> raw; // filter byte 0 + row
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), gray + std::size_t(y) * std::size_t(w), gray + std::size_t(y + 1) * std::size_t(w));
    }
    std::vector<unsigned char> z{0x78, 0x01}; // zlib header, stored blocks
    std::uint32_t a = 1, b = 0;
    for (std::size_t at = 0; at < raw.size();) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size());
        z.push_back((unsigned char)(n & 255)); z.push_back((unsigned char)(n >> 8));
        z.push_back((unsigned char)(~n & 255)); z.push_back((unsigned char)((~n >> 8) & 255));
        for (std::size_t i = 0; i < n; ++i) { a = (a + raw[at + i]) % 65521; b = (b + a) % 65521; }
        z.insert(z.end(), raw.begin() + long(at), raw.begin() + long(at + n));
        at += n;
    }
    be32(z, b << 16 | a);
    std::vector<unsigned char> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}, ihdr;
    be32(ihdr, std::uint32_t(w));
    be32(ihdr, std::uint32_t(h));
    ihdr.insert(ihdr.end(), {8, 0, 0, 0, 0}); // 8-bit grayscale
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

std::string new_request_id() {
    unsigned char r[17]{};
    if (!net::random_bytes(r, sizeof r)) return ""; // never a guessable id
    std::string s = net::base64(r, sizeof r).substr(0, 22);
    for (char& c : s) c = c == '+' ? '-' : c == '/' ? '_' : c;
    return s;
}

bool make_qr(const std::string& request_id, const std::string& device_name, const std::string& ip, int port, const char* png_path,
             std::string& qr_text) {
    qr_text = "{\"type\":\"nzik-pair\",\"v\":1,\"requestId\":" + json::quote(request_id) + ",\"deviceName\":" + json::quote(device_name) +
              ",\"port\":" + std::to_string(port) + ",\"ips\":[" + json::quote(ip) + "]}";
    std::vector<std::uint8_t> qr(qrcodegen_BUFFER_LEN_MAX), tmp(qrcodegen_BUFFER_LEN_MAX);
    if (!qrcodegen_encodeText(qr_text.c_str(), tmp.data(), qr.data(), qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
        return false;
    // Black on white, 4-module quiet zone, scaled so the card shows it crisp.
    const int n = qrcodegen_getSize(qr.data()), quiet = 4, side = n + 2 * quiet, scale = 512 / side > 0 ? 512 / side : 1;
    const int px = side * scale;
    std::vector<unsigned char> img(std::size_t(px) * std::size_t(px), 255);
    for (int y = 0; y < px; ++y)
        for (int x = 0; x < px; ++x)
            if (qrcodegen_getModule(qr.data(), x / scale - quiet, y / scale - quiet)) img[std::size_t(y) * std::size_t(px) + std::size_t(x)] = 0;
    return write_png(png_path, img.data(), px, px);
}

static bool validate(const std::string& ip, int port, const std::string& code, const std::string& request_id,
                     const std::string& device_name, Pairing& out, std::string& status) {
    net::Response r;
    const std::string body = "{\"code\":" + json::quote(code) + ",\"requestId\":" + json::quote(request_id) +
                             ",\"deviceName\":" + json::quote(device_name) + "}";
    if (!net::request(ip, port, "POST", "/api/v1/pairing/validate", "", body, r, 64 << 10)) return false;
    json::Doc d;
    if (r.status != 200 || !d.parse(r.body)) {
        status = TR("The phone refused the code (", "El telefono rechazo el codigo (") + std::to_string(r.status) + ")";
        logf("PAIR validate %s:%d -> %d %s", ip.c_str(), port, r.status, r.body.c_str());
        return false;
    }
    out.token = d.str(d.get(0, "deviceToken"));
    out.device_id = d.str(d.get(0, "deviceId"));
    out.server_name = d.str(d.get(0, "serverName"));
    out.port = int(d.num(d.get(0, "serverPort"), port));
    return out.valid();
}

int open_listener(net::Listener& l) {
    for (int port = offer_port; port < offer_port + 10; port++) {
        if (l.listen_on(port, false)) return port;
        logf("PAIR listen %d failed: errno %d %s", port, errno, std::strerror(errno));
    }
    return 0;
}

bool wait_for_phone(net::Listener& l, const std::string& request_id, const std::string& device_name, int timeout_ms, Pairing& out, std::string& status,
                    const std::function<bool()>& cancel) {
    timespec start{};
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        timespec t{};
        clock_gettime(CLOCK_MONOTONIC, &t);
        const long elapsed = long(t.tv_sec - start.tv_sec) * 1000 + (t.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= timeout_ms || cancel()) return false;
        const int c = l.accept_one(int(std::min<long>(1000, timeout_ms - elapsed)));
        if (c < 0) continue;
        std::string method, path, body;
        json::Doc d;
        if (!net::read_request(c, method, path, body, 4096) || method != "POST" || path != "/nzik-pair/v1/offer" || !d.parse(body)) {
            net::respond(c, 404, "Not Found");
            close(c);
            continue;
        }
        // An offer for another QR, or one we already used: the phone asks for a new scan.
        if (d.str(d.get(0, "requestId")) != request_id || d.num(d.get(0, "v")) != 1) {
            net::respond(c, 410, "Gone");
            close(c);
            continue;
        }
        net::respond(c, 200, "OK", "{}");
        close(c);
        const std::string code = d.str(d.get(0, "code"));
        Pairing p;
        for (int ip : d.items(d.get(0, "serverIps"))) p.ips.push_back(d.str(ip));
        p.port = int(d.num(d.get(0, "serverPort")));
        logf("PAIR offer from %s port %d (%zu ips)", p.ips.empty() ? "?" : p.ips[0].c_str(), p.port, p.ips.size());
        status = TR("Pairing...", "Emparejando...");
        for (auto& ip : p.ips) {
            if (validate(ip, p.port, code, request_id, device_name, p, status)) {
                // Prefer the address that answered on later connections.
                std::swap(p.ips[0], *std::find(p.ips.begin(), p.ips.end(), ip));
                out = p;
                status = TR("Paired with ", "Emparejado con ") + p.server_name;
                return true;
            }
        }
        if (status == TR("Pairing...", "Emparejando...")) status = TR("Could not reach the phone", "No pude contactar al telefono");
        return false; // the QR is spent: the caller makes a new one
    }
}
}
