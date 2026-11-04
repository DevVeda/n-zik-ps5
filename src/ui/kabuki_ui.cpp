// SPDX-License-Identifier: GPL-3.0-or-later
#include "kabuki_ui.hpp"
#include "../i18n.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../notify.hpp"
#include "../offline.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

using json::quote;

static std::uint64_t now_ms() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return std::uint64_t(t.tv_sec) * 1000 + std::uint64_t(t.tv_nsec) / 1000000;
}
static std::string duration(long long ms) {
    if (ms <= 0) return "";
    char t[16];
    std::snprintf(t, sizeof t, "%lld:%02lld", ms / 60000, ms / 1000 % 60);
    return t;
}
static const char* ownership_name(int o) {
    return o == 1 ? TR("A (direct)", "A (directo)") : o == 2 ? TR("B (system source)", "B (fuente del sistema)")
         : o == 3 ? TR("C (source + AudioTag)", "C (fuente + AudioTag)") : TR("Automatic (B)", "Automatico (B)");
}
// How to move, as Kabuki does it: in a list, Right goes back to the tiles and Circle closes the
// panel; in an opened list, Circle steps back out of it.
static std::string nav(bool sub, const std::string& extra = "") {
    const char* keys = sub ? TR("○ Back", "○ Atras") : TR("→ Tiles   ○ Close", "→ Pestanas   ○ Cerrar");
    return std::string(keys) + (extra.empty() ? "" : "   ·   " + extra);
}
// A list row and a list level, as Kabuki's `list` card takes them (protocol: `level`, `rows`).
// Rows are replaced as a whole, so a row leaves out what it doesn't have; a level always says its
// art and heading, or Kabuki would keep the previous sublist's.
struct Row {
    std::string key, title, subtitle, art;
    bool opens = false;
};
static std::string json_of(const Row& r) {
    std::string d = "{\"key\":" + quote(r.key) + ",\"title\":" + quote(r.title);
    if (!r.subtitle.empty()) d += ",\"subtitle\":" + quote(r.subtitle);
    if (!r.art.empty()) d += ",\"art\":" + quote(r.art);
    return d + (r.opens ? ",\"opens\":true}" : "}");
}
struct Level {
    std::string title, heading, art;
    bool covers = false, loading = false;
    std::string empty_title, empty_detail; // shown when there are no rows
    std::vector<Row> rows;
};
static std::string json_of(const Level& l) {
    std::string d = "{\"title\":" + quote(l.title) + ",\"heading\":" + quote(l.heading) + ",\"art\":" + (l.art.empty() ? "null" : quote(l.art));
    d += std::string(",\"covers\":") + (l.covers ? "true" : "false");
    if (l.loading) d += ",\"loading\":true";
    if (!l.empty_title.empty()) d += ",\"empty\":{\"title\":" + quote(l.empty_title) + ",\"detail\":" + quote(l.empty_detail) + "}";
    d += ",\"rows\":[";
    for (std::size_t i = 0; i < l.rows.size(); ++i) d += (i ? "," : "") + json_of(l.rows[i]);
    return d + "]}";
}
// A whole list tile: its top level and, while one is open, a sublist.
static void send_list(kabuki_client& k, const char* tile, const std::string& session, const Level& top, const std::string& sub_key = "",
                      const Level* sub = nullptr, const std::string& busy = "") {
    std::string d = "{\"session\":" + quote(session) + ",\"busy\":" + (busy.empty() ? "null" : quote(busy)) + ",\"top\":" + json_of(top);
    d += sub ? ",\"sub_key\":" + quote(sub_key) + ",\"sub\":" + json_of(*sub) : std::string(",\"sub_key\":null");
    kabuki_content(&k, tile, (d + "}").c_str());
}
static void add_songs(std::vector<Row>& rows, const std::vector<Track>& songs, std::size_t max = 300) {
    for (std::size_t i = 0; i < songs.size() && i < max; ++i) {
        std::string sub = songs[i].artists;
        if (songs[i].duration_ms > 0) sub += (sub.empty() ? "" : "  ·  ") + duration(songs[i].duration_ms);
        rows.push_back({std::to_string(i), songs[i].title, sub});
    }
}
// A row that cycles a choice: "<label><choice>", with how to change it underneath.
static Row choice_row(const char* key, const char* label, const char* choice, const char* hint) { return {key, std::string(label) + choice, hint}; }

// UI rule: every sublist goes back with Circle. Kabuki keeps the cursor in an opened list only
// while it loads or has rows (native_model.hpp, enterable); an empty one loses it and Circle then
// closes the whole panel. So an opened list is never empty: this row stands in, and selecting it
// starts a new session, which closes the list.
static Row back_row(const char* why_en, const char* why_es) { return {"back", TR("← Back", "← Volver"), TR(why_en, why_es)}; }

