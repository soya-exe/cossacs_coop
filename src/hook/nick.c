/*
 * Player name sanitizer, see nick.h.
 *
 * The nick ends up in RoomInfo+0x80 and travels in the game's join request as nick[32]
 * (re/FINDINGS.md sections 3 and 4). The game is from 2002 and draws text with its own
 * bitmap fonts, so only printable ASCII is safe. A leading '[' makes the game start its
 * (dead) clan HTTP check.
 *
 * Transliteration: Russian letters follow the common Latin spelling (Иван Петрович ->
 * Ivan Petrovich, Щ -> Shch, Ё -> Yo, Ъ/Ь dropped). If the name contains a letter that
 * only Ukrainian has (І Ї Є Ґ), Г is written H and И is written Y (Ukrainian national
 * system); otherwise G and I. Multi-letter results keep the case of their context:
 * "Жук" -> "Zhuk", "ЖУК" -> "ZHUK".
 */
#include "nick.h"
#include <stdint.h>
#include <string.h>

#define MAX_CPS 512

/* U+0430..U+044F а..я (U+0410..U+042F are the capitals) */
static const char *const k_ru[32] = {
    "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
    "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "", "y", "", "e", "yu", "ya",
};
/* U+0450..U+045F ѐ ё ђ ѓ є ѕ і ї ј љ њ ћ ќ ѝ ў џ (U+0400..U+040F capitals) */
static const char *const k_cyr_ext[16] = {
    "e", "yo", "dj", "g", "ye", "dz", "i", "yi", "j", "lj", "nj", "c", "k", "i", "u", "dz",
};
/* U+00E0..U+00FF à..ÿ (U+00C0..U+00DE capitals); NULL = not a letter (÷) */
static const char *const k_latin1[32] = {
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", NULL, "o", "u", "u", "u", "u", "y", "th", "y",
};
/* U+0100..U+017F, one base letter each, case included (IJ ij OE oe are special-cased) */
static const char k_latin_ext_a[] =
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOo"
    "RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
_Static_assert(sizeof k_latin_ext_a - 1 == 128, "Latin Extended-A table");

enum { DROP = 0, CHAR = 1, LETTER = 2 };

/* Decode one UTF-8 sequence at *p. Malformed bytes decode to 0xFFFD (dropped later). */
static uint32_t utf8_next(const unsigned char **pp)
{
    const unsigned char *p = *pp;
    uint32_t c = *p++;
    int extra = 0;
    uint32_t min = 0;
    if (c < 0x80) {
        *pp = p;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        extra = 1, c &= 0x1F, min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2, c &= 0x0F, min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3, c &= 0x07, min = 0x10000;
    } else {
        *pp = p;
        return 0xFFFD;
    }
    for (int i = 0; i < extra; i++) {
        if ((*p & 0xC0) != 0x80) {
            *pp = p; /* do not swallow the next character */
            return 0xFFFD;
        }
        c = c << 6 | (*p++ & 0x3F);
    }
    *pp = p;
    return c < min || c > 0x10FFFF ? 0xFFFD : c;
}

static int is_space(uint32_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0x3000 ||
           (c >= 0x2000 && c <= 0x200A);
}

/* Lower-case ASCII transliteration of one code point in buf, *upper for capitals.
 * Returns DROP, CHAR (punctuation, digits) or LETTER. buf may be "" for a LETTER (Ъ, Ь). */
static int translit(uint32_t c, int ukr, char buf[8], int *upper)
{
    const char *s = NULL;
    *upper = 0;
    buf[0] = 0;
    if (c > 0x20 && c < 0x7F) {
        int alpha = (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
        *upper = alpha && c <= 'Z';
        buf[0] = (char)(alpha ? (c | 0x20) : c);
        buf[1] = 0;
        return alpha ? LETTER : CHAR;
    }
    if (c >= 0x0410 && c <= 0x044F) {
        *upper = c < 0x0430;
        unsigned i = (c - 0x0410) & 31;
        s = k_ru[i];
        if (ukr && i == 3) /* Г */
            s = "h";
        if (ukr && i == 8) /* И */
            s = "y";
    } else if (c >= 0x0400 && c <= 0x045F) {
        *upper = c < 0x0450;
        s = k_cyr_ext[c & 15];
    } else if (c == 0x0490 || c == 0x0491) { /* Ґ ґ */
        *upper = c == 0x0490;
        s = "g";
    } else if (c >= 0x00C0 && c <= 0x00FF) {
        if (c == 0x00DF) /* ß */
            s = "ss";
        else {
            *upper = c < 0x00E0;
            s = k_latin1[c & 31];
        }
    } else if (c >= 0x0100 && c <= 0x017F) {
        static const char *const lig[4] = { "ij", "ij", "oe", "oe" };
        if (c == 0x0132 || c == 0x0133 || c == 0x0152 || c == 0x0153) {
            *upper = !(c & 1);
            s = lig[(c >= 0x0152) * 2 + (c & 1)];
        } else {
            char b = k_latin_ext_a[c - 0x0100];
            *upper = b >= 'A' && b <= 'Z';
            buf[0] = (char)(b | 0x20);
            buf[1] = 0;
            return LETTER;
        }
    }
    if (!s)
        return DROP;
    strcpy(buf, s);
    return LETTER;
}

size_t btw_nick_sanitize(const char *utf8, char *out, size_t cap)
{
    uint32_t cps[MAX_CPS];
    size_t ncp = 0, n = 0, max = cap ? cap - 1 : 0;
    if (max > BTW_NICK_MAX)
        max = BTW_NICK_MAX;
    if (!cap)
        return 0;
    out[0] = 0;
    if (!utf8)
        return 0;

    int ukr = 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p && ncp < MAX_CPS;) {
        uint32_t c = utf8_next(&p);
        cps[ncp++] = c;
        ukr |= c == 0x0404 || c == 0x0454 || c == 0x0406 || c == 0x0456 || c == 0x0407 ||
               c == 0x0457 || c == 0x0490 || c == 0x0491;
    }

    int pending_space = 0, brackets = 0, prev_upper = 0;
    for (size_t i = 0; i < ncp; i++) {
        char t[8], nt[8];
        int up, nup = 0;
        if (is_space(cps[i])) {
            pending_space = 1;
            continue;
        }
        int kind = translit(cps[i], ukr, t, &up);
        if (kind == DROP || !t[0])
            continue;
        if (n == 0 && t[0] == '[' && !t[1]) { /* no leading '[': clan check */
            brackets++;
            continue;
        }
        if (brackets && t[0] == ']' && !t[1]) { /* its partner: "[TAG] x" -> "TAG x" */
            brackets--;
            continue;
        }
        if (kind == LETTER && up) {
            int nkind = i + 1 < ncp ? translit(cps[i + 1], ukr, nt, &nup) : DROP;
            int all_caps = (nkind == LETTER && nup) || (prev_upper && !(nkind == LETTER && !nup));
            t[0] = (char)(t[0] - 'a' + 'A');
            for (int k = 1; all_caps && t[k]; k++)
                t[k] = (char)(t[k] - 'a' + 'A');
        }
        size_t len = strlen(t), sp = pending_space && n > 0;
        if (n + sp + len > max)
            break;
        if (sp)
            out[n++] = ' ';
        memcpy(out + n, t, len);
        n += len;
        pending_space = 0;
        prev_upper = kind == LETTER && up;
    }
    out[n] = 0;
    return n;
}
