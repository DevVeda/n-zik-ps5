// SPDX-License-Identifier: GPL-3.0-or-later
// The overlay UI: four Kabuki tiles (L3+R3 over any game).
//   NZKNOW  Sonando (media card): cover, title, seek, play/pause, skip, volume, shuffle, repeat;
//           the pairing QR while pairing.
//   NZKQUE  N-Zik (list): like / save offline for the song, Ajustes (sublist), then the queue.
//   NZKLIB  Explorar (list): the phone's library; a list opens its songs.
//   NZKLOC  Musica (list): songs on the console (/data/music); an album opens its songs.
#pragma once
#include "../app.hpp"
#include "../library.hpp"
#include "../local.hpp"
#include "kabuki.h"
#include <utility>
#include <vector>

class KabukiUi {
public:
    explicit KabukiUi(App& app) : app(app) {}
    void run(); // until app.quit
private:
    App& app;
    struct Seen { std::uint64_t state = ~0ull, pair = ~0ull, second = 0, local = ~0ull; std::string art_id, status, extras; } seen;
    std::string art_path, busy_key;
    std::vector<std::pair<std::string, std::string>> like_state; // our own liked/neutral/disliked, until the phone's queue says so
    // What Cross does on a song of the phone's queue: 0 jump to it, 1 remove it, 2 move it up, 3 move it down.
    int queue_mode = 0;
    std::uint64_t clear_armed = 0; // "clear the queue" was pressed once, at this time: a second press clears
    std::uint64_t busy_at = 0;
    bool settings_open = false;
    // Kabuki remembers an opened sublist across our reconnects; a new session per connection (and
    // per "back" in Explore) makes it start over instead of reopening a list we no longer hold.
    std::string session;
    int lib_back = 0, loc_back = 0;
    bool lib_fetching = false;
    // Explorar: the top level once loaded, and the opened list.
    std::vector<library::Entry> lib_top;
    bool lib_loaded = false;
    std::string lib_key;
    std::vector<Track> lib_songs;
    // Covers of the library's playlists, albums and artists: fetched one at a time between events,
    // kept in the art folder (lib-<key>.jpg/png) for next time.
    std::vector<std::string> covers_todo;
    std::string lib_cover(const std::string& key);
    void fetch_cover(kabuki_client& k);
    library::Entry* entry(const std::string& key);
    // The top level's view, cycled by its first row: 0 everything, 1 playlists, 2 albums, 3 artists,
    // 4 all songs (playable right there, from all_songs).
    int lib_view = 0;
    std::vector<Track> all_songs;
    // Musica: albums on the console, and the opened one.
    std::vector<local::Album> albums;
    int album = -1;
    bool queue_open = false; // the console's queue, opened from Music
    // Music's view, cycled by its first row: 0 albums, 1 artists (one opens all their songs), 2 all songs.
    int loc_view = 0, loc_artist = -1;
    std::vector<std::string> loc_artists;
    std::vector<Track> artist_songs, loc_all;
    void refresh_local(); // albums, artists and (in the songs view) all songs, from the disk
    // What Cross does on a song (cycled by a row above the songs): 0 play from it, 1 play next, 2 add at the end.
    int add_mode = 0;
    struct Row mode_row();
    void phone_song(kabuki_client& k, const std::string& list, const std::vector<Track>& songs, int index);
    void local_song(kabuki_client& k, const std::vector<Track>& songs, int index);
    std::vector<Track> album_songs;

    PlayerState current(); // the local queue while it plays, else the phone
    void push_all(kabuki_client& k, bool force);
    void push_now(kabuki_client& k, const PlayerState& s);
    void push_queue(kabuki_client& k, const PlayerState& s);
    void push_library(kabuki_client& k, bool loading = false);
    void push_local(kabuki_client& k);
    std::vector<struct Row> settings_rows();
    std::string offline_row(const PlayerState& s);
    std::string like_of(const Track& t);
    bool is_liked(const Track& t) { return like_of(t) == "liked"; }
    void toggle_like(const Track& t, const char* state); // "liked" or "disliked", back to "neutral" when already so
    const char* offline_title(const Track& t);
    void on_action(kabuki_client& k, const kabuki_event& ev);
    void on_shown(kabuki_client& k, const std::string& tile);
    void fetch_art(const PlayerState& s);
};
