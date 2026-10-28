/*
 * btwsteam.log next to dmcr.exe. Every line goes straight to WriteFile (no stdio
 * buffering), so the log survives a crash. The previous run is kept as
 * btwsteam.prev.log. btw_log preserves GetLastError, which is also
 * WSAGetLastError, so hooks may log between the real call and their return.
 */
#include "btwsteam.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

btw_counters_t g_ctr;

static CRITICAL_SECTION g_log_cs;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static char g_dir[MAX_PATH];

/* Marker for installer/setup.c and installer/install.sh (they search the DLL's bytes). */
__attribute__((used)) const char btw_version_tag[] = "btwsteam-version:" BTWSTEAM_VERSION;

const char *btw_exe_dir(void)
{
    return g_dir;
}

void btw_log_init(void)
{
    char path[MAX_PATH + 32], prev[MAX_PATH + 32];
    DWORD n = GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        strcpy(g_dir, ".\\");
    else {
        char *slash = strrchr(g_dir, '\\');
        char *fslash = strrchr(g_dir, '/');
        if (fslash > slash)
            slash = fslash;
        if (slash)
            slash[1] = 0;
        else
            strcpy(g_dir, ".\\");
    }
    InitializeCriticalSection(&g_log_cs);

    snprintf(path, sizeof path, "%sbtwsteam.log", g_dir);
    snprintf(prev, sizeof prev, "%sbtwsteam.prev.log", g_dir);
    MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    SYSTEMTIME t;
    GetLocalTime(&t);
    btw_log("btwsteam " BTWSTEAM_VERSION " (ichat.dll proxy) started %04u-%02u-%02u, pid %lu, dir %s",
            t.wYear, t.wMonth, t.wDay, (unsigned long)GetCurrentProcessId(), g_dir);
}

void btw_log(const char *fmt, ...)
{
    DWORD saved = GetLastError();
    if (g_log == INVALID_HANDLE_VALUE) {
        SetLastError(saved);
        return;
    }
    char line[1200];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(line, sizeof line, "%02u:%02u:%02u.%03u [%5lu] ", t.wHour, t.wMinute,
                     t.wSecond, t.wMilliseconds, (unsigned long)GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (size_t)n - 2, fmt, ap);
    va_end(ap);
    if (m < 0)
        m = 0;
    n += m;
    if (n > (int)sizeof line - 3)
        n = (int)sizeof line - 3;
    line[n++] = '\r';
    line[n++] = '\n';

    DWORD w;
    EnterCriticalSection(&g_log_cs);
    WriteFile(g_log, line, (DWORD)n, &w, NULL);
    LeaveCriticalSection(&g_log_cs);
    SetLastError(saved);
}

void btw_counters_dump(void)
{
    btw_counters_t c = g_ctr; /* racy snapshot, fine for statistics */
    btw_log("stats: tx uni %ld bcast-dropped %ld bytes %ld loop %ld | tx drop nonvlan %ld noslot %ld dead %ld "
            "queue %ld steamerr %ld | rx %ld bytes %ld reads %ld | rx drop unknown %ld bad %ld "
            "nosock %ld qfull %ld noslot %ld | ctrl tx %ld rx %ld",
            c.tx_uni, c.tx_bcast, c.tx_bytes, c.tx_loop, c.tx_drop_nonvlan, c.tx_drop_noslot,
            c.tx_drop_dead, c.tx_drop_queue, c.tx_steam_err, c.rx_pkts, c.rx_bytes,
            c.rx_game_reads, c.rx_drop_unknown, c.rx_drop_bad, c.rx_drop_nosock, c.rx_drop_qfull,
            c.rx_drop_noslot, c.ctrl_tx, c.ctrl_rx);
}
