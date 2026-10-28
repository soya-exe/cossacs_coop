/*
 * The Steam worker thread (DESIGN.md "Steam session").
 *
 * Every call into steam_api.dll happens on this thread. Every 10 ms (or right away
 * when the game queues a datagram) it:
 *   1. runs manual callback dispatch (lobby events, invites, session requests),
 *   2. drains channel 0 (game data) and channel 1 (control) into peers.c,
 *   3. sends keep-alives / drops silent peers (peers.c),
 *   4. sends everything the game queued.
 *
 * Lobby: whoever creates it is the host. The host gives each member a slot in lobby
 * data (`slot_<steamid64>` = N, the host prefers 1; existing slots never move) and
 * sets `btwsteam=1`, `ver=1`. Everyone, host included, reads the slot table back
 * from lobby data. Joining: `+connect_lobby <id>` / `-steamjoin <id>` on the
 * command line, env BTWSTEAM_JOIN=<id>, or an invite / "Join game" while running
 * (callbacks 333 / 337). Otherwise we create a friends-only lobby at startup.
 */
#include "btwsteam.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* callback ids */
enum {
    CB_GAME_LOBBY_JOIN_REQUESTED = 333,
    CB_GAME_RICH_PRESENCE_JOIN_REQUESTED = 337,
    CB_LOBBY_ENTER = 504,
    CB_LOBBY_DATA_UPDATE = 505,
    CB_LOBBY_CHAT_UPDATE = 506,
    CB_LOBBY_CREATED = 513,
    CB_API_CALL_COMPLETED = 703,
    CB_NM_SESSION_REQUEST = 1251,
    CB_NM_SESSION_FAILED = 1252,
};

#define LOBBY_TYPE_FRIENDS_ONLY 1
#define LOBBY_MAX_MEMBERS BTW_LOBBY_MAX_MEMBERS
#define ERESULT_OK 1
#define SEND_UNRELIABLE 0
#define SEND_NO_NAGLE 1
#define SEND_RELIABLE 8
#define SEND_AUTO_RESTART_BROKEN_SESSION 32
#define CFG_SEND_BUFFER_SIZE 9
#define CFG_RECV_BUFFER_SIZE 47

static HANDLE g_thread;
static volatile LONG g_stop;
static int g_steam_ok;

static void *g_user, *g_mm, *g_fr, *g_nm, *g_nu;
static int32_t g_pipe;
static uint64_t g_self;
static uint64_t g_lobby;        /* lobby we are in (0 = none) */
static uint64_t g_joining;      /* JoinLobby in flight */
static int g_creating;          /* CreateLobby in flight */
/* Call handles we issued (CreateLobby / JoinLobby). Only their call results (703) are
 * fetched; Steam's own calls (e.g. callbacks 1296, 2101 on real Steam) are only counted. */
static uint64_t g_create_call, g_join_call;
static char g_persona[128];     /* Steam persona name (UTF-8), under btw_lock */
static int g_host_setup_done;   /* lobby data / rich presence set for g_lobby */
static int g_invite_done;
static DWORD g_retry_create_at; /* CreateLobby failed: try again at this tick */
static int g_sender_off;        /* SteamNetworkingMessage_t sender identity: 12 or 16 */

static uint64_t g_prev_members[BTW_MAX_PEERS + 1];
static int g_nprev;

static btw_lobby_state_t g_state;  /* published to the game thread, under btw_lock */
static volatile LONG g_req_host_ready;

typedef struct { uint64_t id; DWORD since; } pending_t;
static pending_t g_pending[16]; /* session requests from not-yet-members */

/* ------------------------------------------------------------------------ */

static void ident(btw_ident_t *out, uint64_t id)
{
    memset(out, 0, sizeof *out);
    S.Ident_SetSteamID64(out, id);
}

static void slot_key(char *buf, size_t cap, uint64_t id)
{
    snprintf(buf, cap, "slot_%llu", (unsigned long long)id);
}

