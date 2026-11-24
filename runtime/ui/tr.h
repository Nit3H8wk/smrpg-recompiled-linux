// The interface in the player's language.
//
// The code has every text in English; lang_es.inc has the Spanish one. With the game in Spanish (Video > Game language,
// or the system's own language with "Automatic"), tr() returns the Spanish text; otherwise, the text it was given.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool ui_spanish(void);
// (format_arg: the compiler keeps checking the printf formats of the English texts)
const char *tr(const char *en) __attribute__((format_arg(1)));
// A name that depends on what it names ("Left" is an arrow key and a direction): kind is "key", "pad" or "input".
const char *trc(const char *kind, const char *en);
#ifdef __cplusplus
}
#endif
