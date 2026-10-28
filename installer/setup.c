/*
 * btwsteam-setup.exe: installs, removes and checks the btwsteam mod (Steam multiplayer for
 * Cossacks: Back to War, Steam app 4850, dmcr.exe v1.35) in the game's bin\ folder.
 *
 *   btwsteam-setup.exe [GAME_DIR] [--install | --uninstall | --status] [--force] [--no-pause]
 *                      [--lang en|ru]
 *
 * Installing means: keep the game's ichat.dll as ichat_orig.dll (once; a backup is never
 * overwritten with our DLL), copy our ichat.dll and Valve's steam_api.dll into bin\ (a
 * steam_api.dll that is not ours is kept as steam_api_orig.dll), and write the record
 * bin\btwsteam-installed.txt (version, date, files, SHA-256). installer/install.sh does the
 * same on Linux and reads/writes the same record.
 *
 * Payload: ichat.dll and steam_api.dll are embedded as RCDATA 101 / 102
 * (tools/build_installer.sh). Files with those names next to this exe win, so a release zip
 * can carry newer builds; an ichat.dll there must contain our "btwsteam-version:" marker.
 * Hidden test options: --ichat FILE, --steam-api FILE.
 *
 * Finding the game: GAME_DIR (the game folder, its bin\ or dmcr.exe), else the folder this
 * exe is in (the game folder or bin\), else Steam: HKCU\Software\Valve\Steam SteamPath and
 * HKLM ...\Valve\Steam InstallPath (both registry views), and under Wine also $HOME's Linux
 * Steam folders; every library in steamapps\libraryfolders.vdf; appmanifest_4850.acf's
 * "installdir"; then steamapps\common\<installdir>\bin\dmcr.exe.
 *
 * Exit codes: 0 ok, 1 failed, 2 bad arguments, 3 game not found, 4 wrong dmcr.exe version,
 * 5 the game is running.
 *
 * Output is UTF-8 (WriteConsoleW on a console). Russian if the Windows UI language is
 * Russian, else English; --lang or BTWSTEAM_LANG=en|ru override.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef BTWSTEAM_VERSION
#error "build with -DBTWSTEAM_VERSION=\"x.y.z\" (tools/build_installer.sh)"
#endif

#define APPID_STR       "4850"
#define DMCR_SHA256     "3923b0ff3ae8da0584f29ae2bbf2e2deb030e57df11d37fb6fb55fa5ceb4acd6"
#define ICHAT_GSC_SHA256 "8a0d5edcb6a207d25c14c3c2af5e8fbabbc2d08f9ed8f751e7bc9b242e14986a"
#define IDR_ICHAT       101
#define IDR_STEAMAPI    102
#define RECORD          L"btwsteam-installed.txt"

enum { RC_OK = 0, RC_FAIL = 1, RC_USAGE = 2, RC_NOGAME = 3, RC_VERSION = 4, RC_RUNNING = 5 };

#define PATHN 1024

static int g_ru;
static int g_wine;

/* ---- output ----------------------------------------------------------------------- */

static void out_raw(const char *s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode, w;
    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return;
    if (GetConsoleMode(h, &mode)) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
        wchar_t *ws = n > 0 ? malloc((size_t)n * sizeof *ws) : NULL;
        if (ws) {
            MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, n);
            WriteConsoleW(h, ws, (DWORD)(n - 1), &w, NULL);
            free(ws);
        }
    } else {
        WriteFile(h, s, (DWORD)strlen(s), &w, NULL);
    }
}

/* say("English %s\n", "Русский %s\n", args): one format per language, same arguments. */
static void say(const char *en, const char *ru, ...) __attribute__((format(printf, 1, 3)));
static void say(const char *en, const char *ru, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, ru);
    vsnprintf(buf, sizeof buf, g_ru ? ru : en, ap);
    va_end(ap);
    out_raw(buf);
}

/* UTF-8 copy of a wide string, from a small ring of buffers (for say()). */
static const char *u8(const wchar_t *w)
{
    static char buf[6][PATHN * 3];
    static int i;
    char *b = buf[i++ % 6];
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, b, (int)sizeof buf[0], NULL, NULL))
        b[0] = 0;
    return b;
}

static const char *errstr(DWORD e)
{
    static char buf[2][600];
    static int i;
    char *b = buf[i++ % 2];
    wchar_t w[256];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, w,
                             256, NULL);
    while (n && (w[n - 1] == L'\r' || w[n - 1] == L'\n' || w[n - 1] == L' ' || w[n - 1] == L'.'))
        w[--n] = 0;
    if (!n)
        snprintf(b, sizeof buf[0], "error %lu", (unsigned long)e);
    else
        snprintf(b, sizeof buf[0], "%s (error %lu)", u8(w), (unsigned long)e);
    return b;
}

/* ---- SHA-256 (FIPS 180-4) ----------------------------------------------------------- */

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha256_t;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_t *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_hex(const void *data, size_t len, char hex[65])
{
    static const uint32_t init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    sha256_t s;
    const uint8_t *p = data;
    memcpy(s.h, init, sizeof init);
    s.len = (uint64_t)len * 8;
    while (len >= 64) {
        sha256_block(&s, p);
        p += 64;
        len -= 64;
    }
    uint8_t tail[128];
    memset(tail, 0, sizeof tail);
    memcpy(tail, p, len);
    tail[len] = 0x80;
    size_t tl = len < 56 ? 64 : 128;
    for (int i = 0; i < 8; i++)
        tail[tl - 1 - i] = (uint8_t)(s.len >> (8 * i));
    sha256_block(&s, tail);
    if (tl == 128)
        sha256_block(&s, tail + 64);
    for (int i = 0; i < 8; i++)
        snprintf(hex + 8 * i, 9, "%08x", (unsigned)s.h[i]);
}

/* ---- files and paths ---------------------------------------------------------------- */

static void pjoin(wchar_t *out, const wchar_t *dir, const wchar_t *name)
{
    wchar_t tmp[PATHN];
    size_t n = wcslen(dir);
    if (n + wcslen(name) + 2 > PATHN) {
        out[0] = 0;
        return;
    }
    wcscpy(tmp, dir);
    if (n && tmp[n - 1] != L'\\' && tmp[n - 1] != L'/')
        wcscat(tmp, L"\\");
    wcscat(tmp, name);
    wcscpy(out, tmp);
}