static int lobby_slot(uint64_t id)
{
    char key[40];
    slot_key(key, sizeof key, id);
    const char *v = S.MM_GetLobbyData(g_mm, g_lobby, key);
    int s = v ? atoi(v) : 0;
    return s >= 1 && s <= BTW_MAX_SLOT ? s : 0;
}

static int lobby_members(uint64_t *ids, int cap)
{
    int n = S.MM_GetNumLobbyMembers(g_mm, g_lobby);
    if (n > cap)
        n = cap;
    for (int i = 0; i < n; i++)
        ids[i] = S.MM_GetLobbyMemberByIndex(g_mm, g_lobby, i);
    return n < 0 ? 0 : n;
}

static int is_lobby_member(uint64_t id)
{
    uint64_t ids[BTW_MAX_PEERS + 1];
    int n = g_lobby ? lobby_members(ids, BTW_MAX_PEERS + 1) : 0;
    for (int i = 0; i < n; i++)
        if (ids[i] == id)
            return 1;
    return 0;
}

static void set_rich_presence(void)
{
    char connect[64];
    snprintf(connect, sizeof connect, "+connect_lobby %llu", (unsigned long long)g_lobby);
    S.FR_SetRichPresence(g_fr, "connect", connect);
    S.FR_SetRichPresence(g_fr, "status", "Cossacks LAN over Steam");
}

/* Host only: give every member a slot. Slots that exist stay where they are (a
 * slot change mid-game would change a player's IP); new members get the lowest
 * free one, the owner prefers 1. Departed members' keys are cleared. */
static void host_assign_slots(const uint64_t *ids, int n)
{
    int existing[BTW_MAX_PEERS + 1], assigned[BTW_MAX_PEERS + 1];
    unsigned char used[BTW_MAX_SLOT + 1];
    memset(used, 0, sizeof used);

    for (int i = 0; i < n; i++) {
        existing[i] = lobby_slot(ids[i]);
        assigned[i] = 0;
        if (existing[i] && !used[existing[i]]) {
            assigned[i] = existing[i];
            used[existing[i]] = 1;
        }
    }
    for (int i = 0; i < n; i++)
        if (!assigned[i] && ids[i] == g_self && !used[1]) {
            assigned[i] = 1;
            used[1] = 1;
        }
    for (int i = 0; i < n; i++) {
        if (assigned[i])
            continue;
        for (int s = 2; s <= BTW_MAX_SLOT && !assigned[i]; s++)
            if (!used[s]) {
                assigned[i] = s;
                used[s] = 1;
            }
        if (!assigned[i] && !used[1]) {
            assigned[i] = 1;
            used[1] = 1;
        }
    }
    for (int i = 0; i < n; i++) {
        if (assigned[i] == existing[i])
            continue;
        char key[40], val[8];
        slot_key(key, sizeof key, ids[i]);
        snprintf(val, sizeof val, "%d", assigned[i]);
        bool ok = S.MM_SetLobbyData(g_mm, g_lobby, key, val);
        btw_log("host: %s = %s%s", key, val, ok ? "" : " (SetLobbyData failed)");
    }
    /* free the slots of members who left */
    for (int j = 0; j < g_nprev; j++) {
        int still = 0;
        for (int i = 0; i < n; i++)
            still |= ids[i] == g_prev_members[j];
        if (!still) {
            char key[40];
            slot_key(key, sizeof key, g_prev_members[j]);
            S.MM_SetLobbyData(g_mm, g_lobby, key, "");
            btw_log("host: %s cleared (member left)", key);
        }
    }
}

static void accept_pending(void)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < 16; i++) {
        if (!g_pending[i].id)
            continue;
        if (is_lobby_member(g_pending[i].id)) {
            btw_ident_t who;
            ident(&who, g_pending[i].id);
            S.NM_AcceptSessionWithUser(g_nm, &who);
            btw_log("steam: accepted held session from %llu (now a lobby member)",
                    (unsigned long long)g_pending[i].id);
            g_pending[i].id = 0;
        } else if (now - g_pending[i].since > 15000) {
            btw_log("steam: session request from %llu ignored (never joined our lobby)",
                    (unsigned long long)g_pending[i].id);
            g_pending[i].id = 0;
        }
    }
}

