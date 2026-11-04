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
#include <map>
#include <vector>

class KabukiUi {
public:
    explicit KabukiUi(App& app) : app(app) {}
    void run(); // until app.quit
private:
    App& app;
    struct Seen { std::uint64_t state = ~0ull, pair = ~0ull, second = 0, local = ~0ull; std::string art_id, status, extras; } seen;
    std::string art_path, busy_key;
    std::map<std::string, bool> liked; // our own likes, until the phone's queue says so
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
    // The top level's view, cycled by its first row: 0 everything, 1 playlists, 2 albums, 3 artists,
    // 4 all songs (playable right there, from all_songs).
    int lib_view = 0;
    std::vector<Track> all_songs;
    // Musica: albums on the console, and the opened one.
    std::vector<local::Album> albums;
    int album = -1;
    bool queue_open = false; // the console's queue, opened from Music
    // What Cross does on a song (cycled by a row above the songs): 0 play from it, 1 play next, 2 add at the end.
    int add_mode = 0;
    std::string mode_row();
    void phone_song(kabuki_client& k, const std::string& list, const std::vector<Track>& songs, int index);
    void local_song(kabuki_client& k, const std::vector<Track>& songs, int index);
    std::vector<Track> album_songs;

    PlayerState current(); // the local queue while it plays, else the phone
    void push_all(kabuki_client& k, bool force);
    void push_now(kabuki_client& k, const PlayerState& s);
    void push_queue(kabuki_client& k, const PlayerState& s);
    void push_library(kabuki_client& k, bool loading = false);
    void push_local(kabuki_client& k);
    std::string settings_rows(const PlayerState& s);
    std::string offline_row(const PlayerState& s);
    bool is_liked(const Track& t);
    void toggle_like(const Track& t);
    const char* offline_title(const Track& t);
    void on_action(kabuki_client& k, const kabuki_event& ev);
    void on_shown(kabuki_client& k, const std::string& tile);
    void fetch_art(const PlayerState& s);
};