static int file_exists(const wchar_t *p)
{
    DWORD a = p[0] ? GetFileAttributesW(p) : INVALID_FILE_ATTRIBUTES;
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* '/' -> '\', no trailing '\' (except "C:\"); under Wine "/unix/path" -> "Z:\unix\path". */
static void normalize(wchar_t *p)
{
    if (g_wine && p[0] == L'/' && p[1] != L'/' && wcslen(p) + 3 < PATHN) {
        memmove(p + 2, p, (wcslen(p) + 1) * sizeof *p);
        p[0] = L'Z';
        p[1] = L':';
    }
    for (wchar_t *c = p; *c; c++)
        if (*c == L'/')
            *c = L'\\';
    wchar_t full[PATHN];
    DWORD n = GetFullPathNameW(p, PATHN, full, NULL);
    if (n && n < PATHN)
        wcscpy(p, full);
    size_t len = wcslen(p);
    while (len > 3 && p[len - 1] == L'\\')
        p[--len] = 0;
}

static void parent_dir(wchar_t *p)
{
    wchar_t *s = wcsrchr(p, L'\\');
    if (s && s > p + 2)
        *s = 0;
    else if (s)
        s[1] = 0;
}

static unsigned char *read_file(const wchar_t *p, DWORD *len)
{
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    DWORD sz = GetFileSize(h, NULL), got = 0, r;
    unsigned char *b = sz != INVALID_FILE_SIZE && sz < 512u * 1024 * 1024 ? malloc(sz + 1) : NULL;
    while (b && got < sz && ReadFile(h, b + got, sz - got, &r, NULL) && r)
        got += r;
    CloseHandle(h);
    if (b && got != sz) {
        free(b);
        b = NULL;
    }
    if (b) {
        b[sz] = 0;
        *len = sz;
    }
    return b;
}

/* Write via <path>.btwtmp + rename, so a failure never leaves half a DLL. 0 or an error. */
static DWORD write_file(const wchar_t *path, const void *data, DWORD len)
{
    wchar_t tmp[PATHN];
    DWORD w = 0, e = 0;
    if (wcslen(path) + 8 > PATHN)
        return ERROR_FILENAME_EXCED_RANGE;
    wcscpy(tmp, path);
    wcscat(tmp, L".btwtmp");
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!WriteFile(h, data, len, &w, NULL) || w != len)
        e = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
    if (!e && !FlushFileBuffers(h))
        e = GetLastError();
    CloseHandle(h);
    if (!e && !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        e = GetLastError();
    if (e)
        DeleteFileW(tmp);
    return e;
}

static const unsigned char *find_bytes(const unsigned char *b, size_t n, const char *s)
{
    size_t m = strlen(s);
    for (size_t i = 0; m && i + m <= n; i++)
        if (b[i] == (unsigned char)s[0] && memcmp(b + i, s, m) == 0)
            return b + i;
    return NULL;
}

/* Our ichat.dll carries "btwsteam-version:<v>" (src/hook/log.c); builds before 0.1.0 only
 * have "btwsteam <v> (ichat.dll proxy) started". Returns 1 and the version if it is ours. */
static int btw_version_of(const unsigned char *b, size_t n, char *ver, size_t cap)
{
    const unsigned char *p = find_bytes(b, n, "btwsteam-version:");
    size_t k = 0;
    if (p) {
        p += 17;
        while (k + 1 < cap && p + k < b + n &&
               (isalnum(p[k]) || p[k] == '.' || p[k] == '-' || p[k] == '+'))
            k++;
        memcpy(ver, p, k);
        ver[k] = 0;
        return 1;
    }
    p = find_bytes(b, n, " (ichat.dll proxy) started");
    if (p) {
        const unsigned char *q = p;
        while (q > b && q[-1] != ' ' && p - q < 16)
            q--;
        k = (size_t)(p - q) < cap ? (size_t)(p - q) : cap - 1;
        memcpy(ver, q, k);
        ver[k] = 0;
        return 1;
    }
    return 0;
}

typedef struct {
    int  exists;
    DWORD len;
    char sha[65];
    int  ours;           /* contains the btwsteam marker */
    char ver[32];
} finfo_t;

static void probe(const wchar_t *path, finfo_t *f)
{
    memset(f, 0, sizeof *f);
    unsigned char *b = read_file(path, &f->len);
    if (!b)
        return;
    f->exists = 1;
    sha256_hex(b, f->len, f->sha);
    f->ours = btw_version_of(b, f->len, f->ver, sizeof f->ver);
    free(b);
}

static int same_file(const finfo_t *a, const finfo_t *b)
{
    return a->exists && b->exists && strcmp(a->sha, b->sha) == 0;
}

/* ---- the record (bin\btwsteam-installed.txt), key=value lines ------------------------- */

typedef struct { char key[40]; char val[PATHN * 3]; } kv_t;
static kv_t g_rec[32];
static int g_nrec;

static void record_load(const wchar_t *bin)
{
    wchar_t p[PATHN];
    DWORD n;
    g_nrec = 0;
    pjoin(p, bin, RECORD);
    char *b = (char *)read_file(p, &n);
    if (!b)
        return;
    for (char *line = strtok(b, "\r\n"); line && g_nrec < 32; line = strtok(NULL, "\r\n")) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq || eq - line >= 40)
            continue;
        *eq = 0;
        snprintf(g_rec[g_nrec].key, sizeof g_rec[0].key, "%s", line);
        snprintf(g_rec[g_nrec].val, sizeof g_rec[0].val, "%s", eq + 1);
        g_nrec++;
    }
    free(b);
}

static const char *rec(const char *key)
{
    for (int i = 0; i < g_nrec; i++)
        if (strcmp(g_rec[i].key, key) == 0)
            return g_rec[i].val;
    return "";
}

/* ---- payload ------------------------------------------------------------------------ */

typedef struct {
    const wchar_t *name;
    int res;
    const unsigned char *data;
    unsigned char *owned;
    DWORD len;
    char sha[65];
    char ver[32];
    wchar_t from[PATHN];   /* empty = embedded */
} payload_t;

static payload_t P_ICHAT = { L"ichat.dll", IDR_ICHAT, 0, 0, 0, "", "", L"" };
static payload_t P_STEAM = { L"steam_api.dll", IDR_STEAMAPI, 0, 0, 0, "", "", L"" };

static int payload_ok(payload_t *pl, const unsigned char *b, DWORD n)
{
    if (n < 1024 || b[0] != 'M' || b[1] != 'Z')
        return 0;
    if (pl->res == IDR_ICHAT)
        return btw_version_of(b, n, pl->ver, sizeof pl->ver);
    return find_bytes(b, n, "SteamAPI_") != NULL;
}

/* explicit: --ichat / --steam-api FILE (must be valid). sidecar_dir: the exe's folder, or
 * NULL when that is the game's bin\ itself. */
