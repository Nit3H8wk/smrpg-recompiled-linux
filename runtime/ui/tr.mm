// The interface in the player's language (tr.h): a table from the English text to the Spanish one, lang_es.inc.
#include "tr.h"
#include <string.h>
#include <string>
#include <string_view>
#include <unordered_map>

#include "../app_name.h"

extern "C" const char *settings_language(void);  // runtime/settings.c: the game's language ("es", "en-US"...)
extern "C" bool g_game_las;                      // runtime/game_addresses.c: Link's Awakening (its menus are English only)

static const char *const k_es[][2] = {
#include "lang_es.inc"
};

extern "C" bool ui_spanish(void) { return !g_game_las && !strncmp(settings_language(), "es", 2); }

static const char *find(std::string_view en) {
    static const std::unordered_map<std::string_view, const char *> map = [] {
        std::unordered_map<std::string_view, const char *> m;
        for (auto &p : k_es) m.emplace(p[0], p[1]);
        return m;
    }();
    auto it = map.find(en);
    return it == map.end() ? NULL : it->second;
}

extern "C" const char *tr(const char *en) {
    if (!en || !*en || !ui_spanish()) return en;
    const char *es = find(en);
    return es ? es : en;
}

extern "C" const char *trc(const char *kind, const char *en) {
    if (!en || !ui_spanish()) return en;
    std::string key = std::string(kind) + ":" + en;
    const char *es = find(key);
    return es ? es : en;
}