Row KabukiUi::mode_row() {
    static const char* const names[][2] = {{"Play", "Reproducir"}, {"Play next", "Reproducir siguiente"}, {"Add to the end", "Agregar al final"}};
    return choice_row("addmode", "✕: ", TR(names[add_mode][0], names[add_mode][1]),
                      TR("Cross here changes what Cross does on a song", "Cross aqui cambia lo que hace Cross en una cancion"));
}

// A phone song chosen in Explore: play the list from it, or put just that song in the phone's queue.
void KabukiUi::phone_song(kabuki_client& k, const std::string& list, const std::vector<Track>& songs, int index) {
    if (index < 0 || index >= int(songs.size())) return;
    if (add_mode == 0) {
        app.local.stop(); // the phone plays again
        if (library::play(app.bridge, list, songs, index, false) == 200) kabuki_show(&k, "NZKNOW");
        return;
    }
    const Track& t = songs[std::size_t(index)];
    if (library::add(app.bridge, t, add_mode == 1) == 200)
        notify("%s: %s", add_mode == 1 ? TR("Plays next", "Suena despues") : TR("Added to the queue", "Agregada a la cola"), t.title.c_str());
}

// A song on the console: the same, with the console's own queue (adding starts it if nothing plays).
void KabukiUi::local_song(kabuki_client& k, const std::vector<Track>& songs, int index) {
    if (index < 0 || index >= int(songs.size())) return;
    const bool starts = add_mode == 0 || !app.local.active();
    if (starts && app.bridge.snapshot().playing) app.bridge.command("player/pause"); // one music at a time
    if (add_mode == 0) app.local.play(songs, index);
    else {
        app.local.add(songs[std::size_t(index)], add_mode == 1);
        if (!starts)
            notify("%s: %s", add_mode == 1 ? TR("Plays next", "Suena despues") : TR("Added to the queue", "Agregada a la cola"),
                   songs[std::size_t(index)].title.c_str());
    }
    if (starts) kabuki_show(&k, "NZKNOW");
    push_all(k, true);
}

PlayerState KabukiUi::current() { return app.local.active() ? app.local.snapshot() : app.bridge.snapshot(); }

void KabukiUi::fetch_art(const PlayerState& s) {
    const Track* t = s.current();
    if (app.local.active()) { // a local song: its folder's cover, read in place
        const std::string cover = t ? local::cover(t->id.substr(0, t->id.rfind('/'))) : "";
        if (cover == seen.art_id) return;
        if (!art_path.empty() && art_path.compare(0, paths::art.size(), paths::art) == 0) std::remove(art_path.c_str());
        seen.art_id = art_path = cover;
        return;
    }
    const std::string id = t && t->has_art ? t->id : "";
    if (id == seen.art_id) return;
    seen.art_id = id;
    if (!art_path.empty() && art_path.compare(0, paths::art.size(), paths::art) == 0) std::remove(art_path.c_str()); // one cover on disk at a time
    art_path.clear();
    std::string bytes, type;
    if (id.empty() || !app.bridge.artwork(id, 544, bytes, type) || bytes.size() < 4) return;
    // Kabuki takes JPEG or PNG only.
    const bool png = bytes.compare(0, 4, "\x89PNG") == 0, jpg = bytes.compare(0, 2, "\xff\xd8") == 0;
    if (!png && !jpg) { logf("UI artwork %s is %s: skipped", id.c_str(), type.c_str()); return; }
    std::string safe;
    for (char c : id) safe += std::isalnum((unsigned char)c) || c == '-' || c == '_' ? c : '_';
    const std::string path = paths::art + "/cover-" + safe + (png ? ".png" : ".jpg");
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        if (std::fclose(f) == 0 && ok) art_path = path;
    }
}