static int payload_load(payload_t *pl, const wchar_t *explicit_path, const wchar_t *sidecar_dir)
{
    wchar_t p[PATHN];
    DWORD n;
    unsigned char *b;
    if (explicit_path) {
        wcscpy(p, explicit_path);
        normalize(p);
        b = read_file(p, &n);
        if (!b || !payload_ok(pl, b, n)) {
            say("Error: %s is missing or is not a usable %s.\n",
                "Ошибка: %s отсутствует или не подходит как %s.\n", u8(p), u8(pl->name));
            free(b);
            return 0;
        }
        goto use_file;
    }
    if (sidecar_dir) {
        pjoin(p, sidecar_dir, pl->name);
        b = file_exists(p) ? read_file(p, &n) : NULL;
        if (b && payload_ok(pl, b, n))
            goto use_file;
        if (b)
            say("Note: %s is not a btwsteam build; using the copy inside the installer.\n",
                "Примечание: %s не из btwsteam; используется копия внутри установщика.\n", u8(p));
        free(b);
    }
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(pl->res), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    pl->data = g ? LockResource(g) : NULL;
    pl->len = r ? SizeofResource(NULL, r) : 0;
    if (!pl->data || !payload_ok(pl, pl->data, pl->len)) {
        say("Error: %s is not embedded in this installer and not found next to it.\n",
            "Ошибка: %s нет ни внутри установщика, ни рядом с ним.\n", u8(pl->name));
        return 0;
    }
    pl->from[0] = 0;
    sha256_hex(pl->data, pl->len, pl->sha);
    return 1;

use_file:
    pl->owned = b;
    pl->data = b;
    pl->len = n;
    wcscpy(pl->from, p);
    sha256_hex(b, n, pl->sha);
    return 1;
}

/* ---- finding the game ------------------------------------------------------------- */

/* d is the game folder, its bin\ or dmcr.exe: sets bin and returns 1 if dmcr.exe is there. */
static int game_bin_from(const wchar_t *d_in, wchar_t *bin)
{
    wchar_t d[PATHN], p[PATHN];
    if (wcslen(d_in) >= PATHN - 32)
        return 0;
    wcscpy(d, d_in);
    normalize(d);
    if (file_exists(d))
        parent_dir(d);
    pjoin(p, d, L"dmcr.exe");
    if (file_exists(p)) {
        wcscpy(bin, d);
        return 1;
    }
    pjoin(p, d, L"bin\\dmcr.exe");
    if (file_exists(p)) {
        pjoin(bin, d, L"bin");
        return 1;
    }
    return 0;
}

#define MAXLIST 24
typedef struct { int n; wchar_t p[MAXLIST][PATHN]; } plist_t;

static void plist_add(plist_t *l, const wchar_t *p_in)
{
    wchar_t p[PATHN];
    if (!p_in[0] || wcslen(p_in) >= PATHN - 64 || l->n >= MAXLIST)
        return;
    wcscpy(p, p_in);
    normalize(p);
    for (int i = 0; i < l->n; i++)
        if (_wcsicmp(l->p[i], p) == 0)
            return;
    wcscpy(l->p[l->n++], p);
}

static int reg_str(HKEY root, const wchar_t *sub, const wchar_t *name, REGSAM view, wchar_t *out)
{
    HKEY k;
    DWORD type = 0, sz = (PATHN - 1) * sizeof(wchar_t);
    out[0] = 0;
    if (RegOpenKeyExW(root, sub, 0, KEY_QUERY_VALUE | view, &k) != ERROR_SUCCESS)
        return 0;
    LONG r = RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        out[0] = 0;
        return 0;
    }
    out[sz / sizeof(wchar_t)] = 0;
    return out[0] != 0;
}

/* Next VDF token: 's' quoted string or bare word (in tok), '{', '}', or 0 at the end. */
static int vdf_next(const char **pp, char *tok, size_t cap)
{
    const char *p = *pp;
    size_t n = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        break;
    }
    if (!*p) {
        *pp = p;
        return 0;
    }
    if (*p == '{' || *p == '}') {
        *pp = p + 1;
        return *p;
    }
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) {
            char c = *p;
            if (c == '\\' && p[1]) {
                c = *++p;
                if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
            }
            if (n + 1 < cap)
                tok[n++] = c;
        }
        if (*p == '"')
            p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '{' && *p != '}' &&
               *p != '"')
            if (n + 1 < cap)
                tok[n++] = *p++;
            else
                p++;
    }
    tok[n] = 0;
    *pp = p;
    return 's';
}

static void utf8_to_w(const char *s, wchar_t *w)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, PATHN))
        w[0] = 0;
}

/* Library folders of one Steam root: the root itself plus libraryfolders.vdf. */
static void steam_libraries(const wchar_t *root, plist_t *libs)
{
    static const wchar_t *const vdfs[] = { L"steamapps\\libraryfolders.vdf", L"config\\libraryfolders.vdf" };
    plist_add(libs, root);
    for (int v = 0; v < 2; v++) {
        wchar_t p[PATHN];
        DWORD n;
        pjoin(p, root, vdfs[v]);
        char *b = (char *)read_file(p, &n);
        if (!b)
            continue;
        const char *c = b;
        char prev[PATHN * 2] = "", tok[PATHN * 2];
        int t, prev_t = 0;
        while ((t = vdf_next(&c, tok, sizeof tok)) != 0) {
            /* new format: "path" "<dir>"; old format: "1" "<dir>" (a numeric key whose value
             * looks like a path; "apps" { "570" "123" } has numbers as values) */
            if (t == 's' && prev_t == 's') {
                int numkey = prev[0] && strspn(prev, "0123456789") == strlen(prev);
                int pathy = strchr(tok, '\\') || strchr(tok, '/') || strchr(tok, ':');
                if ((strcmp(prev, "path") == 0 || (numkey && pathy)) && tok[0]) {
                    wchar_t w[PATHN];
                    utf8_to_w(tok, w);
                    plist_add(libs, w);
                }
                prev_t = 0; /* that was a key/value pair */
                continue;
            }
            prev_t = t;
            snprintf(prev, sizeof prev, "%s", t == 's' ? tok : "");
        }
        free(b);
    }
}

/* steamapps\appmanifest_4850.acf -> "installdir" (default "Cossacks Back to War"). */
static int library_game(const wchar_t *lib, wchar_t *bin)
{
    wchar_t p[PATHN], dir[PATHN] = L"Cossacks Back to War";
    DWORD n;
    pjoin(p, lib, L"steamapps\\appmanifest_" APPID_STR L".acf");
    char *b = (char *)read_file(p, &n);
    if (b) {
        const char *c = b;
        char tok[PATHN * 2];
        int t, want = 0;
        while ((t = vdf_next(&c, tok, sizeof tok)) != 0) {
            if (want && t == 's') {
                if (tok[0] && !strpbrk(tok, "\\/:"))
                    utf8_to_w(tok, dir);
                break;
            }
            want = t == 's' && _stricmp(tok, "installdir") == 0;
        }
        free(b);
    }
    wchar_t game[PATHN];
    pjoin(p, lib, L"steamapps\\common");
    pjoin(game, p, dir);
    return game_bin_from(game, bin);
}

/* Volume serial + file index of bin\dmcr.exe: the same game reached through different paths
 * (e.g. ~/.steam/steam is a symlink to ~/.local/share/Steam) has the same id. */
