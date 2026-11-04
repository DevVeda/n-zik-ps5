// SPDX-License-Identifier: GPL-3.0-or-later
// User settings, kept as "key value" lines in /data/n-zik-ps5/config.txt.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

struct Config {
    int volume = 100;            // 0..100
    bool low_quality = false;    // ask the phone for its "low" stream (less Wi-Fi)
    int ownership = 0;           // shell::Ownership; 0 = not chosen yet (use the default for this firmware)
    bool song_toasts = true;     // a console notification when a song starts
    std::string device_name = "PS5";

    bool load(const char* file) {
        FILE* f = std::fopen(file, "r");
        if (!f) return false;
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            line[std::strcspn(line, "\r\n")] = 0;
            char* v = std::strchr(line, ' ');
            if (!v) continue;
            *v++ = 0;
            if (!std::strcmp(line, "volume")) volume = std::atoi(v) < 0 ? 0 : std::atoi(v) > 100 ? 100 : std::atoi(v);
            else if (!std::strcmp(line, "low_quality")) low_quality = std::atoi(v) != 0;
            else if (!std::strcmp(line, "ownership")) ownership = std::atoi(v) >= 0 && std::atoi(v) <= 3 ? std::atoi(v) : 0;
            else if (!std::strcmp(line, "song_toasts")) song_toasts = std::atoi(v) != 0;
            else if (!std::strcmp(line, "device_name") && *v) device_name = std::string(v).substr(0, 64);
        }
        std::fclose(f);
        return true;
    }
    bool save(const char* file) const {
        const std::string tmp = std::string(file) + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return false;
        std::fprintf(f, "volume %d\nlow_quality %d\nownership %d\nsong_toasts %d\ndevice_name %s\n", volume, low_quality ? 1 : 0, ownership,
                     song_toasts ? 1 : 0, device_name.c_str());
        return std::fclose(f) == 0 && std::rename(tmp.c_str(), file) == 0;
    }
};
