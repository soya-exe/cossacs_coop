/*
 * btwsteam: Cossacks: Back to War LAN multiplayer over Steam.
 *
 * Shared header for the ichat.dll proxy. Module map:
 *   dllmain.c    DllMain, global lock, environment helpers
 *   ichat.c      the 12 IChat exports (Process_GSC_ChatWindow = Steam lobby) and the
 *                GSC-GAME.NET login unlock patch
 *   log.c        btwsteam.log (timestamped, thread-safe, unbuffered)
 *   iat.c        patches dmcr.exe's WS2_32 import table (by ordinal) to our hooks
 *   vlan.c       the hooked winsock functions and the per-socket receive queues
 *   peers.c      lobby member table (slot <-> SteamID), wire format, outbound queue,
 *                keep-alive / timeout bookkeeping
 *   steam_api.c  dynamic loading of steam_api.dll (flat API only)
 *   nick.c       player name sanitizer (UTF-8 Steam name -> ASCII nick; no Windows code)
 *   session.c    Steam worker thread: init, lobby, callbacks, send/receive pump
 *   timing.c     optional latency percentiles (BTWSTEAM_TIMING=1)
 *
 * Threads: the game calls the hooks on its own thread(s); everything that talks to
 * steam_api.dll runs on the single worker thread in session.c, with one exception: when
 * the game polls its socket (FIONREAD / recvfrom) and nothing is queued, the game thread
 * itself calls ISteamNetworkingMessages::ReceiveMessagesOnChannel for game data (thread-safe
 * in Steam; serialised with the worker by a receive lock, see session.c). Callbacks and
 * sends stay on the worker. The two sides meet in peers.c / vlan.c under one global lock
 * (btw_lock), which is never held while calling into Steam.
 */
#ifndef BTWSTEAM_H
#define BTWSTEAM_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "nick.h"

/* Release version. Also embedded as the string "btwsteam-version:<version>" (log.c), which
 * the installers (installer/) look for to tell our ichat.dll from the game's. */
#define BTWSTEAM_VERSION  "0.1.2"

/* ---- virtual LAN constants (DESIGN.md "Virtual addressing") ---- */
#define BTW_GAME_PORT     34000          /* sockets bound to this port are hooked */
#define BTW_VNET          0x0A4D0000u    /* 10.77.0.0/24, host byte order */
#define BTW_VNET_MASK     0xFFFFFF00u
#define BTW_HOSTNAME      "btw-steam"
#define BTW_MAX_SLOT      254

#define BTW_MAX_PEERS     32             /* lobby is 7, room to spare */
#define BTW_MAX_SOCKETS   64             /* tracked UDP sockets */
#define BTW_RXQ_MAX       1024           /* datagrams per hooked socket, drop oldest */
#define BTW_TXQ_MAX       2048           /* outbound Steam messages, drop oldest */
#define BTW_MAX_DGRAM     65507          /* largest UDP payload we accept from the game */

/* ---- wire format on Steam (DESIGN.md "Wire format") ---- */
#define BTW_WIRE_VERSION  1
#define BTW_WIRE_HDR      6              /* u8 ver, u8 kind, u16 src_port, u16 dst_port (BE) */
#define BTW_KIND_UNICAST  0
#define BTW_KIND_BCAST    1
#define BTW_CH_DATA       0              /* unreliable + NoNagle */
#define BTW_CH_CTRL       1              /* reliable: hello / keep-alive / goodbye */
#define BTW_CTRL_HELLO    1
#define BTW_CTRL_KEEPALIVE 2
#define BTW_CTRL_GOODBYE  3

#define BTW_KEEPALIVE_MS  1000
#define BTW_PEER_TIMEOUT_MS 30000        /* > the game's own 20 s; only stops forwarding */
#define BTW_LOBBY_MAX_MEMBERS 7
#define BTW_SLOT_WAIT_MS  5000           /* gethostbyname waits this long for our slot */