static unsigned long long game_id(const wchar_t *bin)
{
    wchar_t p[PATHN];
    BY_HANDLE_FILE_INFORMATION fi;
    unsigned long long id = 0;
    pjoin(p, bin, L"dmcr.exe");
    HANDLE h = CreateFileW(p, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    if (GetFileInformationByHandle(h, &fi))
        id = ((unsigned long long)fi.dwVolumeSerialNumber << 40) ^
             ((unsigned long long)fi.nFileIndexHigh << 32) ^ fi.nFileIndexLow;
    CloseHandle(h);
    return id;
}

static int find_steam_games(plist_t *games)
{
    plist_t roots = { 0 }, libs = { 0 };
    wchar_t v[PATHN];
    if (reg_str(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", 0, v))
        plist_add(&roots, v);
    if (reg_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_32KEY, v))
        plist_add(&roots, v);
    if (reg_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_64KEY, v))
        plist_add(&roots, v);
    if (reg_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", 0, v))
        plist_add(&roots, v);
    if (g_wine) { /* Linux Steam seen through Wine's Z: drive */
        static const wchar_t *const rel[] = { L"/.local/share/Steam", L"/.steam/steam", L"/.steam/root",
                                              L"/.var/app/com.valvesoftware.Steam/.local/share/Steam" };
        /* Wine does not pass $HOME to Windows programs; it sets WINEHOMEDIR = "\??\Z:\home\user".
         * Fall back to HOME (if a launcher exported it) and to Z:\home\%USERNAME%. */
        wchar_t home[PATHN] = L"", user[128];
        DWORD n = GetEnvironmentVariableW(L"WINEHOMEDIR", home, PATHN - 64);
        if (n && n < PATHN - 64 && wcsncmp(home, L"\\??\\", 4) == 0)
            memmove(home, home + 4, (wcslen(home + 4) + 1) * sizeof *home);
        else if (!(n && n < PATHN - 64)) {
            n = GetEnvironmentVariableW(L"HOME", home, PATHN - 64);
            if (!(n && n < PATHN - 64 && home[0] == L'/')) {
                n = GetEnvironmentVariableW(L"USERNAME", user, 128);
                if (n && n < 128)
                    swprintf(home, PATHN - 64, L"Z:\\home\\%ls", user);
                else
                    home[0] = 0;
            }
        }
        if (home[0])
            for (int i = 0; i < 4; i++) {
                wcscpy(v, home);
                wcscat(v, rel[i]);
                normalize(v);
                if (GetFileAttributesW(v) != INVALID_FILE_ATTRIBUTES)
                    plist_add(&roots, v);
            }
    }
    for (int i = 0; i < roots.n; i++)
        steam_libraries(roots.p[i], &libs);
    unsigned long long ids[MAXLIST];
    for (int i = 0; i < libs.n; i++) {
        wchar_t bin[PATHN];
        if (!library_game(libs.p[i], bin))
            continue;
        unsigned long long id = game_id(bin);
        int dup = 0;
        for (int j = 0; j < games->n && id; j++)
            dup |= ids[j] == id;
        if (dup)
            continue;
        if (games->n < MAXLIST)
            ids[games->n] = id;
        plist_add(games, bin);
    }
    return games->n;
}

static int find_game(const wchar_t *arg, const wchar_t *exe_dir, wchar_t *bin)
{
    if (arg) {
        if (game_bin_from(arg, bin))
            return 1;
        say("Error: no bin\\dmcr.exe in \"%s\".\n", "Ошибка: в \"%s\" нет bin\\dmcr.exe.\n", u8(arg));
        return 0;
    }
    if (game_bin_from(exe_dir, bin)) {
        say("Game found next to the installer: %s\n", "Игра найдена рядом с установщиком: %s\n", u8(bin));
        return 1;
    }
    plist_t games = { 0 };
    if (find_steam_games(&games)) {
        wcscpy(bin, games.p[0]);
        say("Game found in the Steam library: %s\n", "Игра найдена в библиотеке Steam: %s\n", u8(bin));
        for (int i = 1; i < games.n; i++)
            say("  (another copy: %s; give its folder as an argument to use it)\n",
                "  (ещё одна копия: %s; чтобы выбрать её, укажите папку аргументом)\n", u8(games.p[i]));
        return 1;
    }
    say("Could not find Cossacks: Back to War (Steam app 4850).\n"
        "Put btwsteam-setup.exe into the game folder and run it there, or drag the game folder\n"
        "onto btwsteam-setup.exe, or run: btwsteam-setup.exe \"D:\\path\\to\\Cossacks Back to War\"\n",
        "Не удалось найти Cossacks: Back to War (Steam app 4850).\n"
        "Положите btwsteam-setup.exe в папку игры и запустите там, или перетащите папку игры\n"
        "на btwsteam-setup.exe, или запустите: btwsteam-setup.exe \"D:\\путь\\к\\Cossacks Back to War\"\n");
    return 0;
}

/* ---- checks ------------------------------------------------------------------------- */

/* 1 if dmcr.exe from this bin\ runs (or a dmcr.exe whose path we cannot read). */
static int game_running(const wchar_t *bin, DWORD *pid)
{
    wchar_t exe[PATHN];
    pjoin(exe, bin, L"dmcr.exe");
    *pid = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof pe;
        for (BOOL ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe)) {
            if (_wcsicmp(pe.szExeFile, L"dmcr.exe") != 0)
                continue;
            wchar_t path[PATHN] = L"";
            HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pe.th32ProcessID);
            if (ms != INVALID_HANDLE_VALUE) {
                MODULEENTRY32W me;
                me.dwSize = sizeof me;
                if (Module32FirstW(ms, &me))
                    wcsncpy(path, me.szExePath, PATHN - 1);
                CloseHandle(ms);
            }
            if (path[0])
                normalize(path);
            if (!path[0] || _wcsicmp(path, exe) == 0) {
                *pid = pe.th32ProcessID;
                CloseHandle(s);
                return 1;
            }
        }
        CloseHandle(s);
    }
    /* a running exe cannot be opened for writing (nothing is written here) */
    HANDLE h = CreateFileW(exe, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    else if (GetLastError() == ERROR_SHARING_VIOLATION)
        return 1;
    return 0;
}

static int refuse_if_running(const wchar_t *bin)
{
    DWORD pid;
    if (!game_running(bin, &pid))
        return 0;
    say("Error: the game is running (dmcr.exe%s%lu). Quit it, then run the installer again.\n",
        "Ошибка: игра запущена (dmcr.exe%s%lu). Закройте её и запустите установщик снова.\n",
        pid ? ", pid " : "", (unsigned long)pid);
    return 1;
}

