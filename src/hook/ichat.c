/*
 * The IChat interface, implemented by us (re/FINDINGS.md "Impact on the hook").
 *
 * The game has no LAN discovery: its native UDP transport unicasts to a host IP
 * string that normally comes from an internet lobby (GameSpy, GSC-GAME.NET; both
 * dead). On the GSC-GAME.NET path (connection button 4) the "lobby" is a single call
 * into IChat.dll: Process_GSC_ChatWindow(true, RoomInfo*). We answer it from the
 * Steam lobby: the lobby owner creates the game room (return 1), members join the
 * owner at 10.77.0.<owner slot> (return 2). Everything else in IChat is a no-op.
 *
 * The exports keep the original MSVC-mangled names and ordinals (ichat.def maps
 * them to the btw_* functions below). All are __cdecl free functions, so the caller
 * cleans the stack and a no-op does not need the exact parameter list.
 *
 * Also here: the patch that makes the GSC-GAME.NET login (FUN_00561d80) succeed.
 */
#include "btwsteam.h"
#include <stdio.h>
#include <string.h>

/* RoomInfo offsets used by the game after Process_GSC_ChatWindow returns */
#define RI_ROOM_NAME   0x00   /* <= 27 chars + NUL (the transport keeps 28) */
#define RI_NICK        0x80   /* <= 31 chars + NUL, must not start with '[' (clan check) */
#define RI_HOST_IP     0xc0   /* joiners: host IP string, port is always 34000 */
#define RI_MAX_PLAYERS 0x124  /* u16 */

#define LOBBY_RESULT_CANCEL 0
#define LOBBY_RESULT_CREATE 1
#define LOBBY_RESULT_JOIN   2

/* ---- the login unlock ------------------------------------------------------ */

/* VA 0x55264e in dmcr.exe 1.35: `call FUN_00561d80` (runs Internet/cash/login.cmd,
 * which loads LogDisabled.cml and returns false). Replace with `mov al,1` + nops. */
#define LOGIN_CALL_VA  0x0055264eu
#define EXE_IMAGE_BASE 0x00400000u

void btw_patch_login(void)
{
    static const BYTE expect[5] = { 0xE8, 0x2D, 0xF7, 0x00, 0x00 };
    static const BYTE patch[5]  = { 0xB0, 0x01, 0x90, 0x90, 0x90 };

    if (btw_env_flag("BTWSTEAM_NOPATCH")) {
        btw_log("patch: BTWSTEAM_NOPATCH set, GSC-GAME.NET login left as is");
        return;
    }
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    DWORD rva = LOGIN_CALL_VA - EXE_IMAGE_BASE; /* the exe has relocations: go by RVA */
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.SizeOfImage < rva + sizeof patch) {
        btw_log("patch: main module too small for the login patch (not dmcr.exe 1.35?); skipped");
        return;
    }
    BYTE *p = base + rva;
    if (memcmp(p, patch, sizeof patch) == 0) {
        btw_log("patch: login call at %p already patched", (void *)p);
        return;
    }
    if (memcmp(p, expect, sizeof expect) != 0) {
        btw_log("patch: bytes at %p are %02X %02X %02X %02X %02X, expected E8 2D F7 00 00 "
                "(different dmcr.exe?); NOT patched", (void *)p, p[0], p[1], p[2], p[3], p[4]);
        return;
    }
    DWORD old;
    if (!VirtualProtect(p, sizeof patch, PAGE_EXECUTE_READWRITE, &old)) {
        btw_log("patch: VirtualProtect failed (%lu); NOT patched", (unsigned long)GetLastError());
        return;
    }
    memcpy(p, patch, sizeof patch);
    VirtualProtect(p, sizeof patch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof patch);
    btw_log("patch: GSC-GAME.NET login unlocked (0x%08X: call -> mov al,1)", LOGIN_CALL_VA);
}

/* ---- Process_GSC_ChatWindow ------------------------------------------------- */