void KabukiUi::push_now(kabuki_client& k, const PlayerState& s) {
    const Track* t = s.current();
    const auto now = now_ms();
    std::string status, qr;
    {
        std::lock_guard<std::mutex> l(app.m);
        status = app.pair_status;
        qr = app.pair_qr;
    }
    std::string d = "{";
    if (!qr.empty()) { // pairing: the biggest picture Kabuki draws (286 px); a list header's is 48 px
        d += "\"title\":" + quote(TR("Scan with N-Zik", "Escanea con N-Zik")) + ",\"subtitle\":" +
             quote(TR("Phone: N-Zik > your avatar > PC Server > Start server > Pair", "Telefono: N-Zik > tu avatar > PC Server > Start server > Pair"));
        d += ",\"art\":" + quote(qr) + ",\"duration_ms\":0";
    } else if (t) {
        d += "\"title\":" + quote(t->title) + ",\"subtitle\":" + quote(t->artists);
        d += ",\"art\":" + (art_path.empty() ? std::string("null") : quote(art_path));
        const long long len = s.duration_ms > 0 ? s.duration_ms : t->duration_ms;
        d += ",\"duration_ms\":" + std::to_string(len > 0 ? len : 0);
    } else {
        d += "\"title\":" + quote(s.connected ? TR("Nothing playing", "Nada sonando") : "N-Zik") + ",\"subtitle\":" +
             quote(s.connected ? TR("Pick music in Explore, Music or on the phone", "Elige musica en Explorar, Musica o el telefono")
                               : app.bridge.paired() ? s.problem : TR("Pair your phone: N-Zik > Settings", "Empareja tu telefono: N-Zik > Ajustes")) +
             ",\"art\":null,\"duration_ms\":0";
    }
    std::string note = app.player.status();
    if (!qr.empty()) note = status;
    else if (app.local.active()) note = TR("On the console  ·  ", "En la consola  ·  ") + note;
    else if (s.connected && s.output != "pc") note = TR("Moving the audio to the PS5...", "Pasando el audio al PS5...");
    d += ",\"note\":" + quote(note);
    d += ",\"position_ms\":" + std::to_string(t ? s.position_now(now) : 0);
    d += std::string(",\"moving\":") + (t && s.playing ? "true" : "false");
    d += std::string(",\"playing\":") + (s.playing ? "true" : "false");
    d += std::string(",\"loading\":") + (s.buffering ? "true" : "false");
    d += std::string(",\"shuffle\":") + (s.shuffle ? "true" : "false");
    d += ",\"repeat\":" + quote(s.repeat.empty() ? "off" : s.repeat);
    d += ",\"volume\":" + std::to_string(app.player.volume.load());
    d += "}";
    kabuki_content(&k, "NZKNOW", d.c_str());
}

std::string KabukiUi::like_of(const Track& t) {
    for (const auto& [id, state] : like_state) if (id == t.id) return state;
    return t.liked ? "liked" : t.disliked ? "disliked" : "neutral";
}
void KabukiUi::toggle_like(const Track& t, const char* state) {
    const std::string next = like_of(t) == state ? "neutral" : state;
    if (app.bridge.command(("library/songs/" + t.id + "/like").c_str(), "{\"state\":\"" + next + "\"}") != 200) return;
    for (auto& [id, state] : like_state) if (id == t.id) { state = next; return; }
    like_state.emplace_back(t.id, next);
}
const char* KabukiUi::offline_title(const Track& t) {
    return offline::busy(t.id) ? TR("Saving offline...", "Guardando offline...")
         : offline::saved(t) ? TR("Saved offline", "Guardada offline") : TR("Save offline", "Guardar offline");
}

std::string KabukiUi::offline_row(const PlayerState& s) {
    const Track* t = s.current();
    if (!t || app.local.active()) return "";
    return offline_title(*t) + std::string("|") + t->artists; // what the row shows, to notice when it changes
}

std::vector<Row> KabukiUi::settings_rows() {
    const bool paired = app.bridge.paired();
    const PlayerState phone = app.bridge.snapshot();
    const std::string conn = phone.connected ? TR("Connected", "Conectado") : paired ? phone.problem : TR("Not paired", "Sin emparejar");
    std::vector<Row> rows{
        {"status", TR("Phone: ", "Telefono: ") + conn, app.player.status()},
        {"pair", paired ? TR("Pair another phone", "Emparejar otro telefono") : TR("Pair phone", "Emparejar telefono"),
         TR("Shows a QR in Now Playing", "Muestra un QR en Sonando")},
        choice_row("quality", TR("Quality: ", "Calidad: "), app.player.low_quality ? TR("low", "baja") : TR("high", "alta"),
                   TR("Low uses less Wi-Fi", "Baja usa menos Wi-Fi")),
        choice_row("toasts", TR("Song notices: ", "Avisos de cancion: "), app.player.song_toasts ? TR("on", "si") : TR("off", "no"),
                   TR("A notification when each song starts", "Notificacion al empezar cada cancion")),
        {"mode", std::string(TR("Audio mode: ", "Modo de audio: ")) + ownership_name(app.config.ownership), app.audio_info},
    };
    if (!phone.queue.empty()) {
        const bool armed = clear_armed && now_ms() - clear_armed < 5000;
        rows.push_back({"clear", armed ? TR("Cross again to clear the queue", "Cross otra vez para vaciar la cola") : TR("Clear the queue", "Vaciar la cola"),
                        std::to_string(phone.queue.size()) + TR(" songs, on the phone too", " canciones, tambien en el telefono")});
    }
    if (paired) rows.push_back({"forget", TR("Forget this phone", "Olvidar este telefono")});
    rows.push_back({"quit", TR("Quit N-Zik", "Cerrar N-Zik"), TR("Stops the music and the payload", "Detiene la musica y el payload")});
    return rows;
}