/* Re-read the lobby: host assigns slots, then everyone reads the table back. */
static void refresh_lobby(void)
{
    if (!g_lobby)
        return;
    uint64_t ids[BTW_MAX_PEERS + 1];
    int slots[BTW_MAX_PEERS + 1];
    int n = lobby_members(ids, BTW_MAX_PEERS + 1);
    if (n <= 0)
        return; /* member list not there yet; a later callback brings it */
    uint64_t owner = S.MM_GetLobbyOwner(g_mm, g_lobby);
    if (owner == g_self)
        host_assign_slots(ids, n);
    int own_slot = 0, owner_slot = 0;
    for (int i = 0; i < n; i++) {
        slots[i] = lobby_slot(ids[i]);
        if (ids[i] == g_self)
            own_slot = slots[i];
        if (ids[i] == owner)
            owner_slot = slots[i];
    }
    const char *hr = S.MM_GetLobbyData(g_mm, g_lobby, "host_ready");
    int host_ready = hr && strcmp(hr, "1") == 0;
    btw_peers_update(ids, slots, n);

    btw_lock();
    if (host_ready && !g_state.host_ready)
        btw_log("steam: lobby %llu: host_ready=1 (the host's game room exists)",
                (unsigned long long)g_lobby);
    g_state.lobby = g_lobby;
    g_state.is_owner = owner == g_self;
    g_state.own_slot = own_slot;
    g_state.owner_slot = owner_slot;
    g_state.host_ready = host_ready;
    btw_unlock();
    memcpy(g_prev_members, ids, sizeof(uint64_t) * (size_t)n);
    g_nprev = n;
    accept_pending();
}

static void create_lobby(void)
{
    if (g_creating)
        return;
    uint64_t call = S.MM_CreateLobby(g_mm, LOBBY_TYPE_FRIENDS_ONLY, LOBBY_MAX_MEMBERS);
    g_creating = call != 0;
    g_create_call = call;
    btw_log("steam: CreateLobby(friends-only, %d) call %llu%s", LOBBY_MAX_MEMBERS,
            (unsigned long long)call, call ? "" : " FAILED");
    if (!call)
        g_retry_create_at = GetTickCount() + 10000;
}

static void join_lobby(uint64_t id)
{
    g_joining = id;
    uint64_t call = S.MM_JoinLobby(g_mm, id);
    g_join_call = call;
    btw_log("steam: JoinLobby(%llu) call %llu", (unsigned long long)id, (unsigned long long)call);
}

static void flush_out(void);

static void leave_lobby(void)
{
    if (!g_lobby)
        return;
    btw_peers_reset(1);
    flush_out(); /* the goodbyes */
    S.MM_LeaveLobby(g_mm, g_lobby);
    if (S.FR_ClearRichPresence)
        S.FR_ClearRichPresence(g_fr);
    btw_log("steam: left lobby %llu", (unsigned long long)g_lobby);
    btw_lock();
    g_state.lobby = 0;
    g_state.is_owner = g_state.own_slot = g_state.owner_slot = g_state.host_ready = 0;
    btw_unlock();
    g_lobby = 0;
    g_host_setup_done = 0;
    g_nprev = 0;
}

static void switch_lobby(uint64_t target, const char *why)
{
    if (!target || target == g_lobby || target == g_joining)
        return;
    btw_log("steam: switching to lobby %llu (%s)", (unsigned long long)target, why);
    leave_lobby();
    join_lobby(target);
}

/* Lobby ids come as `+connect_lobby <id>` (Steam launch / rich presence connect),
 * `-steamjoin <id>` or BTWSTEAM_JOIN=<id>. */