/* ---- dllmain.c ---- */
void btw_lock(void);
void btw_unlock(void);
/* Returns the environment variable's value in buf (empty string if unset). */
const char *btw_env(const char *name, char *buf, size_t cap);
int btw_env_flag(const char *name);      /* 1 if set to something other than "0" */
/* Same, but read as Unicode and returned as UTF-8 (for names that may hold Cyrillic). */
const char *btw_env_utf8(const char *name, char *buf, size_t cap);

/* ---- log.c ---- */
void btw_log_init(void);
void btw_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *btw_exe_dir(void);           /* directory of dmcr.exe, with trailing '\' */

/* Counters, dumped to the log every 10 s by the worker. */
typedef struct {
    volatile LONG tx_uni, tx_bcast, tx_bytes, tx_loop;
    volatile LONG tx_drop_nonvlan, tx_drop_noslot, tx_drop_dead, tx_drop_queue, tx_steam_err;
    volatile LONG rx_pkts, rx_bytes, rx_game_reads;
    volatile LONG rx_drop_unknown, rx_drop_bad, rx_drop_nosock, rx_drop_qfull, rx_drop_noslot;
    volatile LONG ctrl_tx, ctrl_rx;
    volatile LONG foreign_results;   /* call results (703) for Steam's own calls, not fetched */
} btw_counters_t;
extern btw_counters_t g_ctr;
#define BTW_INC(f)      InterlockedIncrement(&g_ctr.f)
#define BTW_ADD(f, n)   InterlockedExchangeAdd(&g_ctr.f, (LONG)(n))
void btw_counters_dump(void);

/* ---- timing.c (BTWSTEAM_TIMING=1) ---- */
enum { BTW_T_TX, BTW_T_TXCALL, BTW_T_RXPOLL, BTW_T_RXGAME, BTW_T_RXTOTAL, BTW_T_RXWAIT,
       BTW_T_RXHOOK, BTW_T_GAP, BTW_T_GTCALL, BTW_T_N };
extern int g_timing;                     /* BTWSTEAM_TIMING set */
void    btw_timing_init(void);           /* DllMain */
int64_t btw_now_us(void);                /* QueryPerformanceCounter in microseconds */
void    btw_timing_add(int metric, int64_t us);
void    btw_timing_poll(void);           /* game: FIONREAD on the hooked socket */
/* game: recvfrom consumed a datagram that Steam received at t_steam (btw_now_us clock) */
void    btw_timing_read(int64_t t_pick, int64_t t_steam);
void    btw_timing_gt_call(int64_t us, int got); /* a Steam call made on the game thread */
void    btw_timing_dump(void);           /* worker, every 10 s */

/* ---- iat.c ---- */
/* Real WS2_32 functions (whatever the IAT held before we patched it). */
typedef struct {
    int    (WSAAPI *bind)(SOCKET, const struct sockaddr *, int);
    int    (WSAAPI *closesocket)(SOCKET);
    int    (WSAAPI *getsockname)(SOCKET, struct sockaddr *, int *);
    int    (WSAAPI *ioctlsocket)(SOCKET, long, u_long *);
    int    (WSAAPI *recvfrom)(SOCKET, char *, int, int, struct sockaddr *, int *);
    int    (WSAAPI *sendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);
    int    (WSAAPI *setsockopt)(SOCKET, int, int, const char *, int);
    SOCKET (WSAAPI *socket)(int, int, int);
    struct hostent *(WSAAPI *gethostbyname)(const char *);
    int    (WSAAPI *gethostname)(char *, int);
} btw_ws2_t;
extern btw_ws2_t g_real;
/* Patch (or re-patch) the main module's IAT. Returns the number of slots changed. */
int btw_iat_patch(int initial);

/* ---- vlan.c (hooks; installed by iat.c) ---- */
int    WSAAPI btw_hook_bind(SOCKET, const struct sockaddr *, int);
int    WSAAPI btw_hook_closesocket(SOCKET);
int    WSAAPI btw_hook_getsockname(SOCKET, struct sockaddr *, int *);
int    WSAAPI btw_hook_ioctlsocket(SOCKET, long, u_long *);
int    WSAAPI btw_hook_recvfrom(SOCKET, char *, int, int, struct sockaddr *, int *);
int    WSAAPI btw_hook_sendto(SOCKET, const char *, int, int, const struct sockaddr *, int);
int    WSAAPI btw_hook_setsockopt(SOCKET, int, int, const char *, int);
SOCKET WSAAPI btw_hook_socket(int, int, int);
struct hostent *WSAAPI btw_hook_gethostbyname(const char *);
int    WSAAPI btw_hook_gethostname(char *, int);

