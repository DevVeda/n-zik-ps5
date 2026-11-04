/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Kabuki provider SDK for C: put tiles in Kabuki's panel from a payload (protocol/PROTOCOL.md).
 *
 * Header only, plain C, POSIX sockets. A provider sends data; Kabuki draws the card.
 *
 *   kabuki_client k;
 *   kabuki_tile tiles[] = {{"HELLO1", "Hello", "icon:iconid_list", "list", 500}};
 *   if (kabuki_connect(&k) == 0 && kabuki_hello(&k, "hello", tiles, 1, NULL, 3000) == 0) {
 *       kabuki_content(&k, "HELLO1", "{\"top\":{\"rows\":[{\"key\":\"a\",\"title\":\"Hi\"}]}}");
 *       kabuki_event ev;
 *       while (kabuki_poll(&k, &ev, 1000) >= 0)
 *           if (ev.kind == KABUKI_ACTION) { ... ev.tile, ev.action, ev.key ... }
 *   }
 *
 * Kabuki isn't there until the connection works: retry kabuki_connect every 2 seconds. When the
 * connection closes (kabuki_poll returns -1) start again from kabuki_connect and say hello again.
 */
#ifndef KABUKI_H
#define KABUKI_H

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KABUKI_PORT 38590

/* One tile in hello. `icon` may be NULL; `card` is "media" or "list"; `priority` 0 to 899 (500 is usual). */
typedef struct {
    const char* id;
    const char* title;
    const char* icon;
    const char* card;
    int priority;
} kabuki_tile;

typedef enum {
    KABUKI_NONE = 0,
    KABUKI_ACTION, /* the user did something on one of your tiles: tile, action, key, value */
    KABUKI_SHOWN,  /* a tile's card came on screen: tile */
    KABUKI_ERROR,  /* something you sent was refused: code, message, tile */
    KABUKI_OTHER   /* a message this header doesn't know; ignore it */
} kabuki_kind;

typedef struct {
    kabuki_kind kind;
    char tile[16];
    char action[48];
    char key[264];
    int value;
    int has_value; /* value is set (seek, volume, shuffle, repeat) */
    char code[40];
    char message[160];
} kabuki_event;

typedef struct {
    int fd;
    size_t used;
    char in[16384];
} kabuki_client;

/* ---- JSON helpers (private) ---- */

/* Appends s as a JSON string (with quotes) to out. Returns the new length, or 0 if it doesn't fit. */
static inline size_t kabuki__quote(char* out, size_t cap, size_t at, const char* s) {
    if (at + 2 >= cap) return 0;
    out[at++] = '"';
    for (; *s; ++s) {
        const unsigned char c = (unsigned char)*s;
        char esc[8];
        size_t n = 0;
        if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; n = 2; }
        else if (c == '\n') { esc[0] = '\\'; esc[1] = 'n'; n = 2; }
        else if (c == '\r') { esc[0] = '\\'; esc[1] = 'r'; n = 2; }
        else if (c == '\t') { esc[0] = '\\'; esc[1] = 't'; n = 2; }
        else if (c < 0x20) { n = (size_t)snprintf(esc, sizeof esc, "\\u%04x", c); }
        else { esc[0] = (char)c; n = 1; }
        if (at + n + 2 >= cap) return 0;
        memcpy(out + at, esc, n);
        at += n;
    }
    out[at++] = '"';
    out[at] = 0;
    return at;
}

/* The string value of "name" in one of Kabuki's own lines (compact JSON, as Kabuki writes it). */
static inline int kabuki__string(const char* line, const char* name, char* out, size_t cap) {
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\":\"", name);
    const char* p = strstr(line, pattern);
    if (!p) { out[0] = 0; return 0; }
    p += strlen(pattern);
    size_t n = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            c = *p++;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'u') { /* \uXXXX: ASCII only; others become '?' */
                unsigned v = 0;
                for (int i = 0; i < 4 && *p; ++i, ++p) v = v * 16 + (unsigned)((*p >= 'a') ? *p - 'a' + 10 : (*p >= 'A') ? *p - 'A' + 10 : *p - '0');
                c = v < 0x80 ? (char)v : '?';
            }
        }
        if (n + 1 < cap) out[n++] = c;
    }
    out[n] = 0;
    return 1;
}

static inline int kabuki__number(const char* line, const char* name, int* out) {
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\":", name);
    const char* p = strstr(line, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    if (*p != '-' && (*p < '0' || *p > '9')) return 0;
    *out = (int)strtol(p, NULL, 10);
    return 1;
}