static uint64_t parse_lobby_after(const char *s, const char *tag)
{
    const char *p = s ? strstr(s, tag) : NULL;
    if (!p)
        return 0;
    p += strlen(tag);
    while (*p == ' ' || *p == '\t' || *p == '"' || *p == '=')
        p++;
    return strtoull(p, NULL, 10);
}

static uint64_t startup_join_target(void)
{
    char env[64];
    const char *cmd = GetCommandLineA();
    uint64_t id = parse_lobby_after(cmd, "+connect_lobby");
    if (!id)
        id = parse_lobby_after(cmd, "-steamjoin");
    if (!id)
        id = strtoull(btw_env("BTWSTEAM_JOIN", env, sizeof env), NULL, 10);
    return id;
}

/* ------------------------------------------------------------------------ */

static void host_setup(void)
{
    if (g_host_setup_done)
        return;
    g_host_setup_done = 1;
    S.MM_SetLobbyData(g_mm, g_lobby, "btwsteam", "1");
    S.MM_SetLobbyData(g_mm, g_lobby, "ver", "1");
    S.MM_SetLobbyJoinable(g_mm, g_lobby, true);
    set_rich_presence();
    btw_log("LOBBY CREATED %llu (we host, slot 1). Friends: \"Join game\" in Steam, an invite, or "
            "-steamjoin %llu", (unsigned long long)g_lobby, (unsigned long long)g_lobby);

    if (!g_invite_done && btw_env_flag("BTWSTEAM_INVITE") && S.FR_ActivateGameOverlayInviteDialog) {
        S.FR_ActivateGameOverlayInviteDialog(g_fr, g_lobby);
        btw_log("steam: overlay invite dialog requested (BTWSTEAM_INVITE=1)");
    }
    char ids[512];
    btw_env("BTWSTEAM_INVITE_IDS", ids, sizeof ids);
    if (!g_invite_done && ids[0] && S.MM_InviteUserToLobby) {
        for (char *p = ids; *p;) {
            char *end;
            uint64_t friend_id = strtoull(p, &end, 10);
            if (end == p) {
                p++;
                continue;
            }
            bool ok = S.MM_InviteUserToLobby(g_mm, g_lobby, friend_id);
            btw_log("steam: InviteUserToLobby(%llu) %s", (unsigned long long)friend_id, ok ? "ok" : "failed");
            p = end;
        }
    }
    g_invite_done = 1;
}

static void on_lobby_created(int32_t result, uint64_t lobby)
{
    g_creating = 0;
    if (result != ERESULT_OK || !lobby) {
        btw_log("steam: CreateLobby failed, EResult %d; retrying in 10 s", (int)result);
        g_retry_create_at = GetTickCount() + 10000;
        return;
    }
    /* real Steam reports this twice (call result 703 and broadcast 513) */
    if (g_lobby && g_lobby != lobby) {
        btw_log("steam: LobbyCreated %llu while in %llu; ignored", (unsigned long long)lobby,
                (unsigned long long)g_lobby);
        return;
    }
    g_lobby = lobby;
    host_setup();
    refresh_lobby();
}

