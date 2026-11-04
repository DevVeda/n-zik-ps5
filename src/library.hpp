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
};
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
