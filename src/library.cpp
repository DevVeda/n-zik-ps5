// SPDX-License-Identifier: GPL-3.0-or-later
#include "i18n.hpp"
#include "library.hpp"
#include "json.hpp"
#include <cctype>

namespace library {
// ponytail: first page only (the bridge caps a page at 200); paging when a library outgrows it.
static const char* page = "offset=0&limit=200";

static std::string count(int n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// One collection route: items with `id` and a name field.
static void collection(Bridge& b, const char* route, const char* prefix, const char* name, const char* kind, std::vector<Entry>& out) {
    std::string body;
    json::Doc d;
    if (b.get(std::string("library/") + route + "?" + page, body) != 200 || !d.parse(body)) return;
    for (int i : d.items(d.get(0, "items"))) {
        const std::string id = d.str(d.get(i, "id"));
        if (!safe_id(id)) continue;
        std::string sub = kind;
        const std::string artists = d.str(d.get(i, "artists"));
        if (!artists.empty()) sub += "  ·  " + artists;
        sub += "  ·  " + count(int(d.num(d.get(i, "trackCount"))), TR("song", "cancion"), TR("songs", "canciones"));
        Entry e{prefix + id, d.str(d.get(i, name)), sub};
        e.saved = d.boolean(d.get(i, "isBookmarked"));
        e.pinned = d.boolean(d.get(i, "isPinned"));
        e.has_art = d.boolean(d.get(i, "hasArtwork"));
        e.art_track = d.str(d.get(i, "artworkTrackId"));
        if (!safe_id(e.art_track)) e.art_track.clear();
        if (prefix[0] == 'p') e.has_art = true; // a custom cover, or its track's (tried in that order)
        out.push_back(std::move(e));
    }
}

bool top(Bridge& b, std::vector<Entry>& out) {
    out.clear();
    std::string body;
    if (b.get("library/songs?filter=liked&limit=1", body) != 200) return false; // also: is the phone there?
    out.push_back({"rec", TR("Recommended", "Recomendadas"), TR("From what you listen to", "Segun lo que escuchas")});
    out.push_back({"liked", TR("Liked songs", "Canciones que te gustan"), ""});
    out.push_back({"top", TR("Most played", "Lo mas escuchado"), ""});
    collection(b, "playlists", "p:", "name", TR("Playlist", "Playlist"), out);
    collection(b, "albums", "a:", "title", TR("Album", "Album"), out);
    collection(b, "artists", "r:", "name", TR("Artist", "Artista"), out);
    return true;
}

// The list reference the phone resolves on its side (contract §10.4), as JSON.
static std::string ref(const std::string& key) {
    if (key == "songs") return "{\"kind\":\"songs\",\"filter\":\"all\"}";
    if (key == "liked" || key == "top") return "{\"kind\":\"songs\",\"filter\":\"" + key + "\"}";
    const char* kind = key[0] == 'p' ? "playlist" : key[0] == 'a' ? "album" : "artist";
    return std::string("{\"kind\":\"") + kind + "\",\"id\":" + json::quote(key.substr(2)) + "}";
}

static bool valid(const std::string& key) {
    if (key == "rec" || key == "liked" || key == "top" || key == "songs") return true;
    return key.size() > 2 && key[1] == ':' && (key[0] == 'p' || key[0] == 'a' || key[0] == 'r') && safe_id(key.substr(2));
}

bool songs(Bridge& b, const std::string& key, std::vector<Track>& out) {
    out.clear();
    if (!valid(key)) return false;
    std::string path, body;
    if (key == "rec") path = "library/songs/recommendations";
    else if (key == "liked" || key == "top") path = "library/songs?filter=" + key + "&" + page;
    else if (key == "songs") path = std::string("library/songs?filter=all&") + page;
    else path = std::string("library/") + (key[0] == 'p' ? "playlists/" : key[0] == 'a' ? "albums/" : "artists/") + key.substr(2) + "/songs?" + page;
    json::Doc d;
    if (b.get(path, body) != 200 || !d.parse(body)) return false;
    if (key != "rec") {
        read_tracks(d, d.get(0, "items"), out);
        return true;
    }
    for (int i : d.items(d.get(0, "items"))) { // recommendations wrap each track: {position, track}
        Track t = read_track(d, d.get(i, "track"));
        if (!t.id.empty()) out.push_back(std::move(t));
    }
    return true;
}

static std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

std::string album_of(Bridge& b, const Track& t) {
    const std::string artist = lower(t.artists.substr(0, t.artists.find(',')));
    std::string body;
    json::Doc d;
    if (artist.empty() || b.get(std::string("library/albums?") + page, body) != 200 || !d.parse(body)) return "";
    int looked = 0;
    for (int i : d.items(d.get(0, "items"))) {
        const std::string id = d.str(d.get(i, "id"));
        if (!safe_id(id) || lower(d.str(d.get(i, "artists"))).find(artist) == std::string::npos) continue;
        if (++looked > 20) break; // ponytail: an artist with more albums than this in the library falls back to Singles
        std::string songs;
        json::Doc s;
        if (b.get("library/albums/" + id + "/songs?" + page, songs) != 200 || !s.parse(songs)) continue;
        for (int j : s.items(s.get(0, "items")))
            if (s.str(s.get(j, "id")) == t.id) return d.str(d.get(i, "title"));
    }
    return "";
}

int set_flag(Bridge& b, Entry& e, bool pin, bool on) {
    if (!valid(e.key) || e.key.size() < 3) return 0;
    const std::string id = e.key.substr(2), flag = on ? "true" : "false";
    std::string route, body;
    if (e.key[0] == 'a') { route = "library/albums/" + id + "/bookmark"; body = "{\"bookmarked\":" + flag + "}"; }
    else if (e.key[0] == 'r') { route = "library/artists/" + id + "/follow"; body = std::string("{\"state\":\"") + (on ? "followed" : "neutral") + "\"}"; }
    else if (pin) { route = "library/playlists/" + id + "/pin"; body = "{\"pinned\":" + flag + "}"; }
    else { route = "library/playlists/" + id + "/bookmark"; body = "{\"bookmarked\":" + flag + "}"; }
    const int status = b.command(route.c_str(), body);
    if (status == 200) (pin ? e.pinned : e.saved) = on;
    return status;
}

bool artwork(Bridge& b, const Entry& e, int size, std::string& bytes) {
    if (!valid(e.key) || e.key.size() < 3 || !e.has_art) return false;
    const std::string id = e.key.substr(2), sz = "?size=" + std::to_string(size);
    const char* kind = e.key[0] == 'a' ? "albums/" : e.key[0] == 'r' ? "artists/" : "playlists/";
    if (b.get(std::string("library/") + kind + id + "/artwork" + (e.key[0] == 'p' ? "" : sz), bytes) == 200) return true;
    return e.key[0] == 'p' && !e.art_track.empty() && b.get("artwork/" + e.art_track + sz, bytes) == 200;
}

int add(Bridge& b, const Track& t, bool next) {
    if (!safe_id(t.id)) return 0;
    return b.command("queue/add", "{\"trackIds\":[" + json::quote(t.id) + "],\"position\":\"" + (next ? "next" : "end") + "\"}");
}

int play(Bridge& b, const std::string& key, const std::vector<Track>& list, int index, bool shuffle) {
    if (!valid(key) || list.empty()) return 0;
    if (index < 0 || index >= int(list.size())) index = 0;
    if (key == "rec") { // not a list the phone can resolve: hand it the ids
        std::string ids;
        for (const auto& t : list) ids += (ids.empty() ? "" : ",") + json::quote(t.id);
        return b.command("queue/play", "{\"trackIds\":[" + ids + "],\"startIndex\":" + std::to_string(shuffle ? 0 : index) + ",\"positionMs\":0}");
    }
    std::string body = "{\"list\":" + ref(key) + ",\"action\":\"" + (shuffle ? "shuffle" : "play") + "\"";
    if (!shuffle) body += ",\"startIndex\":" + std::to_string(index) + ",\"startTrackId\":" + json::quote(list[std::size_t(index)].id);
    return b.command("queue/list", body + "}");
}
}
