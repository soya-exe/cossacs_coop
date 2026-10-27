/* btwlaunch.exe: test-harness stand-in for the Steam-DRM'd csbtw.exe launcher.
 *
 * csemu.dll (dmcr.exe's ddraw/winmm shim) refuses DirectDrawCreate unless the launcher's
 * named event exists: "CSBTW_LAUNCHER_" + ('@'+month) + ('0'+dayOfWeek) + ('`'+day%26),
 * from GetSystemTime (UTC)  -> "LauncherInterfaceCheck() failed with error 2".
 * csbtw.exe creates it, but csbtw.exe is SteamStub-wrapped and only runs with a real Steam
 * client ("Application load error 5:0000065434"). This creates the same event (for now and
 * for +10 minutes, in case UTC midnight passes during start-up), then runs
 *     dmcr.exe <all our arguments>
 * in the current directory and waits for it; exit code = dmcr.exe's.
 * Build: i686-w64-mingw32-gcc -O2 -s -o btwlaunch.exe btwlaunch.c
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static HANDLE make_event(const SYSTEMTIME *t)
{
    char name[64];
    int day = t->wDay;
    if (day >= 26) day -= 26;
    snprintf(name, sizeof name, "CSBTW_LAUNCHER_%c%c%c",
             (char)(0x40 + (t->wMonth & 0xff)), (char)(0x30 + (t->wDayOfWeek & 0xff)),
             (char)(0x60 + day));
    HANDLE h = CreateEventA(NULL, TRUE, FALSE, name);
    printf("btwlaunch: event %s %s\n", name, h ? "ok" : "FAILED");
    return h;
}

int main(void)
{
    SYSTEMTIME now, later;
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTime(&now);
    make_event(&now);
    SystemTimeToFileTime(&now, &ft);
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    u.QuadPart += 10ULL * 60 * 10000000;
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &later);
    if (later.wDay != now.wDay) make_event(&later);

    /* Skip argv[0] in the raw command line, keep the rest verbatim. */
    const char *cl = GetCommandLineA();
    if (*cl == '"') { cl++; while (*cl && *cl != '"') cl++; if (*cl) cl++; }
    else while (*cl && *cl != ' ' && *cl != '\t') cl++;
    while (*cl == ' ' || *cl == '\t') cl++;

    const char *exe = getenv("BTW_GAME_EXE");
    if (!exe || !*exe) exe = "dmcr.exe";
    /* A bare name is resolved against the current directory (the game's bin/), not against
     * this launcher's own directory. */
    char path[MAX_PATH];
    if (!strchr(exe, '\\') && !strchr(exe, '/')) {
        DWORD n = GetCurrentDirectoryA(MAX_PATH, path);
        snprintf(path + n, sizeof path - n, "\\%s", exe);
    } else {
        snprintf(path, sizeof path, "%s", exe);
    }
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "\"%s\"%s%s", path, *cl ? " " : "", cl);

    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(path, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("btwlaunch: CreateProcess(%s) failed: %lu\n", cmd, GetLastError());
        return 1;
    }
    printf("btwlaunch: started pid %lu: %s\n", pi.dwProcessId, cmd);
    fflush(stdout);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0;
    GetExitCodeProcess(pi.hProcess, &rc);
    printf("btwlaunch: %s exited with %lu (0x%lx)\n", exe, rc, rc);
    return (int)rc;
}