void KabukiUi::push_queue(kabuki_client& k, const PlayerState& s) {
    const bool on_console = app.local.active();
    Level top{"N-Zik", nav(false, s.queue.empty() ? "" : std::to_string(s.queue.size()) +
                                    (on_console ? TR(" songs on the console", " canciones en la consola") : TR(" songs in the queue", " canciones en la cola")))};
    if (const Track* t = on_console ? nullptr : s.current()) { // like, dislike and save the song that's playing
        const std::string like = like_of(*t);
        top.rows.push_back({"like", like == "liked" ? TR("Liked  (Cross to undo)", "Te gusta  (Cross para quitar)") : TR("Like", "Me gusta"), t->title});
        top.rows.push_back({"dislike", like == "disliked" ? TR("Disliked  (Cross to undo)", "No te gusta  (Cross para quitar)") : TR("Dislike", "No me gusta"),
                            TR("N-Zik plays it less", "N-Zik la pondra menos")});
        top.rows.push_back({"offline", offline_title(*t), TR("On the console: Music > ", "En la consola: Musica > ") + offline::clean(t->artists)});
    }
    top.rows.push_back({"settings", TR("Settings", "Ajustes"), TR("Phone, quality, notices, audio mode", "Telefono, calidad, avisos, modo de audio"), "", true});
    if (!on_console && !s.queue.empty()) {
        static const char* const modes[][2] = {{"Jump to it", "Saltar a ella"}, {"Remove it", "Quitarla"}, {"Move it up", "Subirla"}, {"Move it down", "Bajarla"}};
        top.rows.push_back(choice_row("qmode", TR("✕ on a song: ", "✕ en una cancion: "), TR(modes[queue_mode][0], modes[queue_mode][1]),
                                      TR("Cross here changes it", "Cross aqui lo cambia")));
    }
    // Kabuki shows up to 500 rows (and lines up to 64 KiB): the current song and what follows matter most.
    const int max = 300;
    const int from = int(s.queue.size()) > max && s.current_index > 0 ? std::min(s.current_index, int(s.queue.size()) - max) : 0;
    for (int i = from; i < int(s.queue.size()) && i < from + max; ++i) {
        const Track& t = s.queue[std::size_t(i)];
        std::string sub = t.artists;
        if (t.duration_ms > 0) sub += (sub.empty() ? "" : "  ·  ") + duration(t.duration_ms);
        if (i == s.current_index) sub = TR("Playing  ·  ", "Sonando  ·  ") + sub;
        top.rows.push_back({std::to_string(i) + "|" + (on_console ? std::string("local") : t.id), t.title, sub});
    }
    Level settings{TR("Settings", "Ajustes"), nav(true)};
    if (settings_open) settings.rows = settings_rows();
    send_list(k, "NZKQUE", session, top, "settings", settings_open ? &settings : nullptr, busy_key);
}

library::Entry* KabukiUi::entry(const std::string& key) {
    for (auto& e : lib_top) if (e.key == key) return &e;
    return nullptr;
}

static std::string cover_base(const std::string& key) {
    std::string name = "lib-";
    for (const char c : key) name += std::isalnum((unsigned char)c) || c == '-' || c == '_' ? c : '_';
    return paths::art + "/" + name;
}
std::string KabukiUi::lib_cover(const std::string& key) {
    for (const char* ext : {".jpg", ".png"})
        if (access((cover_base(key) + ext).c_str(), R_OK) == 0) return cover_base(key) + ext;
    return "";
}
void KabukiUi::fetch_cover(kabuki_client& k) {
    const std::string key = covers_todo.back();
    covers_todo.pop_back();
    std::string bytes;
    const library::Entry* e = entry(key);
    if (e && library::artwork(app.bridge, *e, 128, bytes) && bytes.size() > 4) {
        const bool png = bytes.compare(0, 4, "\x89PNG") == 0, jpg = bytes.compare(0, 2, "\xff\xd8") == 0;
        const std::string path = cover_base(key) + (png ? ".png" : ".jpg");
        if (png || jpg)
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
                if (std::fclose(f) != 0 || !ok) std::remove(path.c_str());
            }
    }
    if (covers_todo.empty() || covers_todo.size() % 8 == 0) push_library(k); // show them in batches
}