static void on_lobby_enter(uint64_t lobby, uint32_t response)
{
    static const char *const k_resp[] = { "?", "success", "lobby does not exist", "not allowed",
                                          "full", "error", "banned", "limited user",
                                          "clan disabled", "community ban", "member blocked you",
                                          "you blocked member", "rate limited" };
    if (response != 1) {
        btw_log("steam: entering lobby %llu failed: %s (%u)", (unsigned long long)lobby,
                response < sizeof k_resp / sizeof k_resp[0] ? k_resp[response] : "?", response);
        if (lobby == g_joining || !g_joining) {
            g_joining = 0;
            if (!g_lobby) {
                btw_log("steam: creating our own lobby instead");
                create_lobby();
            }
        }
        return;
    }
    if (lobby == g_lobby) { /* creator's own enter, or a repeat */
        refresh_lobby();
        return;
    }
    uint64_t owner = S.MM_GetLobbyOwner(g_mm, lobby);
    if (g_creating && owner == g_self && lobby != g_joining) {
        /* LobbyEnter for the lobby we just created arrived before LobbyCreated */
        on_lobby_created(ERESULT_OK, lobby);
        return;
    }
    if (g_lobby)
        leave_lobby();
    g_lobby = lobby;
    g_joining = 0;
    const char *tag = S.MM_GetLobbyData(g_mm, lobby, "btwsteam");
    const char *ver = S.MM_GetLobbyData(g_mm, lobby, "ver");
    btw_log("LOBBY JOINED %llu, owner %llu, %d member(s), btwsteam=%s ver=%s",
            (unsigned long long)lobby, (unsigned long long)owner,
            S.MM_GetNumLobbyMembers(g_mm, lobby), tag && *tag ? tag : "(none)",
            ver && *ver ? ver : "(none)");
    if (!tag || strcmp(tag, "1") != 0)
        btw_log("steam: WARNING lobby %llu has no btwsteam=1 key: not a Cossacks btwsteam game "
                "(or the host has not set it yet)", (unsigned long long)lobby);
    set_rich_presence();
    refresh_lobby();
}

static void on_session_request(uint64_t id)
{
    btw_ident_t who;
    if (is_lobby_member(id)) {
        ident(&who, id);
        bool ok = S.NM_AcceptSessionWithUser(g_nm, &who);
        btw_log("steam: session request from %llu accepted%s", (unsigned long long)id, ok ? "" : " (failed)");
        return;
    }
    /* Lobby membership can lag behind the first packet: hold it for a while. */
    for (int i = 0; i < 16; i++)
        if (g_pending[i].id == id)
            return;
    for (int i = 0; i < 16; i++)
        if (!g_pending[i].id) {
            g_pending[i].id = id;
            g_pending[i].since = GetTickCount();
            break;
        }
    btw_log("steam: session request from %llu, not a member of our lobby: held", (unsigned long long)id);
}

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* Callback structs, Windows pack(8) layout. */
static void handle_callback(int id, const uint8_t *p, int size)
{
    switch (id) {
    case CB_LOBBY_CREATED: /* { int32 result; uint64 lobby @8 } */
        if (size >= 16)
            on_lobby_created((int32_t)rd32(p), rd64(p + 8));
        break;
    case CB_LOBBY_ENTER: /* { uint64 lobby; uint32 perms; bool locked; uint32 response @16 } */
        if (size >= 20)
            on_lobby_enter(rd64(p), rd32(p + 16));
        break;
    case CB_LOBBY_DATA_UPDATE: /* { uint64 lobby; uint64 member; uint8 success } */
        if (size >= 16 && rd64(p) == g_lobby)
            refresh_lobby();
        break;
    case CB_LOBBY_CHAT_UPDATE: /* { uint64 lobby; uint64 changed; uint64 by; uint32 state @24 } */
        if (size >= 28 && rd64(p) == g_lobby) {
            uint32_t st = rd32(p + 24);
            btw_log("steam: lobby member %llu %s%s%s%s%s (0x%x)", (unsigned long long)rd64(p + 8),
                    st & 1 ? "entered " : "", st & 2 ? "left " : "", st & 4 ? "disconnected " : "",
                    st & 8 ? "kicked " : "", st & 16 ? "banned " : "", st);
            refresh_lobby();
        }
        break;
    case CB_GAME_LOBBY_JOIN_REQUESTED: /* { uint64 lobby; uint64 friend } */
        if (size >= 16) {
            btw_log("steam: invite accepted (friend %llu)", (unsigned long long)rd64(p + 8));
            switch_lobby(rd64(p), "invite accepted");
        }
        break;
    case CB_GAME_RICH_PRESENCE_JOIN_REQUESTED: /* { uint64 friend; char connect[256] @8 } */
        if (size >= 8 + 256) {
            char connect[257];
            memcpy(connect, p + 8, 256);
            connect[256] = 0;
            btw_log("steam: \"Join game\" via friend %llu: %s", (unsigned long long)rd64(p), connect);
            uint64_t lobby = parse_lobby_after(connect, "+connect_lobby");
            if (lobby)
                switch_lobby(lobby, "join game");
        }
        break;
    case CB_NM_SESSION_REQUEST: /* { SteamNetworkingIdentity remote @0 } */
        on_session_request(S.Ident_GetSteamID64(p));
        break;
    case CB_NM_SESSION_FAILED: /* { SteamNetConnectionInfo_t info; identity @0 } */
        btw_log("steam: session with %llu failed", (unsigned long long)S.Ident_GetSteamID64(p));
        break;
    default:
        break;
    }
}