static int check_dmcr(const wchar_t *bin, int force)
{
    wchar_t p[PATHN];
    finfo_t f;
    pjoin(p, bin, L"dmcr.exe");
    probe(p, &f);
    if (f.exists && strcmp(f.sha, DMCR_SHA256) == 0) {
        say("dmcr.exe: v1.35 (Steam build 2684), supported.\n",
            "dmcr.exe: v1.35 (сборка Steam 2684), поддерживается.\n");
        return 1;
    }
    say("%s: dmcr.exe is not the supported version.\n"
        "  SHA-256 %s\n  expected %s (Steam v1.35, build 2684)\n"
        "  The mod patches code at fixed addresses and works only with that exact file.\n"
        "  Update the game in Steam (or \"Verify integrity of game files\") and try again.\n",
        "%s: dmcr.exe не той версии.\n"
        "  SHA-256 %s\n  нужен   %s (Steam v1.35, сборка 2684)\n"
        "  Мод правит код по фиксированным адресам и работает только с этим файлом.\n"
        "  Обновите игру в Steam (или «Проверить целостность файлов игры») и повторите.\n",
        force ? (g_ru ? "Предупреждение" : "Warning") : (g_ru ? "Ошибка" : "Error"),
        f.exists ? f.sha : "-", DMCR_SHA256);
    if (force)
        say("  --force given: installing anyway.\n", "  Указан --force: устанавливаем всё равно.\n");
    else
        say("  (--force installs anyway, at your own risk.)\n", "  (--force установит всё равно, на ваш риск.)\n");
    return force;
}

/* ---- status --------------------------------------------------------------------------- */

typedef struct {
    wchar_t bin[PATHN];
    wchar_t ichat[PATHN], ichat_orig[PATHN], steam[PATHN], steam_orig[PATHN], appid[PATHN], record[PATHN],
        log[PATHN];
    finfo_t fi, fio, fs, fso;
} game_t;

static void game_probe(game_t *g, const wchar_t *bin)
{
    wcscpy(g->bin, bin);
    pjoin(g->ichat, bin, L"ichat.dll");
    pjoin(g->ichat_orig, bin, L"ichat_orig.dll");
    pjoin(g->steam, bin, L"steam_api.dll");
    pjoin(g->steam_orig, bin, L"steam_api_orig.dll");
    pjoin(g->appid, bin, L"steam_appid.txt");
    pjoin(g->record, bin, RECORD);
    pjoin(g->log, bin, L"btwsteam.log");
    probe(g->ichat, &g->fi);
    probe(g->ichat_orig, &g->fio);
    probe(g->steam, &g->fs);
    probe(g->steam_orig, &g->fso);
    record_load(bin);
}

static const char *short_sha(const finfo_t *f)
{
    static char b[4][16];
    static int i;
    char *s = b[i++ % 4];
    snprintf(s, 16, "%.12s", f->sha);
    return s;
}

static const char *ichat_desc(const finfo_t *f)
{
    static char b[2][128];
    static int i;
    char *s = b[i++ % 2];
    if (!f->exists)
        snprintf(s, 128, "%s", g_ru ? "нет" : "absent");
    else if (f->ours)
        snprintf(s, 128, "btwsteam %s (%s)", f->ver, short_sha(f));
    else if (strcmp(f->sha, ICHAT_GSC_SHA256) == 0)
        snprintf(s, 128, "%s (%s)", g_ru ? "оригинал GSC" : "original GSC", short_sha(f));
    else
        snprintf(s, 128, "%s (%s)", g_ru ? "не btwsteam" : "not btwsteam", short_sha(f));
    return s;
}