void KabukiUi::push_library(kabuki_client& k, bool loading) {
    Level top{TR("Explore", "Explorar"), nav(false)};
    top.covers = lib_view != 4;
    top.loading = loading;
    if (!loading) {
        top.empty_title = app.bridge.paired() ? TR("The phone is not answering", "El telefono no responde") : TR("No phone", "Sin telefono");
        top.empty_detail = TR("Start the PC Server in N-Zik on the phone and come back to this tab", "Inicia el PC Server de N-Zik en el telefono y vuelve a esta pestana");
    }
    if (!loading && !lib_top.empty()) { // the phone didn't answer: just the empty notice
        static const char* const names[][2] = {{"Everything", "Todo"}, {"Playlists", "Playlists"}, {"Albums", "Albumes"}, {"Artists", "Artistas"}, {"Songs", "Canciones"}};
        top.rows.push_back(choice_row("view", TR("Show: ", "Ver: "), TR(names[lib_view][0], names[lib_view][1]),
                                      TR("Cross: everything, playlists, albums, artists, songs", "Cross: todo, playlists, albumes, artistas, canciones")));
        if (lib_view == 4) {
            top.rows.push_back(mode_row());
            add_songs(top.rows, all_songs, 400);
        } else {
            const char prefix = " par"[lib_view]; // the entries of one kind: p: playlists, a: albums, r: artists
            for (const auto& e : lib_top) {
                if (top.rows.size() > 480) break;
                if (lib_view && !(e.key.size() > 1 && e.key[0] == prefix && e.key[1] == ':')) continue;
                top.rows.push_back({e.key, e.title, e.subtitle, lib_cover(e.key), true});
            }
        }
    }
    if (lib_key.empty()) return send_list(k, "NZKLIB", session + "." + std::to_string(lib_back), top);
    const library::Entry* e = entry(lib_key);
    Level sub{e ? e->title : "", nav(true)};
    sub.loading = lib_fetching;
    // Never an empty list: it can't hold the focus, and then Circle closes the panel instead.
    if (lib_songs.empty() && !lib_fetching) sub.rows.push_back(back_row("No songs here, or the phone did not answer", "Aqui no hay canciones, o el telefono no respondio"));
    // Save / follow / pin the opened playlist, album or artist (the phone's own toggles).
    if (e && !lib_fetching && e->key.size() > 2 && e->key[1] == ':') {
        const char kind = e->key[0];
        const char* save = kind == 'r' ? (e->saved ? TR("Following  (Cross to undo)", "Siguiendo  (Cross para dejar)") : TR("Follow artist", "Seguir artista"))
                         : kind == 'a' ? (e->saved ? TR("Album saved  (Cross to undo)", "Album guardado  (Cross para quitar)") : TR("Save album", "Guardar album"))
                                       : (e->saved ? TR("Playlist saved  (Cross to undo)", "Playlist guardada  (Cross para quitar)") : TR("Save playlist", "Guardar playlist"));
        sub.rows.push_back({"flag:save", save, TR("In N-Zik's library", "En la biblioteca de N-Zik")});
        if (kind == 'p')
            sub.rows.push_back({"flag:pin", e->pinned ? TR("Pinned  (Cross to undo)", "Fijada  (Cross para quitar)") : TR("Pin playlist", "Fijar playlist"),
                                TR("At the top of N-Zik's playlists", "Arriba en las playlists de N-Zik")});
    }
    if (!lib_songs.empty()) {
        sub.rows.push_back(mode_row());
        sub.rows.push_back({"shuffle", TR("Shuffle", "Aleatorio"), std::to_string(lib_songs.size()) + TR(" songs", " canciones")});
        add_songs(sub.rows, lib_songs);
    }
    send_list(k, "NZKLIB", session + "." + std::to_string(lib_back), top, lib_key, &sub);
}

void KabukiUi::refresh_local() {
    albums = local::albums();
    loc_artists.clear();
    for (const auto& a : albums)
        if (!a.artist.empty() && std::find(loc_artists.begin(), loc_artists.end(), a.artist) == loc_artists.end()) loc_artists.push_back(a.artist);
    std::sort(loc_artists.begin(), loc_artists.end());
    loc_all.clear();
    if (loc_view == 2) {
        for (const auto& a : albums) for (auto& t : local::songs(a.dir)) loc_all.push_back(std::move(t));
        std::sort(loc_all.begin(), loc_all.end(), [](const Track& x, const Track& y) { return x.title < y.title; });
    }
    if (album >= int(albums.size())) album = -1;
    if (loc_artist >= int(loc_artists.size())) loc_artist = -1;
}

