/*
 * DllMain of our ichat.dll. The 12 IChat exports live in ichat.c; this file only
 * sets up the machinery.
 *
 * DllMain runs under the loader lock, so it stays minimal: a critical section, the
 * log file, the IAT patch, the login code patch and the perf patches (perf.c; plain
 * memory writes + VirtualProtect) and CreateThread.
 * steam_api.dll is loaded later by the worker thread, which cannot start running
 * before the loader lock is released.
 */
#include "btwsteam.h"
#include <stdlib.h>
#include <string.h>

static CRITICAL_SECTION g_cs;

void btw_lock(void)   { EnterCriticalSection(&g_cs); }
void btw_unlock(void) { LeaveCriticalSection(&g_cs); }

const char *btw_env(const char *name, char *buf, size_t cap)
{
    DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)cap);
    if (n == 0 || n >= cap)
        buf[0] = 0;
    return buf;
}

const char *btw_env_utf8(const char *name, char *buf, size_t cap)
{
    wchar_t wname[64], w[256];
    buf[0] = 0;
    if (!MultiByteToWideChar(CP_ACP, 0, name, -1, wname, 64))
        return buf;
    DWORD n = GetEnvironmentVariableW(wname, w, 256);
    if (n == 0 || n >= 256 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)cap, NULL, NULL))
        buf[0] = 0;
    return buf;
}

int btw_env_flag(const char *name)
{
    char v[16];
    btw_env(name, v, sizeof v);
    return v[0] && strcmp(v, "0") != 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        btw_log_init();
        if (btw_env_flag("BTWSTEAM_DISABLE")) {
            btw_log("BTWSTEAM_DISABLE set: no hooks, no patch, no Steam (IChat calls are no-ops)");
            break;
        }
        btw_timing_init();
        btw_vlan_init();
        btw_peers_init();
        btw_log("IAT: patched %d WS2_32 slot(s) in the main module", btw_iat_patch(1));
        btw_patch_login();
        btw_perf_patch();
        btw_session_start();
        break;
    case DLL_PROCESS_DETACH:
        /* reserved != NULL: the process is exiting and the other threads are already
         * gone; touching Steam now could deadlock. Steam itself notices that we left
         * the lobby when the process disappears. We do not even log then: a thread
         * killed while holding the log lock would make us wait forever. */
        if (reserved == NULL) {
            btw_log("detach (FreeLibrary)");
            btw_session_stop();
        }
        break;
    }
    return TRUE;
}
