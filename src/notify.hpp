// SPDX-License-Identifier: GPL-3.0-or-later
// A console toast (stderr on the host build).
#pragma once
#include "log.hpp"
#include <cstdarg>
#include <cstdio>

#ifdef __PROSPERO__
struct NotifyRequest {
    int type, req_id, priority, msg_id, target_id, user_id, unk1, unk2, app_id, error_num, unk3;
    unsigned char use_icon_image_uri;
    char message[1024], icon_uri[1024], unk[1024];
};
extern "C" int sceKernelSendNotificationRequest(int, NotifyRequest*, std::size_t, int);
#endif

inline void notify(const char* format, ...) {
    char text[1024];
    va_list a;
    va_start(a, format);
    std::vsnprintf(text, sizeof text, format, a);
    va_end(a);
    logf("NOTIFY %s", text);
#ifdef __PROSPERO__
    NotifyRequest r{};
    std::snprintf(r.message, sizeof r.message, "%s", text);
    sceKernelSendNotificationRequest(0, &r, sizeof r, 0);
#endif
}