void KabukiUi::push_local(kabuki_client& k) {
    Level top{TR("Music on the console", "Musica en la consola"), nav(false)};
    top.covers = true;
    top.empty_title = TR("No music in /data/music", "No hay musica en /data/music");
    top.empty_detail = TR("Copy Artist/Album folders by FTP, or use Save offline in N-Zik", "Copia carpetas Artista/Album por FTP, o usa Guardar offline en N-Zik");
    const PlayerState q = app.local.snapshot();
    if (app.local.active() && !q.queue.empty()) {
        const Track* t = q.current();
        top.rows.push_back({"queue", TR("Console queue", "Cola de la consola"),
                            std::to_string(q.queue.size()) + TR(" songs", " canciones") + (t ? "  ·  " + t->title : std::string()), "", true});
    }
    if (!albums.empty()) {
        static const char* const names[][2] = {{"Albums", "Albumes"}, {"Artists", "Artistas"}, {"Songs", "Canciones"}};
        top.rows.push_back(choice_row("view", TR("Show: ", "Ver: "), TR(names[loc_view][0], names[loc_view][1]),
                                      TR("Cross: albums, artists, songs", "Cross: albumes, artistas, canciones")));
    }
    if (loc_view == 0)
        for (std::size_t i = 0; i < albums.size() && i < 480; ++i)
            top.rows.push_back({std::to_string(i), albums[i].title, albums[i].artist, local::cover(albums[i].dir), true});
    else if (loc_view == 1)
        for (std::size_t i = 0; i < loc_artists.size() && i < 480; ++i) {
            int count = 0;
            std::string art;
            for (const auto& a : albums)
                if (a.artist == loc_artists[i]) { ++count; if (art.empty()) art = local::cover(a.dir); }
            top.rows.push_back({"r:" + std::to_string(i), loc_artists[i], std::to_string(count) + (count == 1 ? TR(" album", " album") : TR(" albums", " albumes")), art, true});
        }
    else if (!albums.empty()) {
        top.rows.push_back(mode_row());
        for (std::size_t i = 0; i < loc_all.size() && i < 400; ++i)
            top.rows.push_back({"s:" + std::to_string(i), loc_all[i].title, loc_all[i].artists, local::cover(loc_all[i].id.substr(0, loc_all[i].id.rfind('/')))});
    }
    const std::string session_key = session + "." + std::to_string(loc_back);
    const char* gone_en = "The songs are gone from this folder";
    const char* gone_es = "Las canciones ya no estan en esta carpeta";
    const auto song_list = [&](const std::string& key, const std::string& title, const std::string& art, const std::vector<Track>& songs) {
        Level sub{title, nav(true), art};
        if (songs.empty()) sub.rows.push_back(back_row(gone_en, gone_es));
        else {
            sub.rows.push_back(mode_row());
            add_songs(sub.rows, songs);
        }
        send_list(k, "NZKLOC", session_key, top, key, &sub);
    };
    if (queue_open) {
        Level sub{TR("Console queue", "Cola de la consola"), nav(true)};
        for (std::size_t i = 0; i < q.queue.size() && i < 400; ++i)
            sub.rows.push_back({std::to_string(i), q.queue[i].title, (int(i) == q.current_index ? TR("Playing  ·  ", "Sonando  ·  ") : "") + q.queue[i].artists});
        if (sub.rows.empty()) sub.rows.push_back(back_row("Nothing is playing on the console", "No suena nada en la consola"));
        send_list(k, "NZKLOC", session_key, top, "queue", &sub);
    } else if (album >= 0 && album < int(albums.size()))
        song_list(std::to_string(album), albums[std::size_t(album)].title, local::cover(albums[std::size_t(album)].dir), album_songs);
    else if (loc_artist >= 0 && loc_artist < int(loc_artists.size())) {
        std::string art;
        for (const auto& a : albums) if (a.artist == loc_artists[std::size_t(loc_artist)] && art.empty()) art = local::cover(a.dir);
        song_list("r:" + std::to_string(loc_artist), loc_artists[std::size_t(loc_artist)], art, artist_songs);
    } else send_list(k, "NZKLOC", session_key, top);
}

void KabukiUi::push_all(kabuki_client& k, bool force) {
    const PlayerState s = current();
    // Two sources, two version counters: tell them apart.
    const std::uint64_t version = s.version ^ (app.local.active() ? 1ull << 63 : 0);
    const auto now = now_ms();
    std::uint64_t pair;
    {
        std::lock_guard<std::mutex> l(app.m);
        pair = app.pair_version;
    }
    const std::string status = app.player.status();
    if (const auto lv = app.local.version(); lv != seen.local) { // the console's queue, shown in Music
        seen.local = lv;
        push_local(k);
    }
    if (!busy_key.empty() && now - busy_at > 8000) busy_key.clear(); // the phone never answered
    // Track/queue changes: everything. Otherwise once a second (position, player note).
    if (force || version != seen.state || pair != seen.pair || status != seen.status || now - seen.second >= 1000) {
        const bool changed = force || version != seen.state;
        const std::string extras = offline_row(s);
        fetch_art(s);
        push_now(k, s);
        if (changed || extras != seen.extras || (settings_open && (pair != seen.pair || status != seen.status || now - seen.second >= 1000)))
            push_queue(k, s);
        seen.state = version;
        seen.pair = pair;
        seen.status = status;
        seen.second = now;
        seen.extras = extras;
    }
}

void KabukiUi::on_shown(kabuki_client& k, const std::string& tile) {
    if (tile == "NZKLIB" && (!lib_loaded || lib_top.empty())) {
        all_songs.clear(); // fetched again when the Songs view is next shown
        push_library(k, true);
        lib_loaded = library::top(app.bridge, lib_top);
        covers_todo.clear();
        for (auto it = lib_top.rbegin(); it != lib_top.rend(); ++it) // fetched from the back: the list's order
            if (it->has_art && lib_cover(it->key).empty()) covers_todo.push_back(it->key);
        push_library(k);
    } else if (tile == "NZKLOC") {
        app.local.prune(); // files deleted since (FTP) leave the console's queue
        refresh_local();    // cheap, and picks up what was copied or saved since
        push_local(k);
    }
}

