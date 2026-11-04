// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only JSON over jsmn tokens, plus string escaping for what we write.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#define JSMN_STATIC
#define JSMN_PARENT_LINKS
#define JSMN_STRICT
#include "jsmn.h"

namespace json {
struct Doc {
    std::string text;
    std::vector<jsmntok_t> t;
    bool parse(std::string s) {
        text = std::move(s);
        jsmn_parser p;
        jsmn_init(&p);
        const int n = jsmn_parse(&p, text.c_str(), text.size(), nullptr, 0);
        if (n <= 0) return false;
        t.resize(std::size_t(n));
        jsmn_init(&p);
        return jsmn_parse(&p, text.c_str(), text.size(), t.data(), unsigned(n)) == n;
    }
    bool ok(int i) const { return i >= 0 && i < int(t.size()); }
    const jsmntok_t& at(int i) const { return t[std::size_t(i)]; }
    // Index just past token i and all its children.
    int skip(int i) const {
        int end = i + 1;
        while (end < int(t.size()) && at(end).start < at(i).end) ++end;
        return end;
    }
    // Value of `key` in object `obj`, or -1. Every index is bounds-checked: input may be hostile.
    int get(int obj, const char* key) const {
        if (!ok(obj) || at(obj).type != JSMN_OBJECT) return -1;
        const std::size_t len = std::char_traits<char>::length(key);
        for (int i = obj + 1, n = 0; n < at(obj).size && ok(i); ++n) {
            const auto& k = at(i);
            if (k.type != JSMN_STRING || !ok(i + 1) || at(i + 1).start < k.end) return -1; // a key without a value
            if (std::size_t(k.end - k.start) == len && !text.compare(std::size_t(k.start), len, key)) return i + 1;
            i = skip(i + 1);
        }
        return -1;
    }
    // Elements of an array.
    std::vector<int> items(int arr) const {
        std::vector<int> out;
        if (!ok(arr) || at(arr).type != JSMN_ARRAY) return out;
        for (int i = arr + 1, n = 0; n < at(arr).size && ok(i); ++n, i = skip(i)) out.push_back(i);
        return out;
    }
    std::string raw(int i) const { return ok(i) ? text.substr(std::size_t(at(i).start), std::size_t(at(i).end - at(i).start)) : ""; }
    bool is_null(int i) const { return !ok(i) || (at(i).type == JSMN_PRIMITIVE && text[std::size_t(at(i).start)] == 'n'); }
    std::string str(int i) const {
        if (!ok(i) || at(i).type != JSMN_STRING) return "";
        const std::string r = raw(i);
        std::string out;
        for (std::size_t k = 0; k < r.size(); ++k) {
            if (r[k] != '\\' || k + 1 >= r.size()) { out += r[k]; continue; }
            const char c = r[++k];
            if (c == 'n') out += '\n';
            else if (c == 't') out += '\t';
            else if (c == 'r') out += '\r';
            else if (c == 'b') out += '\b';
            else if (c == 'f') out += '\f';
            else if (c == 'u' && k + 4 < r.size()) {
                unsigned v = unsigned(std::strtoul(r.substr(k + 1, 4).c_str(), nullptr, 16));
                k += 4;
                if (v >= 0xD800 && v < 0xDC00 && k + 6 < r.size() && r[k + 1] == '\\' && r[k + 2] == 'u') { // surrogate pair
                    const unsigned lo = unsigned(std::strtoul(r.substr(k + 3, 4).c_str(), nullptr, 16));
                    if (lo >= 0xDC00 && lo < 0xE000) { v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00); k += 6; }
                }
                if (v >= 0xD800 && v < 0xE000) v = '?'; // a lone surrogate: not valid UTF-8
                if (v < 0x80) out += char(v);
                else if (v < 0x800) { out += char(0xC0 | v >> 6); out += char(0x80 | (v & 63)); }
                else if (v < 0x10000) { out += char(0xE0 | v >> 12); out += char(0x80 | (v >> 6 & 63)); out += char(0x80 | (v & 63)); }
                else { out += char(0xF0 | v >> 18); out += char(0x80 | (v >> 12 & 63)); out += char(0x80 | (v >> 6 & 63)); out += char(0x80 | (v & 63)); }
            } else out += c;
        }
        return out;
    }
    long long num(int i, long long fallback = 0) const {
        if (!ok(i) || at(i).type != JSMN_PRIMITIVE || is_null(i)) return fallback;
        return std::strtoll(raw(i).c_str(), nullptr, 10);
    }
    bool boolean(int i, bool fallback = false) const {
        if (!ok(i) || at(i).type != JSMN_PRIMITIVE) return fallback;
        const char c = text[std::size_t(at(i).start)];
        return c == 't' ? true : c == 'f' ? false : fallback;
    }
};

inline std::string quote(const std::string& s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c == '\n') out += "\\n";
        else if (c < 0x20) { char e[8]; std::snprintf(e, sizeof e, "\\u%04x", c); out += e; }
        else out += char(c);
    }
    return out + "\"";
}
}