static inline int kabuki__write_all(int fd, const char* data, size_t size) {
    while (size) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = send(fd, data, size, MSG_NOSIGNAL);
#else
        const ssize_t n = send(fd, data, size, 0);
#endif
        if (n > 0) { data += n; size -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* ---- the client ---- */

/* Connects to Kabuki on this console. 0 on success, -1 if Kabuki isn't listening. */
static inline int kabuki_connect(kabuki_client* k) {
    memset(k, 0, sizeof *k);
    k->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (k->fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(KABUKI_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(k->fd, (struct sockaddr*)&a, sizeof a)) { close(k->fd); k->fd = -1; return -1; }
#ifdef SO_NOSIGPIPE
    { int yes = 1; setsockopt(k->fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes); }
#endif
    return 0;
}

/* Uses a connection you made yourself (a test, or another transport). */
static inline void kabuki_attach(kabuki_client* k, int fd) { memset(k, 0, sizeof *k); k->fd = fd; }

static inline void kabuki_close(kabuki_client* k) {
    if (k->fd >= 0) close(k->fd);
    k->fd = -1;
    k->used = 0;
}

/* Sends one JSON message (without the newline). 0 on success, -1 if the connection is gone. */
static inline int kabuki_send(kabuki_client* k, const char* json) {
    if (k->fd < 0) return -1;
    if (kabuki__write_all(k->fd, json, strlen(json)) || kabuki__write_all(k->fd, "\n", 1)) { kabuki_close(k); return -1; }
    return 0;
}

static inline void kabuki__event(const char* line, kabuki_event* ev) {
    memset(ev, 0, sizeof *ev);
    ev->kind = KABUKI_OTHER;
    if (strstr(line, "\"action\":{")) {
        ev->kind = KABUKI_ACTION;
        kabuki__string(line, "tile", ev->tile, sizeof ev->tile);
        /* "action":{"action":"select": the inner "action" is the second match */
        const char* inner = strstr(line, "\"action\":{");
        kabuki__string(inner + 10, "action", ev->action, sizeof ev->action);
        kabuki__string(line, "key", ev->key, sizeof ev->key);
        ev->has_value = kabuki__number(line, "value", &ev->value);
    } else if (strstr(line, "\"shown\":{")) {
        ev->kind = KABUKI_SHOWN;
        kabuki__string(line, "tile", ev->tile, sizeof ev->tile);
    } else if (strstr(line, "\"error\":{")) {
        ev->kind = KABUKI_ERROR;
        kabuki__string(line, "code", ev->code, sizeof ev->code);
        kabuki__string(line, "message", ev->message, sizeof ev->message);
        kabuki__string(line, "tile", ev->tile, sizeof ev->tile);
    }
}

/*
 * Waits up to timeout_ms for the next message from Kabuki (0: don't wait, -1: wait forever).
 * Returns 1 and fills ev, 0 if nothing came, -1 if the connection closed.
 */
static inline int kabuki_poll(kabuki_client* k, kabuki_event* ev, int timeout_ms) {
    for (;;) {
        char* end = (char*)memchr(k->in, '\n', k->used);
        if (end) {
            *end = 0;
            kabuki__event(k->in, ev);
            const size_t line = (size_t)(end - k->in) + 1;
            memmove(k->in, k->in + line, k->used - line);
            k->used -= line;
            return 1;
        }
        if (k->used == sizeof k->in) k->used = 0; /* a line too long for us: Kabuki doesn't send them */
        if (k->fd < 0) return -1;
        struct pollfd p = {k->fd, POLLIN, 0};
        const int ready = poll(&p, 1, timeout_ms);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { kabuki_close(k); return -1; }
        if (ready == 0) return 0;
        const ssize_t n = recv(k->fd, k->in + k->used, sizeof k->in - k->used - 1, 0);
        if (n <= 0) { kabuki_close(k); return -1; }
        k->used += (size_t)n;
        timeout_ms = 0; /* parse what came; don't wait twice */
    }
}

/*
 * Says hello and waits for welcome. `provider` is 1 to 24 of a-z, 0-9 and -. `art_dirs`: a JSON
 * array of folders for your pictures (for example "[\"/data/myapp/art\"]"), or NULL.
 * Returns 0 once welcomed, -1 if the connection closed or Kabuki refused (an error line arrived
 * first), -2 on timeout. Tiles Kabuki refuses arrive afterwards as KABUKI_ERROR events.
 */
static inline int kabuki_hello(kabuki_client* k, const char* provider, const kabuki_tile* tiles, int count, const char* art_dirs, int timeout_ms) {
    char out[8192];
    size_t at = 0;
    int n = snprintf(out, sizeof out, "{\"v\":1,\"hello\":{\"provider\":");
    if (n < 0 || (size_t)n >= sizeof out) return -1;
    at = (size_t)n;
    if (!(at = kabuki__quote(out, sizeof out, at, provider))) return -1;
    if (art_dirs) {
        n = snprintf(out + at, sizeof out - at, ",\"art_dirs\":%s", art_dirs);
        if (n < 0 || (size_t)n >= sizeof out - at) return -1;
        at += (size_t)n;
    }
    n = snprintf(out + at, sizeof out - at, ",\"tiles\":[");
    if (n < 0 || (size_t)n >= sizeof out - at) return -1;
    at += (size_t)n;
    for (int i = 0; i < count; ++i) {
        if (at + 16 >= sizeof out) return -1;
        if (i) out[at++] = ',';
        memcpy(out + at, "{\"id\":", 6); at += 6;
        if (!(at = kabuki__quote(out, sizeof out, at, tiles[i].id))) return -1;
        memcpy(out + at, ",\"title\":", 9); at += 9;
        if (!(at = kabuki__quote(out, sizeof out, at, tiles[i].title ? tiles[i].title : ""))) return -1;
        if (tiles[i].icon) {
            memcpy(out + at, ",\"icon\":", 8); at += 8;
            if (!(at = kabuki__quote(out, sizeof out, at, tiles[i].icon))) return -1;
        }
        memcpy(out + at, ",\"card\":", 8); at += 8;
        if (!(at = kabuki__quote(out, sizeof out, at, tiles[i].card ? tiles[i].card : "list"))) return -1;
        n = snprintf(out + at, sizeof out - at, ",\"priority\":%d}", tiles[i].priority ? tiles[i].priority : 500);
        if (n < 0 || (size_t)n >= sizeof out - at) return -1;
        at += (size_t)n;
    }
    n = snprintf(out + at, sizeof out - at, "]}}");
    if (n < 0 || (size_t)n >= sizeof out - at) return -1;
    if (kabuki_send(k, out)) return -1;
    for (;;) {
        kabuki_event ev;
        const int got = kabuki_poll(k, &ev, timeout_ms);
        if (got < 0) return -1;
        if (got == 0) return -2;
        if (ev.kind == KABUKI_ERROR) return -1;
        if (ev.kind == KABUKI_OTHER) return 0; /* the welcome: the only other message before tiles are judged */
    }
}

/* Sends data for one of your tiles. `data_json` is a JSON object, for example {"title":"Song"}. */
static inline int kabuki_content(kabuki_client* k, const char* tile, const char* data_json) {
    char head[96];
    char quoted[40];
    if (!kabuki__quote(quoted, sizeof quoted, 0, tile)) return -1;
    const int n = snprintf(head, sizeof head, "{\"v\":1,\"content\":{\"tile\":%s,\"data\":", quoted);
    if (n < 0 || (size_t)n >= sizeof head || k->fd < 0) return -1;
    if (kabuki__write_all(k->fd, head, (size_t)n) || kabuki__write_all(k->fd, data_json, strlen(data_json)) ||
        kabuki__write_all(k->fd, "}}\n", 3)) { kabuki_close(k); return -1; }
    return 0;
}

/* Asks Kabuki to bring one of your tiles on screen (when the panel is open). */
static inline int kabuki_show(kabuki_client* k, const char* tile) {
    char out[96], quoted[40];
    if (!kabuki__quote(quoted, sizeof quoted, 0, tile)) return -1;
    snprintf(out, sizeof out, "{\"v\":1,\"show\":{\"tile\":%s}}", quoted);
    return kabuki_send(k, out);
}

/* A short note about your provider (up to 60 characters); NULL clears it. */
static inline int kabuki_status(kabuki_client* k, const char* note) {
    char out[256];
    if (!note) return kabuki_send(k, "{\"v\":1,\"status\":{\"note\":null}}");
    char quoted[200];
    if (!kabuki__quote(quoted, sizeof quoted, 0, note)) return -1;
    snprintf(out, sizeof out, "{\"v\":1,\"status\":{\"note\":%s}}", quoted);
    return kabuki_send(k, out);
}

#ifdef __cplusplus
}
#endif
#endif /* KABUKI_H */