void KabukiUi::on_action(kabuki_client& k, const kabuki_event& ev) {
    const std::string tile = ev.tile, action = ev.action, key = ev.key;
    const PlayerState s = current();
    const bool on_console = app.local.active();
    if (tile == "NZKNOW") {
        if (on_console) { // our own queue
            if (action == "play_pause") app.local.toggle();
            else if (action == "next") app.local.next();
            else if (action == "previous") app.local.previous();
            else if (action == "seek" && ev.has_value) app.local.seek(ev.value);
        } else if (action == "play_pause") app.bridge.command(s.playing ? "player/pause" : "player/play");
        else if (action == "next") app.bridge.command("player/next");
        else if (action == "previous") app.bridge.command("player/previous");
        else if (action == "seek" && ev.has_value) app.bridge.command("player/seek", "{\"positionMs\":" + std::to_string(ev.value) + "}");
        else if (action == "shuffle" && ev.has_value) app.bridge.command("player/shuffle", std::string("{\"enabled\":") + (ev.value ? "true}" : "false}"));
        else if (action == "repeat" && ev.has_value)
            app.bridge.command("player/repeat", std::string("{\"mode\":\"") + (ev.value == 2 ? "one" : ev.value == 1 ? "all" : "off") + "\"}");
        if (action == "volume" && ev.has_value) {
            app.player.volume = ev.value < 0 ? 0 : ev.value > 100 ? 100 : ev.value;
            app.save();
        }
        push_all(k, true);
    } else if (tile == "NZKQUE") {
        if (action == "open" && key == "settings") settings_open = true;
        else if (action == "close") settings_open = false;
        else if (action == "select" && (key == "like" || key == "dislike")) {
            if (const Track* t = on_console ? nullptr : s.current()) toggle_like(*t, key == "like" ? "liked" : "disliked");
        } else if (action == "select" && key == "qmode") queue_mode = (queue_mode + 1) % 4;
        else if (action == "select" && key == "clear") {
            if (clear_armed && now_ms() - clear_armed < 5000) {
                clear_armed = 0;
                app.bridge.command("queue/clear");
            } else clear_armed = now_ms();
        } else if (action == "select" && key == "offline") {
            if (const Track* t = s.current()) if (!on_console) offline::save_async(app.bridge, *t);
        } else if (action == "select" && key == "pair") {
            app.pair_requested = true;
            kabuki_show(&k, "NZKNOW");
        } else if (action == "select" && key == "toasts") { app.player.song_toasts = !app.player.song_toasts; app.save(); }
        else if (action == "select" && key == "quality") { app.player.low_quality = !app.player.low_quality; app.save(); }
        else if (action == "select" && key == "mode") {
            app.config.ownership = (app.config.ownership + 1) % 4;
            app.save();
            if (app.on_ownership) app.on_ownership(app.ownership_ctx, app.config.ownership);
        } else if (action == "select" && key == "forget") app.bridge.forget();
        else if (action == "select" && key == "quit") app.quit = true;
        else if (action == "select" && key.find('|') != std::string::npos) { // a queue row
            const auto bar = key.find('|');
            const int index = std::atoi(key.substr(0, bar).c_str());
            if (on_console) {
                if (index >= 0 && index < int(s.queue.size())) app.local.play(s.queue, index);
                kabuki_show(&k, "NZKNOW");
            } else if (queue_mode == 0) {
                busy_key = key;
                busy_at = now_ms();
                push_queue(k, s);
                const int status = app.bridge.command("queue/jump", "{\"index\":" + std::to_string(index) + ",\"trackId\":" + quote(key.substr(bar + 1)) + "}");
                busy_key.clear(); // 409: the queue changed under us; the next push shows the new one
                if (status == 200) kabuki_show(&k, "NZKNOW");
            } else if (queue_mode == 1) {
                app.bridge.command("queue/remove", "{\"index\":" + std::to_string(index) + ",\"trackId\":" + quote(key.substr(bar + 1)) + "}");
            } else { // move by one, within the queue
                const int to = index + (queue_mode == 2 ? -1 : 1);
                if (to >= 0 && to < int(s.queue.size()))
                    app.bridge.command("queue/move", "{\"fromIndex\":" + std::to_string(index) + ",\"toIndex\":" + std::to_string(to) +
                                                         ",\"trackId\":" + quote(key.substr(bar + 1)) + "}");
            }
        }
        push_queue(k, current());
        push_all(k, true);
    } else if (tile == "NZKLIB") {
        if (action == "open") {
            lib_key = key;
            lib_songs.clear();
            lib_fetching = true;
            push_library(k);
            if (!library::songs(app.bridge, key, lib_songs)) logf("UI library %s: no answer", key.c_str());
            lib_fetching = false;
        } else if (action == "close") lib_key.clear();
        else if (action == "select" && key == "back") { // out of an empty list: a new session closes it
            lib_key.clear();
            ++lib_back;
        }
        else if (action == "select" && (key == "flag:save" || key == "flag:pin")) {
            if (library::Entry* e = entry(lib_key)) library::set_flag(app.bridge, *e, key == "flag:pin", key == "flag:pin" ? !e->pinned : !e->saved);
        } else if (action == "select" && key == "view") {
            lib_view = (lib_view + 1) % 5;
            if (lib_view == 4 && all_songs.empty()) {
                push_library(k, true);
                library::songs(app.bridge, "songs", all_songs);
            }
        } else if (action == "select" && key == "addmode") add_mode = (add_mode + 1) % 3;
        else if (action == "select" && lib_key.empty() && lib_view == 4) phone_song(k, "songs", all_songs, std::atoi(key.c_str())); // the Songs view
        else if (action == "select" && !lib_key.empty() && key == "shuffle") {
            app.local.stop(); // the phone plays again
            if (library::play(app.bridge, lib_key, lib_songs, 0, true) == 200) kabuki_show(&k, "NZKNOW");
        } else if (action == "select" && !lib_key.empty()) phone_song(k, lib_key, lib_songs, std::atoi(key.c_str()));
        push_library(k);
    } else if (tile == "NZKLOC") {
        if (action == "open" && key == "queue") { app.local.prune(); queue_open = true; album = loc_artist = -1; }
        else if (action == "open" && key.rfind("r:", 0) == 0) { // an artist: all their songs, album by album
            queue_open = false;
            album = -1;
            loc_artist = std::atoi(key.c_str() + 2);
            artist_songs.clear();
            if (loc_artist >= 0 && loc_artist < int(loc_artists.size()))
                for (const auto& a : albums)
                    if (a.artist == loc_artists[std::size_t(loc_artist)]) for (auto& t : local::songs(a.dir)) artist_songs.push_back(std::move(t));
        } else if (action == "open") {
            queue_open = false;
            loc_artist = -1;
            album = std::atoi(key.c_str());
            album_songs = album >= 0 && album < int(albums.size()) ? local::songs(albums[std::size_t(album)].dir) : std::vector<Track>{};
        } else if (action == "close") { album = loc_artist = -1; queue_open = false; }
        else if (action == "select" && key == "back") {
            album = loc_artist = -1;
            queue_open = false;
            ++loc_back;
            refresh_local();
        } else if (action == "select" && key == "addmode") add_mode = (add_mode + 1) % 3;
        else if (action == "select" && key == "view") {
            loc_view = (loc_view + 1) % 3;
            refresh_local();
        } else if (action == "select" && queue_open) {
            app.local.jump(std::atoi(key.c_str()));
            kabuki_show(&k, "NZKNOW");
        } else if (action == "select" && key.rfind("s:", 0) == 0) local_song(k, loc_all, std::atoi(key.c_str() + 2)); // the songs view
        else if (action == "select" && album >= 0) local_song(k, album_songs, std::atoi(key.c_str()));
        else if (action == "select" && loc_artist >= 0) local_song(k, artist_songs, std::atoi(key.c_str()));
        push_local(k);
    }
}