void btw_vlan_init(void);
/* Queue a datagram for the hooked socket bound to dst_port. Caller holds btw_lock. */
/* t_steam_us: when Steam received it (btw_now_us clock, 0 = unknown), for BTWSTEAM_TIMING. */
void btw_vlan_deliver_locked(int from_slot, uint16_t src_port, uint16_t dst_port,
                             const uint8_t *data, int len, int64_t t_steam_us);

/* ---- peers.c ---- */
typedef struct btw_out {
    struct btw_out *next;
    uint64_t to;          /* SteamID64 */
    int      channel;     /* BTW_CH_DATA / BTW_CH_CTRL, or -1 = close session */
    int      len;
    int64_t  t_us;        /* btw_now_us() when queued (BTWSTEAM_TIMING), else 0 */
    uint8_t  data[];
} btw_out_t;

void     btw_peers_init(void);
void     btw_peers_set_self(uint64_t id);
uint64_t btw_peers_self(void);
/* Replace the member table with the lobby's current members (worker thread). */
void     btw_peers_update(const uint64_t *ids, const int *slots, int n);
void     btw_peers_reset(int send_goodbye);   /* leaving the lobby */
int      btw_own_slot(void);                  /* 0 = unknown yet (and no fallback served) */
/* Our slot for addresses handed to the game: waits for the lobby up to
 * BTW_SLOT_WAIT_MS the first time, then falls back to 1. */
int      btw_own_slot_wait(void);
void     btw_peers_steam_failed(void);        /* stop waiting for a slot */
/* Game thread: route a datagram. dst_slot = 0 means broadcast. Returns the number
 * of peers it was queued for (for logging only). Caller holds btw_lock. */
int      btw_peers_send_locked(int dst_slot, uint16_t src_port, uint16_t dst_port,
                               const uint8_t *data, int len);
/* A message arrived from Steam (t_steam_us: when Steam got it, 0 = unknown). */
void     btw_peers_on_message(uint64_t from, int channel, const uint8_t *data, int len,
                              int64_t t_steam_us);
/* Worker thread, every ~10 ms: keep-alives and timeouts. */
void     btw_peers_tick(DWORD now);
/* Worker thread: take the whole outbound queue (FIFO order). */
btw_out_t *btw_outq_take(void);
void     btw_outq_push_locked(uint64_t to, int channel, const void *data, int len);
/* Signals the worker that there is something to send. */
extern HANDLE g_wake_event;

/* ---- steam_api.c ---- */
#pragma pack(push, 8)
typedef struct { int32_t user; int32_t callback; uint8_t *param; int32_t param_size; } btw_cbmsg_t;
/* SteamNetworkingIdentity is 136 bytes; keep it opaque and roomy. */
typedef struct { _Alignas(8) unsigned char b[160]; } btw_ident_t;
#pragma pack(pop)

