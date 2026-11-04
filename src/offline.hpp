// SPDX-License-Identifier: GPL-3.0-or-later
// "Guardar offline": copies the song the phone is streaming, whole and as served (opus/webm or
// aac/m4a, no re-encoding), to <music>/<artist>/<album>/<title>.<ext>, with its cover. The album
// comes from the phone's library (library::album_of); a song in none of its albums goes to Singles.
#pragma once
#include "bridge.hpp"
#include <string>

namespace offline {
std::string artist_folder(const Track& t);  // <music>/<first artist>
bool saved(const Track& t);                 // already there, in any of the artist's album folders
bool busy(const std::string& id);    // a download of it is under way
void save_async(Bridge& b, const Track& t); // in the background; notifies when done
std::string clean(const std::string& name); // a file name piece: no separators, no control chars, not too long
}