void KabukiUi::run() {
    mkdir(paths::art.c_str(), 0777);
    mkdir(local::root.c_str(), 0777);
    while (!app.quit) {
        kabuki_client k;
        const std::string dirs = "[" + quote(paths::art) + "," + quote(local::root) + "]";
        const kabuki_tile tiles[] = {
            {"NZKNOW", TR("Now Playing", "Sonando"), "icon:iconid_music", "media", 120},
            {"NZKQUE", "N-Zik", "icon:iconid_list", "list", 121},
            {"NZKLIB", TR("Explore", "Explorar"), "icon:iconid_app_library_media", "list", 122},
            {"NZKLOC", TR("Music", "Musica"), "icon:iconid_music", "list", 123},
        };
        if (kabuki_connect(&k) || kabuki_hello(&k, "nzik", tiles, 4, dirs.c_str(), 3000)) {
            kabuki_close(&k);
            sleep(2); // Kabuki isn't there (yet)
            continue;
        }
        logf("UI connected to Kabuki");
        session = std::to_string(now_ms());
        kabuki_status(&k, "N-Zik");
        seen = Seen{};
        settings_open = false;
        lib_key.clear();
        album = -1;
        queue_open = false;
        push_all(k, true);
        push_local(k);
        push_library(k);
        while (!app.quit) {
            kabuki_event ev;
            const int got = kabuki_poll(&k, &ev, 100);
            if (got < 0) break;
            if (got == 1 && ev.kind == KABUKI_ACTION) {
                if (std::strcmp(ev.tile, "NZKNOW")) logf("UI %s %s %s", ev.tile, ev.action, ev.key);
                on_action(k, ev);
            } else if (got == 1 && ev.kind == KABUKI_SHOWN) on_shown(k, ev.tile);
            else if (got == 1 && ev.kind == KABUKI_ERROR) logf("UI kabuki error %s: %s (%s)", ev.code, ev.message, ev.tile);
            else if (got == 0 && !covers_todo.empty()) fetch_cover(k); // idle: one library cover
            push_all(k, false);
        }
        kabuki_close(&k);
        logf("UI lost Kabuki");
    }
}
