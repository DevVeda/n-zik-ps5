// SPDX-License-Identifier: GPL-3.0-or-later
// Music on the console (/data/music/<artist>/<album>/<song>): a queue we own, kept in the same
// PlayerState shape as the phone's, so the Player and the Now Playing card follow either one.
// Track ids are file paths.
#pragma once
#include "bridge.hpp"
#include <mutex>
#include <string>
#include <vector>

namespace local {
// /user/data/music: the same folder as /data/music, under the name ShellUI (Kabuki's pictures) sees.
extern std::string root;
bool is_audio(const std::string& name);

struct Album {
    std::string dir, artist, title;
};
std::vector<Album> albums();                         // every folder under root that holds songs, sorted
std::vector<Track> songs(const std::string& dir);    // the songs of one folder, sorted by file name
std::string cover(const std::string& dir);           // cover.jpg / folder.jpg / cover.png, or ""

class Queue {
public:
    bool active();
    PlayerState snapshot();
    std::uint64_t version();
    void play(std::vector<Track> list, int index); // starts local playback
    void add(const Track& t, bool next);           // after the current song, or at the end; starts if nothing plays
    void jump(int index);                          // to a song of the queue
    void prune();                                  // drops songs whose files are gone (deleted by FTP)
    void stop();                                   // back to following the phone
    void toggle();
    void next();
    void previous();
    void seek(long long ms);
    void ended(); // the player reached the end of the song
    void duration(const std::string& id, long long ms); // learned when the song opens

private:
    std::mutex m;
    PlayerState s;
    bool on = false;
    void go(int index); // with m held
    void start(std::vector<Track> list, int index); // with m held
};
}