static int appid_is_ours(const wchar_t *p)
{
    DWORD n;
    char *b = (char *)read_file(p, &n);
    if (!b)
        return 0;
    char *e = b + strlen(b);
    while (e > b && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
    int r = strcmp(b, APPID_STR) == 0;
    free(b);
    return r;
}

static int cmd_status(const wchar_t *bin)
{
    game_t g;
    wchar_t p[PATHN];
    finfo_t fd;
    DWORD pid;
    game_probe(&g, bin);
    pjoin(p, bin, L"dmcr.exe");
    probe(p, &fd);
    say("Game folder:     %s\n", "Папка игры:      %s\n", u8(bin));
    say("dmcr.exe:        %s\n", "dmcr.exe:        %s\n",
        strcmp(fd.sha, DMCR_SHA256) == 0 ? "v1.35 (OK)" : (g_ru ? "НЕ та версия" : "NOT the supported version"));
    say("ichat.dll:       %s\n", "ichat.dll:       %s\n", ichat_desc(&g.fi));
    say("ichat_orig.dll:  %s\n", "ichat_orig.dll:  %s\n", ichat_desc(&g.fio));
    say("steam_api.dll:   %s%s\n", "steam_api.dll:   %s%s\n",
        g.fs.exists ? short_sha(&g.fs) : (g_ru ? "нет" : "absent"),
        g.fs.exists && strcmp(g.fs.sha, rec("steam_api.dll")) == 0 ? (g_ru ? " (установлен btwsteam)" : " (installed by btwsteam)") : "");
    if (g.fso.exists)
        say("steam_api_orig.dll: %s\n", "steam_api_orig.dll: %s\n", short_sha(&g.fso));
    say("steam_appid.txt: %s\n", "steam_appid.txt: %s\n",
        file_exists(g.appid) ? (appid_is_ours(g.appid) ? APPID_STR : "?") : (g_ru ? "нет" : "absent"));
    if (g_nrec)
        say("Record:          %s, version %s, installed %s by %s\n",
            "Запись:          %s, версия %s, установлено %s (%s)\n", u8(RECORD), rec("version"), rec("installed"),
            rec("installer"));
    else
        say("Record:          none\n", "Запись:          нет\n");
    DWORD n;
    char *lb = (char *)read_file(g.log, &n);
    if (lb) {
        char *nl = strpbrk(lb, "\r\n");
        if (nl)
            *nl = 0;
        say("Log:             %s\n                 %s\n", "Журнал:          %s\n                 %s\n", u8(g.log),
            lb);
        free(lb);
    }
    if (game_running(bin, &pid))
        say("The game is running now.\n", "Игра сейчас запущена.\n");

    if (g.fi.ours && g.fio.exists && !g.fio.ours)
        say("State: INSTALLED (btwsteam %s).\n", "Состояние: УСТАНОВЛЕН (btwsteam %s).\n", g.fi.ver);
    else if (g.fi.ours)
        say("State: btwsteam %s is in place, but there is no backup ichat_orig.dll.\n",
            "Состояние: btwsteam %s на месте, но копии ichat_orig.dll нет.\n", g.fi.ver);
    else if (g.fio.exists || g_nrec)
        say("State: NOT active: ichat.dll is the game's own (Steam \"Verify integrity\" restores it). Run the "
            "installer again.\n",
            "Состояние: НЕ активен: ichat.dll снова оригинальный (его возвращает проверка целостности в "
            "Steam). Запустите установку ещё раз.\n");
    else
        say("State: not installed.\n", "Состояние: не установлен.\n");
    return RC_OK;
}

/* ---- install -------------------------------------------------------------------------- */

static int do_move(const wchar_t *from, const wchar_t *to, int replace)
{
    if (MoveFileExW(from, to, (replace ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH))
        return 1;
    DWORD e = GetLastError();
    say("Error: cannot rename %s -> %s: %s\n", "Ошибка: не удалось переименовать %s -> %s: %s\n", u8(from),
        u8(to), errstr(e));
    if (e == ERROR_ACCESS_DENIED)
        say("  Try right-click -> \"Run as administrator\".\n",
            "  Попробуйте: правый клик -> «Запуск от имени администратора».\n");
    return 0;
}

static int put_payload(const payload_t *pl, const wchar_t *path)
{
    DWORD e = write_file(path, pl->data, pl->len);
    if (e) {
        say("Error: cannot write %s: %s\n", "Ошибка: не удалось записать %s: %s\n", u8(path), errstr(e));
        if (e == ERROR_ACCESS_DENIED)
            say("  Try right-click -> \"Run as administrator\".\n",
                "  Попробуйте: правый клик -> «Запуск от имени администратора».\n");
        return 0;
    }
    finfo_t f;
    probe(path, &f);
    if (!f.exists || strcmp(f.sha, pl->sha) != 0) {
        say("Error: %s does not match what was written.\n", "Ошибка: %s не совпадает с записанным.\n", u8(path));
        return 0;
    }
    return 1;
}

static int write_record(const game_t *g, const char *appid_pre)
{
    char b[8192];
    SYSTEMTIME t;
    finfo_t fd, fi, fio, fs, fso;
    wchar_t p[PATHN];
    GetLocalTime(&t);
    pjoin(p, g->bin, L"dmcr.exe");
    probe(p, &fd);
    probe(g->ichat, &fi);
    probe(g->ichat_orig, &fio);
    probe(g->steam, &fs);
    probe(g->steam_orig, &fso);
    int n = snprintf(b, sizeof b,
                     "# btwsteam (Cossacks: Back to War multiplayer over Steam). Written by the installer,\r\n"
                     "# read by --uninstall / --status (btwsteam-setup.exe and install.sh). Do not edit.\r\n"
                     "version=%s\r\ninstalled=%04u-%02u-%02u %02u:%02u:%02u\r\n"
                     "installer=btwsteam-setup.exe %s\r\ngame_bin=%s\r\n"
                     "dmcr.exe=%s\r\nichat.dll=%s\r\nichat_orig.dll=%s\r\nsteam_api.dll=%s\r\n",
                     fi.ver, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, BTWSTEAM_VERSION,
                     u8(g->bin), fd.sha, fi.sha, fio.sha, fs.sha);
    if (fso.exists)
        n += snprintf(b + n, sizeof b - (size_t)n, "steam_api_orig.dll=%s\r\n", fso.sha);
    snprintf(b + n, sizeof b - (size_t)n, "steam_appid_preexisting=%s\r\n", appid_pre);
    DWORD e = write_file(g->record, b, (DWORD)strlen(b));
    if (e)
        say("Warning: cannot write %s: %s\n", "Предупреждение: не удалось записать %s: %s\n", u8(g->record),
            errstr(e));
    return e == 0;
}

static int cmd_install(const wchar_t *bin, int force, const wchar_t *ichat_src, const wchar_t *steam_src,
                       const wchar_t *sidecar_dir)
{
    game_t g;
    if (!check_dmcr(bin, force))
        return RC_VERSION;
    if (refuse_if_running(bin))
        return RC_RUNNING;
    if (!payload_load(&P_ICHAT, ichat_src, sidecar_dir) || !payload_load(&P_STEAM, steam_src, sidecar_dir))
        return RC_FAIL;
    if (P_ICHAT.from[0])
        say("Using %s (btwsteam %s) instead of the built-in copy.\n",
            "Используется %s (btwsteam %s) вместо встроенной копии.\n", u8(P_ICHAT.from), P_ICHAT.ver);
    if (P_STEAM.from[0])
        say("Using %s instead of the built-in copy.\n", "Используется %s вместо встроенной копии.\n",
            u8(P_STEAM.from));
    game_probe(&g, bin);

    /* steam_appid.txt: the DLL writes "4850" at its first start. Remember whether a file was
     * there before the mod ever was, so --uninstall leaves that one alone. */
    char appid_pre[8];
    if (g_nrec && rec("steam_appid_preexisting")[0])
        snprintf(appid_pre, sizeof appid_pre, "%s", rec("steam_appid_preexisting"));
    else
        snprintf(appid_pre, sizeof appid_pre, "%d", file_exists(g.appid) && !g.fi.ours && !g.fio.exists);

    /* 1. the game's ichat.dll -> ichat_orig.dll */
    int moved = 0;
    if (g.fio.ours) {
        if (!g.fi.exists || g.fi.ours) {
            say("Error: ichat_orig.dll is a btwsteam build, not the game's file, and ichat.dll is not the\n"
                "  original either. Use Steam's \"Verify integrity of game files\", then run the installer again.\n",
                "Ошибка: ichat_orig.dll — сборка btwsteam, а не файл игры, и ichat.dll тоже не оригинал.\n"
                "  Выполните в Steam «Проверить целостность файлов игры» и запустите установку снова.\n");
            return RC_FAIL;
        }
        if (!do_move(g.ichat, g.ichat_orig, 1))
            return RC_FAIL;
        moved = 1;
        say("ichat_orig.dll held a btwsteam build; replaced with the game's ichat.dll.\n",
            "В ichat_orig.dll была сборка btwsteam; заменена оригинальным ichat.dll.\n");
    } else if (!g.fio.exists) {
        if (!g.fi.exists) {
            say("Error: bin\\ichat.dll is missing. Use Steam's \"Verify integrity of game files\" first.\n",
                "Ошибка: нет bin\\ichat.dll. Сначала выполните в Steam «Проверить целостность файлов игры».\n");
            return RC_FAIL;
        }
        if (g.fi.ours) {
            say("Error: ichat.dll is already btwsteam %s, but the game's original (ichat_orig.dll) is gone.\n"
                "  Use Steam's \"Verify integrity of game files\" (it restores ichat.dll), then run the installer again.\n",
                "Ошибка: ichat.dll уже btwsteam %s, но оригинала (ichat_orig.dll) нет.\n"
                "  Выполните в Steam «Проверить целостность файлов игры» (вернёт ichat.dll) и запустите установку снова.\n",
                g.fi.ver);
            return RC_FAIL;
        }
        if (!do_move(g.ichat, g.ichat_orig, 0))
            return RC_FAIL;
        moved = 1;
        say("Original ichat.dll kept as ichat_orig.dll.\n", "Оригинальный ichat.dll сохранён как ichat_orig.dll.\n");
    } else if (g.fi.exists && !g.fi.ours && !same_file(&g.fi, &g.fio)) {
        /* Steam put a different original back (update / verify): that one is the backup now */
        if (!do_move(g.ichat, g.ichat_orig, 1))
            return RC_FAIL;
        moved = 1;
        say("ichat.dll was replaced by Steam since the last install; the new original is kept as ichat_orig.dll.\n",
            "ichat.dll был заменён Steam после прошлой установки; новый оригинал сохранён как ichat_orig.dll.\n");
    } else {
        say("Backup ichat_orig.dll already present (kept).\n", "Копия ichat_orig.dll уже есть (оставлена).\n");
    }

    /* 2. our ichat.dll */
    if (!put_payload(&P_ICHAT, g.ichat)) {
        if (moved && !file_exists(g.ichat) && MoveFileExW(g.ichat_orig, g.ichat, 0))
            say("Rolled back: the original ichat.dll is in place again.\n",
                "Откат: оригинальный ichat.dll возвращён на место.\n");
        return RC_FAIL;
    }
    say("Installed ichat.dll (btwsteam %s).\n", "Установлен ichat.dll (btwsteam %s).\n", P_ICHAT.ver);

    /* 3. steam_api.dll; a foreign one is kept as steam_api_orig.dll */
    if (g.fs.exists && strcmp(g.fs.sha, P_STEAM.sha) != 0 && strcmp(g.fs.sha, rec("steam_api.dll")) != 0 &&
        !g.fso.exists) {
        if (!do_move(g.steam, g.steam_orig, 0))
            return RC_FAIL;
        say("The existing steam_api.dll is kept as steam_api_orig.dll.\n",
            "Имевшийся steam_api.dll сохранён как steam_api_orig.dll.\n");
    }
    if (!put_payload(&P_STEAM, g.steam))
        return RC_FAIL;
    say("Installed steam_api.dll.\n", "Установлен steam_api.dll.\n");

    write_record(&g, appid_pre);
    say("\nDone: btwsteam %s is installed in %s\n"
        "Play: start the game from Steam -> Multiplayer -> Deathmatch -> \"GSC-Game.Net Internet Game\" -> Create.\n"
        "Friends first join your Steam lobby (your invite, or \"Join game\" in the friends list), then click the same Create.\n"
        "After Steam's \"Verify integrity of game files\" run this installer again. Log: bin\\btwsteam.log\n",
        "\nГотово: btwsteam %s установлен в %s\n"
        "Игра: запустите игру из Steam -> Multiplayer -> Deathmatch -> «GSC-Game.Net Internet Game» -> Create.\n"
        "Друзья сначала входят в ваше лобби Steam (приглашение или «Присоединиться к игре» в списке друзей), потом жмут ту же кнопку Create.\n"
        "После проверки целостности файлов в Steam запустите установщик снова. Журнал: bin\\btwsteam.log\n",
        P_ICHAT.ver, u8(bin));
    return RC_OK;
}

/* ---- uninstall ------------------------------------------------------------------------ */

static int cmd_uninstall(const wchar_t *bin)
{
    game_t g;
    int rc = RC_OK, changed = 0;
    if (refuse_if_running(bin))
        return RC_RUNNING;
    game_probe(&g, bin);

    /* ichat.dll */
    if (g.fio.exists && !g.fio.ours) {
        if (!g.fi.exists || g.fi.ours) {
            if (!do_move(g.ichat_orig, g.ichat, 1))
                return RC_FAIL;
            say("Restored the original ichat.dll.\n", "Оригинальный ichat.dll восстановлен.\n");
            changed = 1;
        } else if (same_file(&g.fi, &g.fio)) {
            DeleteFileW(g.ichat_orig);
            say("ichat.dll is already the original; removed the extra ichat_orig.dll.\n",
                "ichat.dll уже оригинальный; лишний ichat_orig.dll удалён.\n");
            changed = 1;
        } else {
            say("ichat.dll is not btwsteam and differs from ichat_orig.dll; both left as they are.\n",
                "ichat.dll не из btwsteam и отличается от ichat_orig.dll; оба файла оставлены как есть.\n");
        }
    } else if (g.fi.ours) {
        say("Error: ichat.dll is btwsteam but there is no usable ichat_orig.dll to restore.\n"
            "  Use Steam's \"Verify integrity of game files\": it downloads the original ichat.dll.\n",
            "Ошибка: ichat.dll из btwsteam, а годного ichat_orig.dll для восстановления нет.\n"
            "  Выполните в Steam «Проверить целостность файлов игры»: Steam скачает оригинальный ichat.dll.\n");
        rc = RC_FAIL;
    } else {
        say("ichat.dll is the game's own; nothing to restore.\n", "ichat.dll — файл игры; восстанавливать нечего.\n");
    }

    /* steam_api.dll */
    if (g.fso.exists) {
        if (!do_move(g.steam_orig, g.steam, 1))
            return RC_FAIL;
        say("Restored the previous steam_api.dll from steam_api_orig.dll.\n",
            "Прежний steam_api.dll восстановлен из steam_api_orig.dll.\n");
        changed = 1;
    } else if (g.fs.exists) {
        int ours = strcmp(g.fs.sha, rec("steam_api.dll")) == 0;
        if (!ours) { /* no record (or another build): compare with the copy we carry */
            HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_STEAMAPI), MAKEINTRESOURCEW(10));
            HGLOBAL h = r ? LoadResource(NULL, r) : NULL;
            const void *d = h ? LockResource(h) : NULL;
            char sha[65];
            if (d) {
                sha256_hex(d, SizeofResource(NULL, r), sha);
                ours = strcmp(sha, g.fs.sha) == 0;
            }
        }
        if (ours) {
            if (DeleteFileW(g.steam)) {
                say("Removed steam_api.dll.\n", "steam_api.dll удалён.\n");
                changed = 1;
            } else {
                say("Error: cannot delete %s: %s\n", "Ошибка: не удалось удалить %s: %s\n", u8(g.steam),
                    errstr(GetLastError()));
                rc = RC_FAIL;
            }
        } else {
            say("steam_api.dll was not installed by btwsteam; left in place.\n",
                "steam_api.dll поставлен не btwsteam; оставлен.\n");
        }
    }

    /* steam_appid.txt ("4850", written by our DLL) unless it was there before the mod */
    if (file_exists(g.appid) && appid_is_ours(g.appid) && strcmp(rec("steam_appid_preexisting"), "1") != 0) {
        if (DeleteFileW(g.appid)) {
            say("Removed steam_appid.txt.\n", "steam_appid.txt удалён.\n");
            changed = 1;
        }
    }
    if (file_exists(g.record)) {
        if (DeleteFileW(g.record))
            changed = 1;
        else
            rc = RC_FAIL;
    }
    if (rc == RC_OK)
        say(changed ? "\nDone: btwsteam is removed. Logs (btwsteam.log) are kept.\n"
                    : "\nbtwsteam was not installed; nothing changed.\n",
            changed ? "\nГотово: btwsteam удалён. Журналы (btwsteam.log) оставлены.\n"
                    : "\nbtwsteam не был установлен; ничего не изменено.\n");
    return rc;
}

