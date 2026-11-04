// SPDX-License-Identifier: GPL-3.0-or-later
// The phone's library over the bridge (contract §10): what the Explore tile lists and plays.
// Keys: "rec" (recommendations), "liked", "top", "songs" (all), "p:<id>" playlist, "a:<id>" album, "r:<id>" artist.
#pragma once
#include "bridge.hpp"
#include <string>
#include <vector>

namespace library {
struct Entry {
    std::string key, title, subtitle;
    bool opens = true;
    bool saved = false;  // album / playlist bookmarked, artist followed
    bool pinned = false; // playlist pinned
    bool has_art = false;
    std::string art_track; // a playlist's cover comes from this track when it has no custom one
};
// Saves / follows (`pin` false) or pins (`pin` true, playlists) an entry, or undoes it. Updates `e`.
// Returns the HTTP status.
int set_flag(Bridge& b, Entry& e, bool pin, bool on);
// An entry's cover (JPEG or PNG bytes) at `size` px.
bool artwork(Bridge& b, const Entry& e, int size, std::string& bytes);
// The top level: the fixed lists, then playlists, albums and artists (first page of each).
// false when the phone didn't answer.
bool top(Bridge& b, std::vector<Entry>& out);
// The songs of one entry (first 200).
bool songs(Bridge& b, const std::string& key, std::vector<Track>& out);
// Plays `list` (the songs of `key`) from `index`, or shuffled. Returns the HTTP status.
int play(Bridge& b, const std::string& key, const std::vector<Track>& list, int index, bool shuffle);
// The title of the album in the phone's library that holds `t` (among the first artist's albums),
// or "" when none does. The bridge gives no album with a track, hence the search.
std::string album_of(Bridge& b, const Track& t);
// Puts one song in the phone's queue: next, or at the end. Returns the HTTP status.
int add(Bridge& b, const Track& t, bool next);
}