static void dispatch(void)
{
    btw_cbmsg_t m;
    S.MD_RunFrame(g_pipe);
    while (S.MD_GetNextCallback(g_pipe, &m)) {
        if (m.callback == CB_API_CALL_COMPLETED && m.param && m.param_size >= 16) {
            /* SteamAPICallCompleted_t { uint64 call; int32 callback; uint32 size } */
            uint64_t call = rd64(m.param);
            int cb = (int)rd32(m.param + 8);
            uint32_t sz = rd32(m.param + 12);
            static uint64_t buf[1024 / 8]; /* 8-aligned */
            bool failed = false;
            if (!call || (call != g_create_call && call != g_join_call)) {
                BTW_INC(foreign_results); /* Steam's own call: not ours to fetch or log */
            } else {
                if (call == g_create_call)
                    g_create_call = 0;
                else
                    g_join_call = 0;
                if (sz <= sizeof buf &&
                    S.MD_GetAPICallResult(g_pipe, call, buf, (int)sz, cb, &failed) && !failed)
                    handle_callback(cb, (const uint8_t *)buf, (int)sz);
                else
                    btw_log("steam: call result %llu (callback %d, %u bytes) failed",
                            (unsigned long long)call, cb, sz);
            }
        } else if (m.param) {
            handle_callback(m.callback, m.param, m.param_size);
        }
        S.MD_FreeLastCallback(g_pipe);
    }
}

/* SteamNetworkingMessage_t (32-bit): data @0, size @4, sender identity @12 in the
 * real steam_api.dll (16 if you trust the header packing; a mock may use 16). Read
 * both, keep whichever decodes to a SteamID, and log the winner. */
static uint64_t msg_sender(const uint8_t *m)
{
    uint64_t a = S.Ident_GetSteamID64(m + 12), b = S.Ident_GetSteamID64(m + 16);
    int off = a ? 12 : b ? 16 : 0;
    if (off && off != g_sender_off) {
        btw_log("steam: message sender identity found at offset %d", off);
        g_sender_off = off;
    }
    return off == 12 ? a : b;
}

static void receive(void)
{
    for (int ch = BTW_CH_DATA; ch <= BTW_CH_CTRL; ch++) {
        void *msgs[64];
        int n;
        do {
            n = S.NM_ReceiveMessagesOnChannel(g_nm, ch, msgs, 64);
            for (int i = 0; i < n; i++) {
                const uint8_t *m = msgs[i];
                const uint8_t *data;
                int32_t size;
                memcpy(&data, m, sizeof data);
                memcpy(&size, m + 4, sizeof size);
                uint64_t from = msg_sender(m);
                if (from && data && size >= 0)
                    btw_peers_on_message(from, ch, data, size);
                else
                    BTW_INC(rx_drop_bad);
                S.Msg_Release(msgs[i]);
            }
        } while (n == 64);
    }
}

