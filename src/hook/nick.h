/*
 * Player name sanitizer (nick.c). Plain C, no Windows headers, so the host-side unit test
 * (src/test/test_nick.c, tools/test_nick.sh) builds it with the system gcc.
 */
#ifndef BTW_NICK_H
#define BTW_NICK_H

#include <stddef.h>

/* The game's nick field is 32 bytes: at most 31 characters + NUL (re/FINDINGS.md). */
#define BTW_NICK_MAX 31

/*
 * Turn a UTF-8 name (a Steam persona name, BTWSTEAM_NICK) into something the game's
 * bitmap fonts and its 32-byte nick field can take:
 *   - Cyrillic (Russian, Ukrainian, Belarusian letters) is transliterated to Latin,
 *     Latin letters with diacritics (Latin-1, Latin Extended-A) lose the diacritic;
 *   - other printable ASCII is kept, everything else (emoji, CJK, controls) is dropped;
 *   - runs of spaces become one space, no leading or trailing space;
 *   - never starts with '[' (that would trigger the game's clan HTTP check): leading '['
 *     are removed together with the first matching ']' each, so "[TAG] Name" -> "TAG Name";
 *   - at most min(cap - 1, BTW_NICK_MAX) characters; a transliteration is never cut in half.
 * Returns the length of out (0 = nothing usable, the caller picks a fallback).
 */
size_t btw_nick_sanitize(const char *utf8, char *out, size_t cap);

#endif