/* ---- main ----------------------------------------------------------------------------- */

static void usage(void)
{
    say("btwsteam-setup %s: Steam multiplayer for Cossacks: Back to War (Steam, v1.35)\n\n"
        "  btwsteam-setup.exe [GAME_DIR] [--install | --uninstall | --status] [options]\n\n"
        "  GAME_DIR      the game folder (or its bin\\); default: this exe's folder, then Steam\n"
        "  --install     install or update (the default)\n"
        "  --uninstall   restore the original files\n"
        "  --status      show what is installed\n"
        "  --force       install even if dmcr.exe is not v1.35\n"
        "  --no-pause    do not wait for Enter at the end\n"
        "  --lang en|ru  language of the messages\n\n"
        "Exit codes: 0 ok, 1 failed, 2 bad arguments, 3 game not found, 4 wrong dmcr.exe, 5 game running.\n",
        "btwsteam-setup %s: сетевая игра через Steam для Cossacks: Back to War (Steam, v1.35)\n\n"
        "  btwsteam-setup.exe [ПАПКА_ИГРЫ] [--install | --uninstall | --status] [параметры]\n\n"
        "  ПАПКА_ИГРЫ    папка игры (или её bin\\); по умолчанию: папка этого exe, потом Steam\n"
        "  --install     установить или обновить (по умолчанию)\n"
        "  --uninstall   вернуть оригинальные файлы\n"
        "  --status      показать, что установлено\n"
        "  --force       установить, даже если dmcr.exe не v1.35\n"
        "  --no-pause    не ждать Enter в конце\n"
        "  --lang en|ru  язык сообщений\n\n"
        "Коды выхода: 0 успех, 1 ошибка, 2 неверные аргументы, 3 игра не найдена, 4 не тот dmcr.exe, 5 игра запущена.\n",
        BTWSTEAM_VERSION);
}