/* The in-game nick: BTWSTEAM_NICK, else the Steam persona name, else Player<slot>.
 * Both names go through btw_nick_sanitize (nick.c): Latin letters only (Cyrillic is
 * transliterated), printable ASCII, <= 31 chars, no leading '[' (clan check). */
static void make_nick(int slot, char out[BTW_NICK_MAX + 1])
{
    char name[512];
    const char *src = "BTWSTEAM_NICK";
    if (!btw_nick_sanitize(btw_env_utf8("BTWSTEAM_NICK", name, sizeof name), out, BTW_NICK_MAX + 1)) {
        src = "Steam name";
        btw_session_persona(name, sizeof name);
        if (!btw_nick_sanitize(name, out, BTW_NICK_MAX + 1)) {
            src = "default";
            snprintf(out, BTW_NICK_MAX + 1, "Player%d", slot);
        }
    }
    btw_log("ichat: nick \"%s\" (%s)", out, src);
}

/* Pump the game window's messages while we wait, so it stays responsive.
 * Returns 0 if the user pressed Escape or the app is quitting. */
static int pump_and_check(void)
{
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) {
            PostQuitMessage((int)m.wParam);
            return 0;
        }
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    /* 0x8000: down now; 0x0001: pressed since the previous call */
    return !(GetAsyncKeyState(VK_ESCAPE) & 0x8001);
}

static int lobby_window(unsigned char *ri)
{
    btw_lobby_state_t st;
    DWORD t0 = GetTickCount(), last_note = t0;
    int asked_host_ready = 0;

    GetAsyncKeyState(VK_ESCAPE); /* clear the "pressed since" bit */
    btw_log("ichat: Process_GSC_ChatWindow(true): waiting for the Steam lobby (Esc cancels)");
    for (;;) {
        btw_session_get(&st);
        if (st.steam_state < 0) {
            btw_log("ichat: no Steam session; lobby window cancelled (returns 0)");
            return LOBBY_RESULT_CANCEL;
        }
        if (st.lobby && st.own_slot) {
            if (st.is_owner) {
                char nick[BTW_NICK_MAX + 1], room[28];
                make_nick(st.own_slot, nick);
                int nlen = (int)strlen(nick);
                if (nlen > 19) /* "<nick>'s game" <= 26 chars */
                    nlen = 19;
                while (nlen > 0 && nick[nlen - 1] == ' ')
                    nlen--;
                snprintf(room, sizeof room, "%.*s's game", nlen, nick);
                memset(ri + RI_ROOM_NAME, 0, sizeof room);
                memcpy(ri + RI_ROOM_NAME, room, strlen(room) + 1);
                memset(ri + RI_NICK, 0, 32);
                memcpy(ri + RI_NICK, nick, strlen(nick) + 1);
                uint16_t maxp = BTW_LOBBY_MAX_MEMBERS;
                memcpy(ri + RI_MAX_PLAYERS, &maxp, sizeof maxp);
                /* the game creates the room in this same frame; joiners may come now */
                btw_session_request_host_ready();
                btw_log("ichat: we own lobby %llu: CREATE room \"%s\", nick \"%s\", max %u, "
                        "address 10.77.0.%d (returns 1)", (unsigned long long)st.lobby, room, nick,
                        maxp, st.own_slot);
                return LOBBY_RESULT_CREATE;
            }
            if (st.owner_slot && st.host_ready) {
                char nick[BTW_NICK_MAX + 1], host[20];
                make_nick(st.own_slot, nick);
                snprintf(host, sizeof host, "10.77.0.%d", st.owner_slot);
                memset(ri + RI_NICK, 0, 32);
                memcpy(ri + RI_NICK, nick, strlen(nick) + 1);
                memset(ri + RI_HOST_IP, 0, sizeof host);
                memcpy(ri + RI_HOST_IP, host, strlen(host) + 1);
                uint16_t maxp = BTW_LOBBY_MAX_MEMBERS;
                memcpy(ri + RI_MAX_PLAYERS, &maxp, sizeof maxp);
                btw_log("ichat: member of lobby %llu: JOIN host %s:%d as \"%s\" (we are 10.77.0.%d, "
                        "returns 2)", (unsigned long long)st.lobby, host, BTW_GAME_PORT, nick,
                        st.own_slot);
                return LOBBY_RESULT_JOIN;
            }
            if (!asked_host_ready) {
                asked_host_ready = 1;
                btw_log("ichat: member of lobby %llu (slot %d), waiting for the host's room "
                        "(host_ready=1, host slot %d)", (unsigned long long)st.lobby, st.own_slot,
                        st.owner_slot);
            }
        }
        if (!pump_and_check()) {
            btw_log("ichat: lobby window cancelled by the user after %lu ms (returns 0)",
                    (unsigned long)(GetTickCount() - t0));
            return LOBBY_RESULT_CANCEL;
        }
        if (GetTickCount() - last_note >= 5000) {
            last_note = GetTickCount();
            btw_log("ichat: still waiting: steam %d, lobby %llu, own slot %d, owner %d, host slot %d, "
                    "host_ready %d", st.steam_state, (unsigned long long)st.lobby, st.own_slot,
                    st.is_owner, st.owner_slot, st.host_ready);
        }
        Sleep(10);
    }
}