typedef struct {
    HMODULE dll;
    int      (__cdecl *InitFlat)(char *err1024);
    void     (__cdecl *Shutdown)(void);
    bool     (__cdecl *IsSteamRunning)(void);
    int32_t  (__cdecl *GetHSteamPipe)(void);
    void     (__cdecl *MD_Init)(void);
    void     (__cdecl *MD_RunFrame)(int32_t pipe);
    bool     (__cdecl *MD_GetNextCallback)(int32_t pipe, btw_cbmsg_t *msg);
    void     (__cdecl *MD_FreeLastCallback)(int32_t pipe);
    bool     (__cdecl *MD_GetAPICallResult)(int32_t pipe, uint64_t call, void *buf, int size,
                                            int expected, bool *failed);
    void    *(__cdecl *SteamUser)(void);
    uint64_t (__cdecl *User_GetSteamID)(void *self);
    void    *(__cdecl *SteamMatchmaking)(void);
    uint64_t (__cdecl *MM_CreateLobby)(void *self, int type, int max_members);
    uint64_t (__cdecl *MM_JoinLobby)(void *self, uint64_t lobby);
    void     (__cdecl *MM_LeaveLobby)(void *self, uint64_t lobby);
    bool     (__cdecl *MM_SetLobbyData)(void *self, uint64_t lobby, const char *k, const char *v);
    const char *(__cdecl *MM_GetLobbyData)(void *self, uint64_t lobby, const char *k);
    bool     (__cdecl *MM_SetLobbyJoinable)(void *self, uint64_t lobby, bool joinable);
    uint64_t (__cdecl *MM_GetLobbyOwner)(void *self, uint64_t lobby);
    int      (__cdecl *MM_GetNumLobbyMembers)(void *self, uint64_t lobby);
    uint64_t (__cdecl *MM_GetLobbyMemberByIndex)(void *self, uint64_t lobby, int i);
    bool     (__cdecl *MM_InviteUserToLobby)(void *self, uint64_t lobby, uint64_t invitee);
    void    *(__cdecl *SteamFriends)(void);
    bool     (__cdecl *FR_SetRichPresence)(void *self, const char *k, const char *v);
    void     (__cdecl *FR_ClearRichPresence)(void *self);
    void     (__cdecl *FR_ActivateGameOverlayInviteDialog)(void *self, uint64_t lobby);
    const char *(__cdecl *FR_GetPersonaName)(void *self);   /* UTF-8 */
    void    *(__cdecl *SteamNetworkingMessages)(void);
    int      (__cdecl *NM_SendMessageToUser)(void *self, const btw_ident_t *to, const void *data,
                                             uint32_t len, int flags, int channel);
    int      (__cdecl *NM_ReceiveMessagesOnChannel)(void *self, int channel, void **out, int max);
    bool     (__cdecl *NM_AcceptSessionWithUser)(void *self, const btw_ident_t *who);
    bool     (__cdecl *NM_CloseSessionWithUser)(void *self, const btw_ident_t *who);
    void     (__cdecl *Msg_Release)(void *msg);
    void     (__cdecl *Ident_SetSteamID64)(btw_ident_t *self, uint64_t id);
    uint64_t (__cdecl *Ident_GetSteamID64)(const void *self);
    void    *(__cdecl *SteamNetworkingUtils)(void);
    bool     (__cdecl *NU_SetGlobalConfigValueInt32)(void *self, int key, int32_t val);
    int64_t  (__cdecl *NU_GetLocalTimestamp)(void *self);  /* optional, BTWSTEAM_TIMING only */
} btw_sapi_t;
extern btw_sapi_t S;
/* Loads <dir>steam_api.dll and resolves the flat exports. 0 on success. */
int btw_sapi_load(const char *dir);

/* ---- ichat.c ---- */
void btw_patch_login(void);     /* from DllMain: unlock the GSC-GAME.NET connection path */

/* ---- session.c ---- */
void btw_session_start(void);   /* from DllMain: only creates the worker thread */
void btw_session_stop(void);    /* asks the worker to leave the lobby and exit */
/* Game thread (FIONREAD / recvfrom on the hooked socket found nothing queued): receive game
 * data from Steam now. Non-blocking, rate-limited; a no-op without Steam. */
void btw_session_rx_from_game(void);

/* Lobby state for the game thread (ichat.c), a copy taken under btw_lock. */
typedef struct {
    int      steam_state;       /* 0 starting, 1 running, -1 unavailable */
    uint64_t lobby;             /* 0 = not in a lobby (yet) */
    int      is_owner;
    int      own_slot;          /* from lobby data, 0 = not assigned yet */
    int      owner_slot;
    int      host_ready;        /* lobby data host_ready=1: the owner's game room exists */
} btw_lobby_state_t;
void btw_session_get(btw_lobby_state_t *out);
/* Owner: publish host_ready=1 (done by the worker within ~10 ms). */
void btw_session_request_host_ready(void);
/* The Steam persona name (UTF-8, as Steam returns it) read by the worker at init;
 * "" without Steam. */
void btw_session_persona(char *out, size_t cap);

#endif