static int read_line(char *buf, int cap)
{
    DWORD r = 0;
    int n = 0;
    char c;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    while (ReadFile(h, &c, 1, &r, NULL) && r == 1 && c != '\n')
        if (c != '\r' && n + 1 < cap)
            buf[n++] = c;
    buf[n] = 0;
    return n;
}

static int stdin_console(void)
{
    DWORD m;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &m) != 0;
}

/* Started by a double click (or a drop onto the exe): the console is ours alone. */
static int own_console(void)
{
    DWORD l[4];
    return GetConsoleProcessList(l, 4) == 1 && stdin_console();
}

int wmain(int argc, wchar_t **argv)
{
    enum { C_NONE, C_INSTALL, C_UNINSTALL, C_STATUS } cmd = C_NONE;
    const wchar_t *game_arg = NULL, *ichat_src = NULL, *steam_src = NULL;
    int force = 0, no_pause = 0, rc;
    wchar_t lang[8];

    g_wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != NULL;
    g_ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    if (GetEnvironmentVariableW(L"BTWSTEAM_LANG", lang, 8) && lang[0])
        g_ru = _wcsicmp(lang, L"ru") == 0;

    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        if (!wcscmp(a, L"--install"))
            cmd = C_INSTALL;
        else if (!wcscmp(a, L"--uninstall") || !wcscmp(a, L"--remove"))
            cmd = C_UNINSTALL;
        else if (!wcscmp(a, L"--status"))
            cmd = C_STATUS;
        else if (!wcscmp(a, L"--force"))
            force = 1;
        else if (!wcscmp(a, L"--no-pause"))
            no_pause = 1;
        else if (!wcscmp(a, L"--lang") && i + 1 < argc)
            g_ru = _wcsicmp(argv[++i], L"ru") == 0;
        else if (!wcscmp(a, L"--ichat") && i + 1 < argc)
            ichat_src = argv[++i];
        else if (!wcscmp(a, L"--steam-api") && i + 1 < argc)
            steam_src = argv[++i];
        else if (!wcscmp(a, L"-h") || !wcscmp(a, L"--help") || !wcscmp(a, L"/?")) {
            usage();
            return RC_OK;
        } else if (a[0] == L'-' && a[1] == L'-') {
            say("Unknown option: %s\n\n", "Неизвестный параметр: %s\n\n", u8(a));
            usage();
            return RC_USAGE;
        } else if (!game_arg)
            game_arg = a;
        else {
            say("Only one game folder, please.\n", "Укажите только одну папку игры.\n");
            return RC_USAGE;
        }
    }
    int pause = !no_pause && own_console();
    int menu = pause && cmd == C_NONE && !game_arg;

    say("btwsteam-setup %s: Cossacks: Back to War multiplayer over Steam\n\n",
        "btwsteam-setup %s: сетевая игра Cossacks: Back to War через Steam\n\n", BTWSTEAM_VERSION);

    wchar_t exe_dir[PATHN], bin[PATHN];
    DWORD n = GetModuleFileNameW(NULL, exe_dir, PATHN);
    if (!n || n >= PATHN)
        exe_dir[0] = 0;
    normalize(exe_dir);
    parent_dir(exe_dir);

    if (!find_game(game_arg, exe_dir, bin)) {
        rc = RC_NOGAME;
    } else {
        /* files next to the exe are payload only when the exe is not inside the game's bin\ */
        const wchar_t *sidecar = _wcsicmp(exe_dir, bin) == 0 ? NULL : exe_dir;
        if (menu) {
            char line[16];
            say("\n", "\n");
            cmd_status(bin);
            say("\nEnter = install / update, U + Enter = uninstall, Q + Enter = quit: ",
                "\nEnter = установить / обновить, U + Enter = удалить, Q + Enter = выход: ");
            read_line(line, sizeof line);
            say("\n", "\n");
            cmd = line[0] == 'u' || line[0] == 'U' ? C_UNINSTALL
                : line[0] == 'q' || line[0] == 'Q' ? C_STATUS
                : C_INSTALL;
            if (cmd == C_STATUS) {
                rc = RC_OK;
                goto out;
            }
        }
        if (cmd == C_STATUS)
            rc = cmd_status(bin);
        else if (cmd == C_UNINSTALL)
            rc = cmd_uninstall(bin);
        else
            rc = cmd_install(bin, force, ichat_src, steam_src, sidecar);
    }
out:
    if (pause) {
        char line[8];
        say("\nPress Enter to close.", "\nНажмите Enter, чтобы закрыть.");
        read_line(line, sizeof line);
    }
    free(P_ICHAT.owned);
    free(P_STEAM.owned);
    return rc;
}
