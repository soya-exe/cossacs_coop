/*
 * Host-side unit test of the nick sanitizer (src/hook/nick.c). Run: tools/test_nick.sh
 * Prints one line per case and PASS / FAIL last; exit code 0 / 1.
 */
#include "../hook/nick.h"
#include <stdio.h>
#include <string.h>

static int nfail;

static void expect(const char *in, const char *want)
{
    char out[32];
    size_t n = btw_nick_sanitize(in, out, sizeof out);
    int ok = strcmp(out, want) == 0 && n == strlen(out) && n <= BTW_NICK_MAX && out[0] != '[' &&
             out[0] != ' ' && (n == 0 || out[n - 1] != ' ');
    for (size_t i = 0; i < n; i++)
        ok &= out[i] >= 0x20 && out[i] <= 0x7e;
    printf("%s  \"%s\" -> \"%s\"%s%s%s\n", ok ? "ok  " : "FAIL", in, out, ok ? "" : " (want \"",
           ok ? "" : want, ok ? "" : "\")");
    nfail += !ok;
}

int main(void)
{
    /* Russian */
    expect("Иван Петрович", "Ivan Petrovich");
    expect("Щукин Жора", "Shchukin Zhora");
    expect("ЖУК", "ZHUK");
    expect("Ёжик в тумане", "Yozhik v tumane");
    expect("Объявление", "Obyavlenie");
    expect("Хрущёв", "Khrushchyov");
    /* Ukrainian: і ї є ґ, and then Г -> H, И -> Y */
    expect("Їжак і Єнот", "Yizhak i Yenot");
    expect("Ґанок", "Ganok");
    expect("Микола Гнатюк і", "Mykola Hnatyuk i");
    expect("Микола", "Mikola"); /* no Ukrainian-only letter: Russian rules */
    /* Latin with diacritics */
    expect("[TAG] Ölaf", "TAG Olaf");
    expect("Łukasz Żółć", "Lukasz Zolc");
    expect("Straße Æon", "Strasse Aeon");
    /* emoji and other scripts are dropped, spaces collapse */
    expect("😀 Pro 🎮 Gamer 😀", "Pro Gamer");
    expect("李小龙", "");
    expect("🔥🔥🔥", "");
    expect("  lots   of\tspace  ", "lots of space");
    /* never a leading '[' */
    expect("[[x]]y", "xy");
    expect("[", "");
    expect("[]", "");
    expect("  [RU] Bob", "RU Bob");
    expect("Bob [RU]", "Bob [RU]");
    expect("a]b", "a]b");
    /* ASCII punctuation stays */
    expect("x_X-1.2 (ok)!", "x_X-1.2 (ok)!");
    /* over-long input: 31 chars max, no cut inside a transliteration, no trailing space */
    expect("abcdefghijklmnopqrstuvwxyz0123456789", "abcdefghijklmnopqrstuvwxyz01234");
    expect("abcdefghijklmnopqrstuvwxyz01234 trailing", "abcdefghijklmnopqrstuvwxyz01234");
    expect("abcdefghijklmnopqrstuvwxyz0123 x", "abcdefghijklmnopqrstuvwxyz0123");
    expect("abcdefghijklmnopqrstuvwxyz0123Щ", "abcdefghijklmnopqrstuvwxyz0123");
    expect("ЩЩЩЩЩЩЩЩЩЩ", "SHCHSHCHSHCHSHCHSHCHSHCHSHCH"); /* 7 x 4: the 8th does not fit */
    /* malformed UTF-8 and control characters */
    expect("a\xff\xfe" "b\x01" "c\xc3", "abc");
    expect("\xe2\x82", "");
    expect("", "");

    /* small output buffer */
    char small[6];
    size_t n = btw_nick_sanitize("Иван Петрович", small, sizeof small);
    int ok = n == 4 && strcmp(small, "Ivan") == 0;
    printf("%s  cap 6: \"%s\"\n", ok ? "ok  " : "FAIL", small);
    nfail += !ok;
    n = btw_nick_sanitize(NULL, small, sizeof small);
    ok = n == 0 && small[0] == 0;
    printf("%s  NULL input\n", ok ? "ok  " : "FAIL");
    nfail += !ok;

    printf("%s\n", nfail ? "FAIL" : "PASS");
    return nfail ? 1 : 0;
}
