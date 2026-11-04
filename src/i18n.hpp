// SPDX-License-Identifier: GPL-3.0-or-later
// Texts in the console's language: Spanish when the console is in Spanish (Spain or Latin
// America), English otherwise. TR("English", "Español") at each text.
// ponytail: two languages inline; a table per language once a third one is wanted.
#pragma once
#include <cstdlib>
#include <cstring>

#ifdef __PROSPERO__
extern "C" int sceSystemServiceParamGetInt(int id, int* value);
#endif

namespace i18n {
inline bool spanish = true;
inline int lang = -1; // the console's language code, for the log
inline void init() {
#ifdef __PROSPERO__
    // SCE_SYSTEM_SERVICE_PARAM_ID_LANG: 3 es-ES, 20 es-419
    spanish = sceSystemServiceParamGetInt(1, &lang) == 0 && (lang == 3 || lang == 20);
#else
    const char* l = std::getenv("NZIK_LANG"); // host build: Spanish unless NZIK_LANG=en
    spanish = !(l && !std::strncmp(l, "en", 2));
#endif
}
inline const char* tr(const char* en, const char* es) { return spanish ? es : en; }
}
#define TR(en, es) i18n::tr(en, es)