/* ?Process_GSC_ChatWindow@@YAH_NPAURoomInfo@@@Z: int __cdecl (bool, RoomInfo *) */
int btw_Process_GSC_ChatWindow(int first_raw, void *room_info)
{
    /* MSVC passes bool in the low byte of a 4-byte slot; ignore the upper bytes */
    int first = (first_raw & 0xFF) != 0;
    if (!first) {
        btw_log("ichat: Process_GSC_ChatWindow(false) (chat button in the room): returns 0");
        return LOBBY_RESULT_CANCEL;
    }
    if (!room_info) {
        btw_log("ichat: Process_GSC_ChatWindow(true, NULL)?: returns 0");
        return LOBBY_RESULT_CANCEL;
    }
    return lobby_window((unsigned char *)room_info);
}

/* ---- the no-ops --------------------------------------------------------------- */

#define ONCE(name)                                                                            \
    do {                                                                                      \
        static volatile LONG done;                                                            \
        if (!InterlockedExchange(&done, 1))                                                   \
            btw_log("ichat: " name " called (no-op; logged once)");                           \
    } while (0)

void btw_ChatProcess(void)        { ONCE("ChatProcess"); }          /* every room frame */
void btw_GoHomeAnyway(void)       { ONCE("GoHomeAnyway"); }         /* each Multi Player menu */
void btw_LeaveGSCRoom(void)       { ONCE("LeaveGSCRoom"); }
void btw_ReportAliveState(int a, int *b)                  { (void)a; (void)b; ONCE("ReportAliveState"); }
void btw_ReportGSCGame(int a, int b, void *report)        { (void)a; (void)b; (void)report; ONCE("ReportGSCGame"); }
void btw_SendPrivateMessage(char *to, char *text)         { (void)to; (void)text; ONCE("SendPrivateMessage"); }
void btw_SendVictoryState(int a, unsigned char b)         { (void)a; (void)b; ONCE("SendVictoryState"); }
void btw_SetChatWState(int a, int b)                      { (void)a; (void)b; ONCE("SetChatWState"); }
void btw_SetGameID(char *id)                              { (void)id; ONCE("SetGameID"); }

void btw_GetGameID(char *id)
{
    if (id)
        id[0] = 0;
    ONCE("GetGameID");
}

/* (char *, char *, int, int *, char **, int *, int *) per the mangled name */
void btw_StartGSCGame(char *a, char *b, int c, int *d, char **e, int *f, int *g)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    btw_log("ichat: StartGSCGame (game starts; no-op)");
}