static void flush_out(void)
{
    static int nerr_logged;
    btw_out_t *m = btw_outq_take();
    while (m) {
        btw_out_t *next = m->next;
        btw_ident_t to;
        ident(&to, m->to);
        if (m->channel < 0) {
            S.NM_CloseSessionWithUser(g_nm, &to);
        } else {
            int flags = SEND_NO_NAGLE | SEND_AUTO_RESTART_BROKEN_SESSION |
                        (m->channel == BTW_CH_CTRL ? SEND_RELIABLE : SEND_UNRELIABLE);
            int r = S.NM_SendMessageToUser(g_nm, &to, m->data, (uint32_t)m->len, flags, m->channel);
            if (r != ERESULT_OK) {
                BTW_INC(tx_steam_err);
                if (nerr_logged < 20 || (g_ctr.tx_steam_err % 1000) == 0) {
                    nerr_logged++;
                    btw_log("steam: SendMessageToUser(%llu, ch %d, %d bytes) = EResult %d",
                            (unsigned long long)m->to, m->channel, m->len, r);
                }
            }
        }
        free(m);
        m = next;
    }
}

static void discard_out(void)
{
    btw_out_t *m = btw_outq_take();
    while (m) {
        btw_out_t *next = m->next;
        free(m);
        m = next;
    }
}

/* ------------------------------------------------------------------------ */

static void ensure_appid_file(void)
{
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%ssteam_appid.txt", btw_exe_dir());
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        return;
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        btw_log("steam: cannot write %s (error %lu)", path, (unsigned long)GetLastError());
        return;
    }
    DWORD w;
    WriteFile(h, "4850", 4, &w, NULL);
    CloseHandle(h);
    btw_log("steam: wrote %s (4850)", path);
}

/* The Steam name, for the in-game nick (ichat.c make_nick). */
static void read_persona(void)
{
    const char *p = S.FR_GetPersonaName ? S.FR_GetPersonaName(g_fr) : NULL;
    char nick[BTW_NICK_MAX + 1];
    btw_lock();
    snprintf(g_persona, sizeof g_persona, "%s", p ? p : "");
    btw_unlock();
    btw_nick_sanitize(p, nick, sizeof nick);
    btw_log("steam: persona name \"%s\" (in game: \"%s\")", p ? p : "(GetPersonaName missing)",
            nick[0] ? nick : "nothing usable, Player<slot>");
}

static int steam_start(void)
{
    if (btw_env_flag("BTWSTEAM_NOSTEAM")) {
        btw_log("steam: BTWSTEAM_NOSTEAM set, running without Steam");
        return 0;
    }
    ensure_appid_file();
    char appid[32];
    if (btw_env("BTWSTEAM_APPID", appid, sizeof appid)[0]) {
        /* SteamAppId in the environment beats steam_appid.txt (e.g. 480 Spacewar) */
        SetEnvironmentVariableA("SteamAppId", appid);
        SetEnvironmentVariableA("SteamGameId", appid);
        btw_log("steam: BTWSTEAM_APPID=%s", appid);
    }
    if (btw_sapi_load(btw_exe_dir()) != 0)
        return 0;
    if (S.IsSteamRunning)
        btw_log("steam: IsSteamRunning = %d", (int)S.IsSteamRunning());

    char err[1024];
    memset(err, 0, sizeof err);
    int r = S.InitFlat(err);
    if (r != 0) {
        static const char *const k_init[] = { "ok", "failed", "Steam not running", "version mismatch" };
        btw_log("steam: SteamAPI_InitFlat failed: %d (%s) \"%s\"; playing alone on the virtual LAN", r,
                r >= 0 && r <= 3 ? k_init[r] : "?", err);
        return 0;
    }
    S.MD_Init();
    g_pipe = S.GetHSteamPipe();
    g_user = S.SteamUser();
    g_mm = S.SteamMatchmaking();
    g_fr = S.SteamFriends();
    g_nm = S.SteamNetworkingMessages();
    g_nu = S.SteamNetworkingUtils ? S.SteamNetworkingUtils() : NULL;
    if (!g_user || !g_mm || !g_fr || !g_nm) {
        btw_log("steam: missing interface (user %p, matchmaking %p, friends %p, netmsgs %p)", g_user,
                g_mm, g_fr, g_nm);
        S.Shutdown();
        return 0;
    }
    g_self = S.User_GetSteamID(g_user);
    btw_peers_set_self(g_self);
    btw_log("steam: initialised, SteamID %llu, pipe %d", (unsigned long long)g_self, (int)g_pipe);
    read_persona();
    if (g_nu && S.NU_SetGlobalConfigValueInt32) {
        S.NU_SetGlobalConfigValueInt32(g_nu, CFG_SEND_BUFFER_SIZE, 4 * 1024 * 1024);
        S.NU_SetGlobalConfigValueInt32(g_nu, CFG_RECV_BUFFER_SIZE, 4 * 1024 * 1024);
    }

    uint64_t target = startup_join_target();
    if (target) {
        btw_log("steam: joining lobby %llu from the command line / BTWSTEAM_JOIN",
                (unsigned long long)target);
        join_lobby(target);
    } else {
        create_lobby();
    }
    return 1;
}

