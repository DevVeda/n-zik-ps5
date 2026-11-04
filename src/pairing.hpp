// SPDX-License-Identifier: GPL-3.0-or-later
// QR pairing with N-Zik (contract §4.3–4.5): we show a QR, the phone scans it and pushes
// an offer (code + its address) to our temporary listener, we redeem the code for a token.
#pragma once
#include "bridge.hpp"
#include "net.hpp"
#include <string>

namespace pairing {
constexpr int offer_port = 40000; // first of 10 tried; the console refuses 42440-42449 (EACCES)
// Opens the offer listener on the first free port of offer_port..+9. Returns the port, 0 if none.
int open_listener(net::Listener& l);
// Writes the QR PNG to `png_path`. `qr_text` gets the JSON inside it (for logs/tests).
bool make_qr(const std::string& request_id, const std::string& device_name, const std::string& ip, int port,
             const char* png_path, std::string& qr_text);
std::string new_request_id(); // 22 chars of base64url
// Waits up to timeout_ms for the phone's offer on `l`, then validates it.
// true: `out` holds a working pairing. `status` says what happened (for the UI).
// Gives up early (false, empty status) once cancel(ctx) says so.
bool wait_for_phone(net::Listener& l, const std::string& request_id, const std::string& device_name, int timeout_ms, Pairing& out, std::string& status,
                    bool (*cancel)(void* ctx), void* ctx);
// Minimal grayscale PNG (stored deflate, no zlib).
bool write_png(const char* path, const unsigned char* gray, int width, int height);
}
