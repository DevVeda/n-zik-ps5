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
static std::string song_rows(const std::vector<Track>& songs, std::size_t max = 300) {
    std::string d;
    for (std::size_t i = 0; i < songs.size() && i < max; ++i) {
        std::string sub = songs[i].artists;
        if (songs[i].duration_ms > 0) sub += (sub.empty() ? "" : "  ·  ") + duration(songs[i].duration_ms);
        d += (d.empty() ? "" : ",") + std::string("{\"key\":") + quote(std::to_string(i)) + ",\"title\":" + quote(songs[i].title) +
             ",\"subtitle\":" + quote(sub) + "}";
    }
    return d;
}


// UI rule: every sublist goes back with Circle. Kabuki keeps the cursor in an opened list only
// while it loads or has rows (native_model.hpp, enterable); an empty one loses it and Circle then
// closes the whole panel. So an opened list is never empty: this row stands in, and selecting it
// starts a new session, which closes the list.
static std::string back_row(const char* why_en, const char* why_es) {
    return "{\"key\":\"back\",\"title\":" + quote(TR("← Back", "← Volver")) + ",\"subtitle\":" + quote(TR(why_en, why_es)) + "}";
}

std::string KabukiUi::mode_row() {
    static const char* const names[][2] = {{"Play", "Reproducir"}, {"Play next", "Reproducir siguiente"}, {"Add to the end", "Agregar al final"}};
    return "{\"key\":\"addmode\",\"title\":" + quote(std::string("✕: ") + TR(names[add_mode][0], names[add_mode][1])) +
           ",\"subtitle\":" + quote(TR("Cross here changes what Cross does on a song", "Cross aqui cambia lo que hace Cross en una cancion")) + "}";
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
             quote(TR("Phone: N-Zik > PC bridge > Scan QR", "Telefono: N-Zik > Puente PC > Escanear QR"));
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
    else if (s.connected && s.output != "pc") note = "Pasando el audio al PS5...";
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

bool KabukiUi::is_liked(const Track& t) {
    const auto own = liked.find(t.id);
    return own != liked.end() ? own->second : t.liked;
}
void KabukiUi::toggle_like(const Track& t) {
    const bool on = !is_liked(t);
    if (app.bridge.command(("library/songs/" + t.id + "/like").c_str(), std::string("{\"state\":\"") + (on ? "liked" : "neutral") + "\"}") == 200)
        liked[t.id] = on;
}
const char* KabukiUi::offline_title(const Track& t) {
    return offline::busy(t.id) ? TR("Saving offline...", "Guardando offline...")
         : offline::saved(t) ? TR("Saved offline", "Guardada offline") : TR("Save offline", "Guardar offline");
}

std::string KabukiUi::offline_row(const PlayerState& s) {
    const Track* t = s.current();
    if (!t || app.local.active()) return "";
    return "{\"key\":\"offline\",\"title\":" + quote(offline_title(*t)) + ",\"subtitle\":" +
           quote(TR("On the console: Music > ", "En la consola: Musica > ") + offline::clean(t->artists)) + "}";
}

std::string KabukiUi::settings_rows(const PlayerState& s) {
    const bool paired = app.bridge.paired();
    const PlayerState phone = app.bridge.snapshot();
    std::string conn = phone.connected ? TR("Connected", "Conectado") : paired ? phone.problem : TR("Not paired", "Sin emparejar");
    auto row = [](const char* key, const std::string& title, const std::string& subtitle) {
        return std::string("{\"key\":\"") + key + "\",\"title\":" + quote(title) + ",\"subtitle\":" + quote(subtitle) + "},";
    };
    std::string d = row("status", TR("Phone: ", "Telefono: ") + conn, app.player.status());
    d += row("pair", paired ? TR("Pair another phone", "Emparejar otro telefono") : TR("Pair phone", "Emparejar telefono"),
             TR("Shows a QR in Now Playing", "Muestra un QR en Sonando"));
    d += row("quality", std::string(TR("Quality: ", "Calidad: ")) + (app.player.low_quality ? TR("low", "baja") : TR("high", "alta")),
             TR("Low uses less Wi-Fi", "Baja usa menos Wi-Fi"));
    d += row("toasts", std::string(TR("Song notices: ", "Avisos de cancion: ")) + (app.player.song_toasts ? TR("on", "si") : TR("off", "no")),
             TR("A notification when each song starts", "Notificacion al empezar cada cancion"));
    d += "{\"key\":\"mode\",\"title\":" + quote(std::string(TR("Audio mode: ", "Modo de audio: ")) + ownership_name(app.config.ownership)) +
         ",\"subtitle\":" + quote(app.audio_info) + "},";
    if (paired) d += row("forget", TR("Forget this phone", "Olvidar este telefono"), "");
    d += row("quit", TR("Quit N-Zik", "Cerrar N-Zik"), TR("Stops the music and the payload", "Detiene la musica y el payload"));
    d.pop_back(); // the last comma
    (void)s;
    return d;
}

void KabukiUi::push_queue(kabuki_client& k, const PlayerState& s) {
    const bool on_console = app.local.active();
    std::string d = "{\"session\":" + quote(session) + ",\"busy\":" + (busy_key.empty() ? std::string("null") : quote(busy_key)) +
                    ",\"top\":{\"title\":\"N-Zik\",\"covers\":false";
    d += ",\"heading\":" + quote(nav(false, s.queue.empty() ? "" : std::to_string(s.queue.size()) +
                                       (on_console ? TR(" songs on the console", " canciones en la consola") : TR(" songs in the queue", " canciones en la cola"))));
    d += ",\"rows\":[";
    if (const Track* t = on_console ? nullptr : s.current()) { // the like button for the song that's playing
        const bool on = is_liked(*t);
        d += "{\"key\":\"like\",\"title\":" + quote(on ? TR("Liked  (Cross to undo)", "Te gusta  (Cross para quitar)") : TR("Like", "Me gusta")) + ",\"subtitle\":" + quote(t->title) + "},";
        d += offline_row(s) + ",";
    }
    d += "{\"key\":\"settings\",\"title\":" + quote(TR("Settings", "Ajustes")) + ",\"subtitle\":" +
         quote(TR("Phone, quality, notices, audio mode", "Telefono, calidad, avisos, modo de audio")) + ",\"opens\":true}";
    // Kabuki shows up to 500 rows (and lines up to 64 KiB): the current song and what follows matter most.
    const int max = 300;
    const int from = int(s.queue.size()) > max && s.current_index > 0 ? std::min(s.current_index, int(s.queue.size()) - max) : 0;
    for (int i = from; i < int(s.queue.size()) && i < from + max; ++i) {
        const Track& t = s.queue[std::size_t(i)];
        std::string sub = t.artists;
        if (t.duration_ms > 0) sub += (sub.empty() ? "" : "  ·  ") + duration(t.duration_ms);
        if (i == s.current_index) sub = TR("Playing  ·  ", "Sonando  ·  ") + sub;
        d += std::string(",{\"key\":") + quote(std::to_string(i) + "|" + (on_console ? std::string("local") : t.id)) + ",\"title\":" + quote(t.title) +
             ",\"subtitle\":" + quote(sub) + "}";
    }
    d += "]}";
    if (settings_open)
        d += ",\"sub_key\":\"settings\",\"sub\":{\"title\":" + quote(TR("Settings", "Ajustes")) + ",\"heading\":" + quote(nav(true)) +
             ",\"covers\":false,\"rows\":[" + settings_rows(s) + "]}";
    else d += ",\"sub_key\":null";
    d += "}";
    kabuki_content(&k, "NZKQUE", d.c_str());
}

void KabukiUi::push_library(kabuki_client& k, bool loading) {
    std::string d = "{\"session\":" + quote(session + "." + std::to_string(lib_back)) +
                    ",\"top\":{\"title\":" + quote(TR("Explore", "Explorar")) + ",\"heading\":" + quote(nav(false)) + ",\"covers\":false";
    if (loading) d += ",\"loading\":true";
    else {
        d += ",\"empty\":{\"title\":" + quote(app.bridge.paired() ? TR("The phone is not answering", "El telefono no responde") : TR("No phone", "Sin telefono")) +
             ",\"detail\":" + quote(TR("Open N-Zik on the phone and come back to this tab", "Abre N-Zik en el telefono y vuelve a esta pestana")) + "},\"rows\":[";
        if (!lib_top.empty()) {
            static const char* const names[][2] = {{"Everything", "Todo"}, {"Playlists", "Playlists"}, {"Albums", "Albumes"},
                                                   {"Artists", "Artistas"}, {"Songs", "Canciones"}};
            d += "{\"key\":\"view\",\"title\":" + quote(std::string(TR("Show: ", "Ver: ")) + TR(names[lib_view][0], names[lib_view][1])) +
                 ",\"subtitle\":" + quote(TR("Cross: everything, playlists, albums, artists, songs", "Cross: todo, playlists, albumes, artistas, canciones")) + "}";
        }
        if (lib_top.empty()) { // the phone didn't answer: just the empty notice
        } else if (lib_view == 4) {
            d += "," + mode_row();
            const std::string rows = song_rows(all_songs, 400);
            if (!rows.empty()) d += "," + rows;
        } else {
            const char prefix = " par"[lib_view]; // the entries of one kind: p: playlists, a: albums, r: artists
            for (std::size_t i = 0, shown = 0; i < lib_top.size() && shown < 480; ++i) {
                if (lib_view && !(lib_top[i].key.size() > 1 && lib_top[i].key[0] == prefix && lib_top[i].key[1] == ':')) continue;
                d += std::string(",{\"key\":") + quote(lib_top[i].key) + ",\"title\":" + quote(lib_top[i].title) +
                     ",\"subtitle\":" + quote(lib_top[i].subtitle) + ",\"opens\":true}";
                ++shown;
            }
        }
        d += "]";
    }
    d += "}";
    if (!lib_key.empty()) {
        std::string title;
        for (const auto& e : lib_top) if (e.key == lib_key) title = e.title;
        d += ",\"sub_key\":" + quote(lib_key) + ",\"sub\":{\"title\":" + quote(title) + ",\"heading\":" + quote(nav(true)) + ",\"covers\":false";
        if (lib_fetching) d += ",\"loading\":true";
        d += ",\"rows\":[";
        // Never an empty list: it can't hold the focus, and then Circle closes the panel instead.
        if (lib_songs.empty() && !lib_fetching)
            d += back_row("No songs here, or the phone did not answer", "Aqui no hay canciones, o el telefono no respondio");
        if (!lib_songs.empty()) d += mode_row() + ",";
        if (!lib_songs.empty())
            d += "{\"key\":\"shuffle\",\"title\":" + quote(TR("Shuffle", "Aleatorio")) + ",\"subtitle\":" +
                 quote(std::to_string(lib_songs.size()) + TR(" songs", " canciones")) + "},";
        d += song_rows(lib_songs);
        if (d.back() == ',') d.pop_back();
        d += "]}";
    } else d += ",\"sub_key\":null";
    d += "}";
    kabuki_content(&k, "NZKLIB", d.c_str());
}

void KabukiUi::push_local(kabuki_client& k) {
    std::string d = "{\"session\":" + quote(session + "." + std::to_string(loc_back)) +
                    ",\"top\":{\"title\":" + quote(TR("Music on the console", "Musica en la consola")) + ",\"heading\":" + quote(nav(false)) + ",\"covers\":true";
    d += ",\"empty\":{\"title\":" + quote(TR("No music in /data/music", "No hay musica en /data/music")) + ",\"detail\":" +
         quote(TR("Copy Artist/Album folders by FTP, or use Save offline in N-Zik", "Copia carpetas Artista/Album por FTP, o usa Guardar offline en N-Zik")) + "},\"rows\":[";
    const PlayerState q = app.local.snapshot();
    const bool has_queue = app.local.active() && !q.queue.empty();
    if (has_queue) {
        const Track* t = q.current();
        d += "{\"key\":\"queue\",\"title\":" + quote(TR("Console queue", "Cola de la consola")) + ",\"subtitle\":" +
             quote(std::to_string(q.queue.size()) + TR(" songs", " canciones") + (t ? "  ·  " + t->title : std::string())) + ",\"opens\":true}";
    }
    for (std::size_t i = 0; i < albums.size() && i < 480; ++i) {
        const std::string art = local::cover(albums[i].dir);
        d += (i || has_queue ? "," : "") + std::string("{\"key\":") + quote(std::to_string(i)) + ",\"title\":" + quote(albums[i].title) +
             ",\"subtitle\":" + quote(albums[i].artist) + ",\"art\":" + (art.empty() ? std::string("null") : quote(art)) + ",\"opens\":true}";
    }
    d += "]}";
    if (queue_open) {
        std::string rows;
        for (std::size_t i = 0; i < q.queue.size() && i < 400; ++i) {
            std::string sub = q.queue[i].artists;
            if (int(i) == q.current_index) sub = TR("Playing  ·  ", "Sonando  ·  ") + sub;
            rows += (rows.empty() ? "" : ",") + std::string("{\"key\":") + quote(std::to_string(i)) + ",\"title\":" + quote(q.queue[i].title) +
                    ",\"subtitle\":" + quote(sub) + "}";
        }
        d += ",\"sub_key\":\"queue\",\"sub\":{\"title\":" + quote(TR("Console queue", "Cola de la consola")) + ",\"heading\":" + quote(nav(true)) +
             ",\"covers\":false,\"rows\":[" + (rows.empty() ? back_row("Nothing is playing on the console", "No suena nada en la consola") : rows) + "]}";
    } else if (album >= 0 && album < int(albums.size())) {
        const std::string art = local::cover(albums[std::size_t(album)].dir);
        d += ",\"sub_key\":" + quote(std::to_string(album)) + ",\"sub\":{\"title\":" + quote(albums[std::size_t(album)].title) +
             ",\"art\":" + (art.empty() ? std::string("null") : quote(art)) + ",\"heading\":" + quote(nav(true)) + ",\"covers\":false,\"rows\":[" +
             (album_songs.empty() ? back_row("The songs are gone from this folder", "Las canciones ya no estan en esta carpeta")
                                  : mode_row() + "," + song_rows(album_songs)) + "]}";
    } else d += ",\"sub_key\":null";
    d += "}";
    kabuki_content(&k, "NZKLOC", d.c_str());
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
        push_library(k);
    } else if (tile == "NZKLOC") {
        app.local.prune(); // files deleted since (FTP) leave the console's queue
        albums = local::albums(); // cheap, and picks up what was copied or saved since
        if (album >= int(albums.size())) album = -1;
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
        else if (action == "select" && key == "like") {
            if (const Track* t = s.current()) toggle_like(*t);
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
            if (app.on_ownership) app.on_ownership(app.config.ownership);
        } else if (action == "select" && key == "forget") app.bridge.forget();
        else if (action == "select" && key == "quit") app.quit = true;
        else if (action == "select" && key.find('|') != std::string::npos) { // a queue row
            const auto bar = key.find('|');
            const int index = std::atoi(key.substr(0, bar).c_str());
            if (on_console) {
                if (index >= 0 && index < int(s.queue.size())) app.local.play(s.queue, index);
                kabuki_show(&k, "NZKNOW");
            } else {
                busy_key = key;
                busy_at = now_ms();
                push_queue(k, s);
                const int status = app.bridge.command("queue/jump", "{\"index\":" + std::to_string(index) + ",\"trackId\":" + quote(key.substr(bar + 1)) + "}");
                busy_key.clear(); // 409: the queue changed under us; the next push shows the new one
                if (status == 200) kabuki_show(&k, "NZKNOW");
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
        else if (action == "select" && key == "view") {
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
        if (action == "open" && key == "queue") { app.local.prune(); queue_open = true; album = -1; }
        else if (action == "open") {
            queue_open = false;
            album = std::atoi(key.c_str());
            album_songs = album >= 0 && album < int(albums.size()) ? local::songs(albums[std::size_t(album)].dir) : std::vector<Track>{};
        } else if (action == "close") { album = -1; queue_open = false; }
        else if (action == "select" && key == "back") {
            album = -1;
            queue_open = false;
            ++loc_back;
            albums = local::albums();
        } else if (action == "select" && key == "addmode") add_mode = (add_mode + 1) % 3;
        else if (action == "select" && queue_open) {
            app.local.jump(std::atoi(key.c_str()));
            kabuki_show(&k, "NZKNOW");
        } else if (action == "select" && album >= 0) local_song(k, album_songs, std::atoi(key.c_str()));
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
            push_all(k, false);
        }
        kabuki_close(&k);
        logf("UI lost Kabuki");
    }
}