/* The game's player identity (FUN_00590f80); two copies sharing it collide. */
static void log_ccuid(void)
{
    HKEY k;
    char v[64];
    DWORD type = 0, len = sizeof v - 1;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\GSC Game World", 0, KEY_READ, &k) != ERROR_SUCCESS) {
        btw_log("game: no HKCU\\Software\\GSC Game World key yet (CCUID will be created)");
        return;
    }
    memset(v, 0, sizeof v);
    if (RegQueryValueExA(k, "CCUID", NULL, &type, (BYTE *)v, &len) == ERROR_SUCCESS && type == REG_SZ)
        btw_log("game: CCUID \"%s\"", v);
    else
        btw_log("game: no CCUID value yet");
    RegCloseKey(k);
}

void btw_session_get(btw_lobby_state_t *out)
{
    btw_lock();
    *out = g_state;
    btw_unlock();
}

void btw_session_persona(char *out, size_t cap)
{
    btw_lock();
    snprintf(out, cap, "%s", g_persona);
    btw_unlock();
}

void btw_session_request_host_ready(void)
{
    InterlockedExchange(&g_req_host_ready, 1);
    SetEvent(g_wake_event);
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    btw_log("worker: started");
    log_ccuid();
    g_steam_ok = steam_start();
    btw_lock();
    g_state.steam_state = g_steam_ok ? 1 : -1;
    btw_unlock();
    if (!g_steam_ok)
        btw_peers_steam_failed();

    DWORD last_sec = GetTickCount(), last_stats = last_sec;
    while (!g_stop) {
        WaitForSingleObject(g_wake_event, 10);
        DWORD now = GetTickCount();
        if (g_steam_ok) {
            dispatch();
            if (g_req_host_ready && g_lobby && S.MM_GetLobbyOwner(g_mm, g_lobby) == g_self) {
                InterlockedExchange(&g_req_host_ready, 0);
                bool ok = S.MM_SetLobbyData(g_mm, g_lobby, "host_ready", "1");
                btw_log("host: host_ready = 1%s", ok ? "" : " (SetLobbyData failed)");
            }
            receive();
            btw_peers_tick(now);
            flush_out();
        } else {
            discard_out();
        }
        if (now - last_sec >= 1000) {
            last_sec = now;
            btw_iat_patch(0);
            if (g_steam_ok) {
                accept_pending();
                if (!g_lobby && !g_creating && !g_joining && g_retry_create_at &&
                    (LONG)(now - g_retry_create_at) >= 0) {
                    g_retry_create_at = 0;
                    create_lobby();
                }
            }
        }
        if (now - last_stats >= 10000) {
            last_stats = now;
            btw_counters_dump();
        }
    }
    if (g_steam_ok) {
        leave_lobby();
        S.Shutdown();
    }
    btw_log("worker: stopped");
    return 0;
}

void btw_session_start(void)
{
    g_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!g_thread) {
        btw_log("worker: CreateThread failed (error %lu): no Steam", (unsigned long)GetLastError());
        g_state.steam_state = -1;
        btw_peers_steam_failed();
    } else
        CloseHandle(g_thread);
}

void btw_session_stop(void)
{
    InterlockedExchange(&g_stop, 1);
    SetEvent(g_wake_event);
}
