/*
 * Mock steam_api.dll (32-bit) for testing btwsteam with several game processes on one PC.
 *
 * Every process is one fake Steam user (BTWSTEAM_MOCK_ID = 1, 2, ...) and runs a tiny
 * "Steam backend" on UDP 127.0.0.1:(47000 + id). Lobbies live in their owner's process;
 * networking messages go straight to the peer's port. No shared memory, so it works across
 * Wine/Proton prefixes. See README.md for behaviour and environment variables.
 */
#define STEAM_FLAT_EXPORT
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "steam_flat.h"

#define ID_BASE     76561190000000000ULL
#define LOBBY_BASE  109775240000000000ULL
#define PORT_BASE   47000
#define MAX_MOCK_ID 1000
#define MAX_MEMBERS 64
#define MAX_KV      128
#define MAX_PEERS   128
#define CHUNK       60000            /* payload bytes per UDP datagram */
#define WIRE_MAGIC  0x4D575442u      /* "BTWM" */
#define MAX_STATE   60000            /* serialized lobby state must fit in one datagram */

enum { W_MSG = 1, W_JOIN_REQ, W_JOIN_RESP, W_STATE, W_LEAVE, W_PING, W_REDIRECT };
enum { EV_SYNC = 0, EV_MEMBER = 1, EV_DATA = 2 };

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  type;
    uint8_t  flags;        /* W_MSG: the sender's nSendFlags (low 8 bits) */
    uint16_t frag_idx;
    uint32_t from_id;      /* sender's mock id */
    int32_t  channel;
    uint32_t seq;
    uint16_t frag_cnt;
    uint16_t pad;
    uint32_t total_len;
} wire_hdr;
#pragma pack(pop)

/* ---- state ------------------------------------------------------------------------------ */
typedef struct cbnode { struct cbnode *next; int id; void *data; int size; } cbnode;
typedef struct resnode { struct resnode *next; SteamAPICall_t call; int id; void *data; int size; } resnode;
typedef struct msgnode { struct msgnode *next; uint64_t from; int channel; int flags; char *data; int size; } msgnode;
typedef struct pktnode { struct pktnode *next; ULONGLONG due; int len; char data[]; } pktnode;

typedef struct {
    uint64_t id;
    int accepted, requested;
    int64_t msgnum;
    uint32_t r_seq, r_total, r_next; uint16_t r_cnt; char *r_buf; int r_flags, r_channel;
} peer_t;

typedef struct { char *key, *val; } kv_t;
typedef struct {
    int in;
    uint64_t id, owner;
    int max, joinable, type;
    int n; uint64_t members[MAX_MEMBERS]; ULONGLONG seen[MAX_MEMBERS];
    int nkv; kv_t kv[MAX_KV];
} lobby_t;

static CRITICAL_SECTION g_lock, g_loglock;
static int g_lock_ready;
static int g_inited, g_dispatch;
static unsigned g_id = 1;
static SOCKET g_sock = INVALID_SOCKET;
static HANDLE g_thread;
static volatile LONG g_running;
static FILE *g_log;
static ULONGLONG g_t0;
static int g_lag_ms;
static double g_loss;
static uint64_t g_invite_lobby;
static ULONGLONG g_invite_at;
static int g_timeout_ms = 5000;
static uint32_t g_rng = 2463534242u;
static SteamAPICall_t g_next_call = 0x100000001ULL;
static uint32_t g_send_seq;
static unsigned long g_dropped, g_sent, g_recv;

static cbnode *g_cb_head, *g_cb_tail, *g_cb_current;
static resnode *g_res;
static msgnode *g_ready_head, *g_ready_tail, *g_held_head, *g_held_tail;
static pktnode *g_pkt_head, *g_pkt_tail;
static peer_t g_peers[MAX_PEERS]; static int g_npeers;
static lobby_t g_lobby;
static ULONGLONG g_owner_seen, g_last_ping;
static int g_join_pending, g_join_redirects; static uint64_t g_join_lobby; static SteamAPICall_t g_join_call;
static ULONGLONG g_join_start;
static uint64_t g_redirect_lobby, g_redirect_owner;
static char g_getdata_ring[8][8193]; static int g_getdata_idx;
static uint8_t g_sendbuf[sizeof(wire_hdr) + CHUNK + 64];

/* distinct dummy interface objects */
static struct { int tag; } g_user = {1}, g_mm = {2}, g_friends = {3}, g_netmsg = {4}, g_netutils = {5};

static void lock(void)   { EnterCriticalSection(&g_lock); }
static void unlock(void) { LeaveCriticalSection(&g_lock); }
static ULONGLONG now_ms(void) { return GetTickCount64(); }
static uint64_t self_sid(void) { return ID_BASE + g_id; }
static unsigned sid_to_mock(uint64_t sid) { return (sid > ID_BASE && sid - ID_BASE <= MAX_MOCK_ID) ? (unsigned)(sid - ID_BASE) : 0; }
static unsigned lobby_to_mock(uint64_t l) { return (l > LOBBY_BASE && l - LOBBY_BASE <= MAX_MOCK_ID) ? (unsigned)(l - LOBBY_BASE) : 0; }

static void logf_(const char *fmt, ...)
{
    if (!g_lock_ready) return;
    EnterCriticalSection(&g_loglock);
    if (g_log) {
        va_list ap;
        fprintf(g_log, "[%8.3f] ", (double)(now_ms() - g_t0) / 1000.0);
        va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_loglock);
}
#define LOG(...) logf_(__VA_ARGS__)

static void check_self(const void *self, const void *want, const char *fn)
{
    if (self != want) LOG("WARNING %s: self=%p, expected interface %p", fn, self, want);
}

static uint32_t rng(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

/* ---- identity helpers (real 32-bit layout: type@0, cbSize@4, steamid@8) ----------------- */
static void ident_set(SteamNetworkingIdentity *id, uint64_t sid)
{
    memset(id, 0, sizeof *id);
    int32_t t = k_ESteamNetworkingIdentityType_SteamID, sz = 8;
    memcpy(id->m_bytes + 0, &t, 4);
    memcpy(id->m_bytes + 4, &sz, 4);
    memcpy(id->m_bytes + 8, &sid, 8);
}
static uint64_t ident_get(const SteamNetworkingIdentity *id)
{
    int32_t t; uint64_t sid;
    if (!id) return 0;
    memcpy(&t, id->m_bytes, 4);
    if (t != k_ESteamNetworkingIdentityType_SteamID) return 0;
    memcpy(&sid, id->m_bytes + 8, 8);
    return sid;
}

/* ---- callbacks -------------------------------------------------------------------------- */
static void push_cb(int id, const void *data, int size)
{
    cbnode *n = calloc(1, sizeof *n);
    n->id = id; n->size = size; n->data = malloc(size > 0 ? size : 1);
    memcpy(n->data, data, size);
    if (g_cb_tail) g_cb_tail->next = n; else g_cb_head = n;
    g_cb_tail = n;
    LOG("callback %d queued (%d bytes)", id, size);
}
static void push_call_result(SteamAPICall_t call, int id, const void *data, int size)
{
    resnode *r = calloc(1, sizeof *r);
    SteamAPICallCompleted_t done;
    r->call = call; r->id = id; r->size = size; r->data = malloc(size); memcpy(r->data, data, size);
    r->next = g_res; g_res = r;
    memset(&done, 0, sizeof done);
    done.m_hAsyncCall = call; done.m_iCallback = id; done.m_cubParam = (uint32_t)size;
    LOG("call result %llu: callback %d via 703", (unsigned long long)call, id);
    push_cb(k_iSteamAPICallCompleted, &done, sizeof done);
}
static void cb_lobby_chat(uint64_t user, uint64_t by, uint32_t flags)
{
    LobbyChatUpdate_t u; memset(&u, 0, sizeof u);
    u.m_ulSteamIDLobby = g_lobby.id; u.m_ulSteamIDUserChanged = user;
    u.m_ulSteamIDMakingChange = by; u.m_rgfChatMemberStateChange = flags;
    LOG("506 LobbyChatUpdate user=%llu flags=0x%x", (unsigned long long)user, flags);
    push_cb(k_iLobbyChatUpdate, &u, sizeof u);
}
static void cb_lobby_data(void)
{
    LobbyDataUpdate_t u; memset(&u, 0, sizeof u);
    u.m_ulSteamIDLobby = g_lobby.id; u.m_ulSteamIDMember = g_lobby.id; u.m_bSuccess = 1;
    push_cb(k_iLobbyDataUpdate, &u, sizeof u);
}
static void cb_lobby_enter(SteamAPICall_t call, uint64_t lobby, uint32_t response)
{
    LobbyEnter_t e; memset(&e, 0, sizeof e);
    e.m_ulSteamIDLobby = lobby; e.m_EChatRoomEnterResponse = response;
    if (call) push_call_result(call, k_iLobbyEnter, &e, sizeof e);
    push_cb(k_iLobbyEnter, &e, sizeof e);
}

/* ---- wire ------------------------------------------------------------------------------- */
static int send_raw(unsigned to, int type, int flags, int channel, uint32_t seq,
                    int fidx, int fcnt, uint32_t total, const void *payload, int len)
{
    wire_hdr h; struct sockaddr_in a; int tries;
    if (g_sock == INVALID_SOCKET || !to || to > MAX_MOCK_ID || len > CHUNK + 64) return -1;
    memset(&h, 0, sizeof h);
    h.magic = WIRE_MAGIC; h.type = (uint8_t)type; h.flags = (uint8_t)flags; h.frag_idx = (uint16_t)fidx;
    h.from_id = g_id; h.channel = channel; h.seq = seq; h.frag_cnt = (uint16_t)fcnt; h.total_len = total;
    memcpy(g_sendbuf, &h, sizeof h);
    if (len) memcpy(g_sendbuf + sizeof h, payload, len);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((u_short)(PORT_BASE + to));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (tries = 0; tries < 200; tries++) {
        if (sendto(g_sock, (const char *)g_sendbuf, (int)sizeof h + len, 0, (struct sockaddr *)&a, sizeof a) >= 0)
            return 0;
        if (WSAGetLastError() != WSAEWOULDBLOCK) break;
        Sleep(1);
    }
    LOG("sendto mock %u failed: %d", to, WSAGetLastError());
    return -1;
}

typedef struct { uint8_t b[MAX_STATE + 256]; int n, err; } wbuf;
static void wput(wbuf *w, const void *d, int n) { if (w->n + n > (int)sizeof w->b) { w->err = 1; return; } memcpy(w->b + w->n, d, n); w->n += n; }
static void wu8(wbuf *w, uint8_t v)  { wput(w, &v, 1); }
static void wu16(wbuf *w, uint16_t v){ wput(w, &v, 2); }
static void wu32(wbuf *w, uint32_t v){ wput(w, &v, 4); }
static void wu64(wbuf *w, uint64_t v){ wput(w, &v, 8); }
static void wstr(wbuf *w, const char *s) { uint16_t l = (uint16_t)strlen(s); wu16(w, l); wput(w, s, l); }

typedef struct { const uint8_t *p; int n, off, err; } rbuf;
static void rget(rbuf *r, void *d, int n) { if (r->off + n > r->n) { r->err = 1; memset(d, 0, n); return; } memcpy(d, r->p + r->off, n); r->off += n; }
static uint8_t  ru8(rbuf *r)  { uint8_t v;  rget(r, &v, 1); return v; }
static uint16_t ru16(rbuf *r) { uint16_t v; rget(r, &v, 2); return v; }
static uint32_t ru32(rbuf *r) { uint32_t v; rget(r, &v, 4); return v; }
static uint64_t ru64(rbuf *r) { uint64_t v; rget(r, &v, 8); return v; }
static char *rstr(rbuf *r) { uint16_t l = ru16(r); char *s = malloc(l + 1u); rget(r, s, l); s[l] = 0; return s; }

static void lobby_ser(wbuf *w, const lobby_t *l)
{
    int i;
    wu64(w, l->id); wu64(w, l->owner); wu32(w, (uint32_t)l->max); wu8(w, (uint8_t)l->joinable); wu8(w, (uint8_t)l->type);
    wu16(w, (uint16_t)l->n);
    for (i = 0; i < l->n; i++) wu64(w, l->members[i]);
    wu16(w, (uint16_t)l->nkv);
    for (i = 0; i < l->nkv; i++) { wstr(w, l->kv[i].key); wstr(w, l->kv[i].val); }
}
static void lobby_free_kv(lobby_t *l)
{
    int i;
    for (i = 0; i < l->nkv; i++) { free(l->kv[i].key); free(l->kv[i].val); }
    l->nkv = 0;
}
static void lobby_clear(void) { lobby_free_kv(&g_lobby); memset(&g_lobby, 0, sizeof g_lobby); }
/* read a serialized lobby into *l (which must be cleared); returns 0 on success */
static int lobby_deser(rbuf *r, lobby_t *l)
{
    int i, n;
    l->id = ru64(r); l->owner = ru64(r); l->max = (int)ru32(r); l->joinable = ru8(r); l->type = ru8(r);
    n = ru16(r); if (n > MAX_MEMBERS) return -1;
    l->n = n;
    for (i = 0; i < n; i++) { l->members[i] = ru64(r); l->seen[i] = now_ms(); }
    n = ru16(r); if (n > MAX_KV) return -1;
    for (i = 0; i < n && !r->err; i++) { l->kv[i].key = rstr(r); l->kv[i].val = rstr(r); l->nkv = i + 1; }
    l->in = 1;
    return r->err ? -1 : 0;
}

static int lobby_member_idx(uint64_t sid)
{
    int i;
    for (i = 0; i < g_lobby.n; i++) if (g_lobby.members[i] == sid) return i;
    return -1;
}
static void lobby_remove_member(int i)
{
    memmove(&g_lobby.members[i], &g_lobby.members[i + 1], (g_lobby.n - i - 1) * sizeof g_lobby.members[0]);
    memmove(&g_lobby.seen[i], &g_lobby.seen[i + 1], (g_lobby.n - i - 1) * sizeof g_lobby.seen[0]);
    g_lobby.n--;
}

/* owner -> every member except `skip` (and itself) */
static void broadcast_state(int ev, uint64_t user, uint32_t flags, uint64_t skip)
{
    static wbuf w; int i;
    w.n = 0; w.err = 0;
    wu8(&w, (uint8_t)ev); wu64(&w, user); wu32(&w, flags);
    lobby_ser(&w, &g_lobby);
    if (w.err) { LOG("lobby state too large to send"); return; }
    for (i = 0; i < g_lobby.n; i++) {
        uint64_t m = g_lobby.members[i];
        if (m == self_sid() || m == skip) continue;
        send_raw(sid_to_mock(m), W_STATE, 0, 0, 0, 0, 1, (uint32_t)w.n, w.b, w.n);
    }
}
static void send_u64(unsigned to, int type, uint64_t v) { send_raw(to, type, 0, 0, 0, 0, 1, 8, &v, 8); }

/* leave the current lobby (lock held) */
static void do_leave(void)
{
    if (!g_lobby.in) return;
    LOG("leaving lobby %llu (owner=%s, %d members)", (unsigned long long)g_lobby.id,
        g_lobby.owner == self_sid() ? "me" : "other", g_lobby.n);
    if (g_lobby.owner == self_sid()) {
        int me = lobby_member_idx(self_sid());
        if (me >= 0) lobby_remove_member(me);
        if (g_lobby.n > 0) {
            g_lobby.owner = g_lobby.members[0];
            g_redirect_lobby = g_lobby.id; g_redirect_owner = g_lobby.owner;
            LOG("ownership -> %llu", (unsigned long long)g_lobby.owner);
            broadcast_state(EV_MEMBER, self_sid(), k_EChatMemberStateChangeLeft, 0);
        }
    } else {
        send_u64(sid_to_mock(g_lobby.owner), W_LEAVE, g_lobby.id);
    }
    lobby_clear();
}

/* ---- messages --------------------------------------------------------------------------- */
static peer_t *peer_get(uint64_t sid, int create)
{
    int i;
    for (i = 0; i < g_npeers; i++) if (g_peers[i].id == sid) return &g_peers[i];
    if (!create || g_npeers >= MAX_PEERS) return NULL;
    memset(&g_peers[g_npeers], 0, sizeof g_peers[0]);
    g_peers[g_npeers].id = sid;
    return &g_peers[g_npeers++];
}
static void msg_append(msgnode **h, msgnode **t, msgnode *m) { m->next = NULL; if (*t) (*t)->next = m; else *h = m; *t = m; }

static void msg_complete(peer_t *p, int channel, int flags, char *data, int size)
{
    msgnode *m = calloc(1, sizeof *m);
    m->from = p->id; m->channel = channel; m->flags = flags; m->data = data; m->size = size;
    g_recv++;
    if (p->accepted) { msg_append(&g_ready_head, &g_ready_tail, m); return; }
    msg_append(&g_held_head, &g_held_tail, m);
    if (!p->requested) {
        SteamNetworkingMessagesSessionRequest_t req;
        p->requested = 1;
        ident_set(&req.m_identityRemote, p->id);
        LOG("1251 SessionRequest from %llu (message held)", (unsigned long long)p->id);
        push_cb(k_iSteamNetworkingMessagesSessionRequest, &req, sizeof req);
    }
}

static void on_msg(const wire_hdr *h, const char *pl, int len)
{
    peer_t *p = peer_get(ID_BASE + h->from_id, 1);
    if (!p) return;
    if (h->frag_cnt <= 1) {
        char *d = malloc(len > 0 ? len : 1);
        memcpy(d, pl, len);
        msg_complete(p, h->channel, h->flags, d, len);
        return;
    }
    if (h->total_len > k_cbMaxSteamNetworkingSocketsMessageSizeSend) return;
    if (h->frag_idx == 0) {
        free(p->r_buf);
        p->r_buf = malloc(h->total_len); p->r_total = h->total_len; p->r_seq = h->seq;
        p->r_cnt = h->frag_cnt; p->r_next = 0; p->r_flags = h->flags; p->r_channel = h->channel;
    }
    if (!p->r_buf || h->seq != p->r_seq || h->frag_idx != p->r_next ||
        (uint64_t)h->frag_idx * CHUNK + (uint32_t)len > p->r_total) {
        LOG("fragment out of order from %u (seq %u idx %u), dropping message", h->from_id, h->seq, h->frag_idx);
        free(p->r_buf); p->r_buf = NULL;
        return;
    }
    memcpy(p->r_buf + (size_t)h->frag_idx * CHUNK, pl, len);
    p->r_next++;
    if (p->r_next == p->r_cnt) {
        char *d = p->r_buf; p->r_buf = NULL;
        msg_complete(p, p->r_channel, p->r_flags, d, (int)p->r_total);
    }
}

static void free_msg_data(SteamNetworkingMessage_t *m) { free(m->m_pData); m->m_pData = NULL; }
static void release_msg(SteamNetworkingMessage_t *m)
{
    if (!m) return;
    if (m->m_pfnFreeData) m->m_pfnFreeData(m);
    free(m);
}

/* ---- lobby protocol --------------------------------------------------------------------- */
static void on_join_req(unsigned from, uint64_t lobby)
{
    uint64_t sid = ID_BASE + from;
    static wbuf w; uint32_t resp = k_EChatRoomEnterResponseSuccess;
    LOG("join request from %llu for lobby %llu", (unsigned long long)sid, (unsigned long long)lobby);
    if (!(g_lobby.in && g_lobby.id == lobby && g_lobby.owner == self_sid())) {
        if (g_redirect_lobby == lobby && g_redirect_owner) {
            uint32_t to = sid_to_mock(g_redirect_owner);
            LOG("redirecting join to new owner %u", to);
            send_raw(from, W_REDIRECT, 0, 0, 0, 0, 1, 4, &to, 4);
            return;
        }
        resp = k_EChatRoomEnterResponseDoesntExist;
    } else if (lobby_member_idx(sid) < 0) {
        if (!g_lobby.joinable) resp = k_EChatRoomEnterResponseNotAllowed;
        else if (g_lobby.n >= g_lobby.max || g_lobby.n >= MAX_MEMBERS) resp = k_EChatRoomEnterResponseFull;
        else {
            g_lobby.members[g_lobby.n] = sid; g_lobby.seen[g_lobby.n] = now_ms(); g_lobby.n++;
            cb_lobby_chat(sid, sid, k_EChatMemberStateChangeEntered);
            broadcast_state(EV_MEMBER, sid, k_EChatMemberStateChangeEntered, sid);
        }
    }
    w.n = 0; w.err = 0;
    wu64(&w, lobby); wu32(&w, resp);
    if (resp == k_EChatRoomEnterResponseSuccess) lobby_ser(&w, &g_lobby);
    LOG("join response to %u: %u", from, resp);
    send_raw(from, W_JOIN_RESP, 0, 0, 0, 0, 1, (uint32_t)w.n, w.b, w.n);
}

static void on_join_resp(rbuf *r)
{
    uint64_t lobby = ru64(r); uint32_t resp = ru32(r);
    if (!g_join_pending || lobby != g_join_lobby) { LOG("stray join response for %llu", (unsigned long long)lobby); return; }
    g_join_pending = 0;
    if (resp == k_EChatRoomEnterResponseSuccess) {
        lobby_clear();
        if (lobby_deser(r, &g_lobby) != 0) { LOG("bad lobby state"); lobby_clear(); resp = k_EChatRoomEnterResponseError; }
    }
    LOG("joined lobby %llu: response %u, %d members", (unsigned long long)lobby, resp, g_lobby.n);
    if (resp == k_EChatRoomEnterResponseSuccess) {
        g_owner_seen = now_ms();
        cb_lobby_data();
    }
    cb_lobby_enter(g_join_call, lobby, resp);
}

static void on_state(unsigned from, rbuf *r)
{
    int ev = ru8(r); uint64_t user = ru64(r); uint32_t flags = ru32(r);
    lobby_t nl; memset(&nl, 0, sizeof nl);
    if (lobby_deser(r, &nl) != 0) { LOG("bad state from %u", from); lobby_free_kv(&nl); return; }
    if (!g_lobby.in || nl.id != g_lobby.id || ID_BASE + from != g_lobby.owner) {
        /* an ownership transfer comes from the old owner, so also accept from current owner */
        if (!(g_lobby.in && nl.id == g_lobby.id && lobby_member_idx(ID_BASE + from) >= 0)) {
            LOG("ignoring state from %u for lobby %llu", from, (unsigned long long)nl.id);
            lobby_free_kv(&nl); return;
        }
    }
    {
        int was_owner_change = nl.owner != g_lobby.owner;
        lobby_free_kv(&g_lobby);
        g_lobby = nl;
        g_owner_seen = now_ms();
        if (lobby_member_idx(self_sid()) < 0) {
            LOG("no longer a member of lobby %llu", (unsigned long long)g_lobby.id);
            lobby_clear(); return;
        }
        if (ev == EV_MEMBER) cb_lobby_chat(user, user, flags);
        if (ev == EV_DATA || was_owner_change) cb_lobby_data();
        if (was_owner_change) LOG("lobby owner is now %llu", (unsigned long long)g_lobby.owner);
    }
}

static void on_leave(unsigned from, uint64_t lobby)
{
    uint64_t sid = ID_BASE + from; int i;
    if (!g_lobby.in || g_lobby.id != lobby || g_lobby.owner != self_sid()) return;
    i = lobby_member_idx(sid);
    if (i < 0) return;
    lobby_remove_member(i);
    LOG("member %llu left", (unsigned long long)sid);
    cb_lobby_chat(sid, sid, k_EChatMemberStateChangeLeft);
    broadcast_state(EV_MEMBER, sid, k_EChatMemberStateChangeLeft, 0);
}

static void handle_packet(const char *buf, int len)
{
    wire_hdr h; rbuf r;
    if (len < (int)sizeof h) return;
    memcpy(&h, buf, sizeof h);
    if (h.magic != WIRE_MAGIC || !h.from_id || h.from_id > MAX_MOCK_ID) return;
    r.p = (const uint8_t *)buf + sizeof h; r.n = len - (int)sizeof h; r.off = 0; r.err = 0;
    /* any packet from the owner/a member counts as alive */
    if (g_lobby.in) {
        int i = lobby_member_idx(ID_BASE + h.from_id);
        if (i >= 0) g_lobby.seen[i] = now_ms();
        if (ID_BASE + h.from_id == g_lobby.owner) g_owner_seen = now_ms();
    }
    switch (h.type) {
    case W_MSG:       on_msg(&h, (const char *)r.p, r.n); break;
    case W_JOIN_REQ:  on_join_req(h.from_id, ru64(&r)); break;
    case W_JOIN_RESP: on_join_resp(&r); break;
    case W_STATE:     on_state(h.from_id, &r); break;
    case W_LEAVE:     on_leave(h.from_id, ru64(&r)); break;
    case W_PING:      break;
    case W_REDIRECT: {
        uint32_t to = ru32(&r);
        if (g_join_pending && g_join_redirects < 4 && to && to <= MAX_MOCK_ID) {
            g_join_redirects++; g_join_start = now_ms();
            LOG("join redirected to mock %u", to);
            send_u64(to, W_JOIN_REQ, g_join_lobby);
        }
        break; }
    default: break;
    }
}

/* periodic work (lock held) */
static void tick(void)
{
    ULONGLONG t = now_ms();
    int i;
    if (g_invite_lobby && g_invite_at && t >= g_invite_at) {
        GameLobbyJoinRequested_t j;
        j.m_steamIDLobby = g_invite_lobby;
        j.m_steamIDFriend = ID_BASE + lobby_to_mock(g_invite_lobby);
        LOG("333 GameLobbyJoinRequested lobby=%llu friend=%llu (BTWSTEAM_MOCK_INVITE)",
            (unsigned long long)j.m_steamIDLobby, (unsigned long long)j.m_steamIDFriend);
        push_cb(k_iGameLobbyJoinRequested, &j, sizeof j);
        g_invite_at = 0;
    }
    if (g_join_pending && t - g_join_start > (ULONGLONG)g_timeout_ms) {
        LOG("join of %llu timed out", (unsigned long long)g_join_lobby);
        g_join_pending = 0;
        cb_lobby_enter(g_join_call, g_join_lobby, k_EChatRoomEnterResponseDoesntExist);
    }
    if (!g_lobby.in) return;
    if (t - g_last_ping >= 1000) {
        g_last_ping = t;
        if (g_lobby.owner == self_sid()) {
            for (i = 0; i < g_lobby.n; i++)
                if (g_lobby.members[i] != self_sid()) send_u64(sid_to_mock(g_lobby.members[i]), W_PING, g_lobby.id);
        } else send_u64(sid_to_mock(g_lobby.owner), W_PING, g_lobby.id);
    }
    if (g_lobby.owner == self_sid()) {
        for (i = 0; i < g_lobby.n; i++) {
            uint64_t m = g_lobby.members[i];
            if (m == self_sid() || t - g_lobby.seen[i] <= (ULONGLONG)g_timeout_ms) continue;
            LOG("member %llu silent for %d ms: disconnected", (unsigned long long)m, g_timeout_ms);
            lobby_remove_member(i);
            cb_lobby_chat(m, m, k_EChatMemberStateChangeDisconnected);
            broadcast_state(EV_MEMBER, m, k_EChatMemberStateChangeDisconnected, 0);
            i--;
        }
    } else if (t - g_owner_seen > (ULONGLONG)g_timeout_ms) {
        uint64_t old = g_lobby.owner; int oi = lobby_member_idx(old);
        LOG("owner %llu silent for %d ms: disconnected", (unsigned long long)old, g_timeout_ms);
        if (oi >= 0) lobby_remove_member(oi);
        cb_lobby_chat(old, old, k_EChatMemberStateChangeDisconnected);
        if (g_lobby.n == 0) { lobby_clear(); return; }
        g_lobby.owner = g_lobby.members[0];
        g_owner_seen = t;
        for (i = 0; i < g_lobby.n; i++) g_lobby.seen[i] = t;
        LOG("lobby owner is now %llu", (unsigned long long)g_lobby.owner);
        cb_lobby_data();
        if (g_lobby.owner == self_sid()) broadcast_state(EV_SYNC, 0, 0, 0);
    }
}

static DWORD WINAPI backend_thread(LPVOID arg)
{
    static char buf[70000];
    (void)arg;
    while (g_running) {
        fd_set rs; struct timeval tv = {0, 5000};
        FD_ZERO(&rs); FD_SET(g_sock, &rs);
        select(0, &rs, NULL, NULL, &tv);
        for (;;) {
            int n = recvfrom(g_sock, buf, sizeof buf, 0, NULL, NULL);
            pktnode *p;
            if (n < 0) {
                int e = WSAGetLastError();
                if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;  /* ICMP unreachable etc. */
                break;
            }
            p = malloc(sizeof *p + n);
            p->next = NULL; p->due = now_ms() + (ULONGLONG)g_lag_ms; p->len = n; memcpy(p->data, buf, n);
            lock();
            if (g_pkt_tail) g_pkt_tail->next = p; else g_pkt_head = p;
            g_pkt_tail = p;
            unlock();
        }
        lock();
        while (g_pkt_head && g_pkt_head->due <= now_ms()) {
            pktnode *p = g_pkt_head;
            g_pkt_head = p->next; if (!g_pkt_head) g_pkt_tail = NULL;
            handle_packet(p->data, p->len);
            free(p);
        }
        tick();
        unlock();
    }
    return 0;
}

/* ---- exports: lifecycle ----------------------------------------------------------------- */
static void env_str(const char *name, char *out, DWORD n) { out[0] = 0; if (!GetEnvironmentVariableA(name, out, n)) out[0] = 0; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH && !g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        InitializeCriticalSection(&g_loglock);
        g_lock_ready = 1;
    }
    return TRUE;
}

int __cdecl SteamAPI_InitFlat(char *err)
{
    char v[256], path[MAX_PATH], *slash;
    WSADATA wd; struct sockaddr_in a; int bufsz = 8 << 20; u_long nb = 1;
    if (err) err[0] = 0;
    lock();
    if (g_inited) { unlock(); return k_ESteamAPIInitResult_OK; }
    g_t0 = now_ms();
    env_str("BTWSTEAM_MOCK_ID", v, sizeof v);
    g_id = v[0] ? (unsigned)strtoul(v, NULL, 10) : 1;
    if (!g_id || g_id > MAX_MOCK_ID) g_id = 1;
    GetModuleFileNameA(NULL, path, sizeof path);
    slash = strrchr(path, '\\'); if (!slash) slash = strrchr(path, '/');
    if (slash) slash[1] = 0; else path[0] = 0;
    snprintf(path + strlen(path), sizeof path - strlen(path), "steam_mock_%u.log", g_id);
    if (!g_log) g_log = fopen(path, "a");
    env_str("BTWSTEAM_MOCK_LAG_MS", v, sizeof v); g_lag_ms = v[0] ? atoi(v) : 0; if (g_lag_ms < 0) g_lag_ms = 0;
    env_str("BTWSTEAM_MOCK_LOSS", v, sizeof v);   g_loss = v[0] ? atof(v) : 0.0;
    if (g_loss < 0) g_loss = 0;
    if (g_loss > 1) g_loss = 1;
    env_str("BTWSTEAM_MOCK_TIMEOUT_MS", v, sizeof v); if (v[0] && atoi(v) > 0) g_timeout_ms = atoi(v);
    env_str("BTWSTEAM_MOCK_INVITE", v, sizeof v); g_invite_lobby = v[0] ? strtoull(v, NULL, 10) : 0;
    g_invite_at = g_invite_lobby ? g_t0 + 3000 : 0;
    g_rng ^= (g_id * 2654435761u) ^ (uint32_t)GetTickCount();
    if (!g_rng) g_rng = 1;
    LOG("==== mock steam_api InitFlat: id=%u steamid=%llu port=%u lag=%dms loss=%.3f invite=%llu timeout=%dms",
        g_id, (unsigned long long)self_sid(), PORT_BASE + g_id, g_lag_ms, g_loss,
        (unsigned long long)g_invite_lobby, g_timeout_ms);

    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) goto fail;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) goto fail;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&bufsz, sizeof bufsz);
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, (const char *)&bufsz, sizeof bufsz);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((u_short)(PORT_BASE + g_id)); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_sock, (struct sockaddr *)&a, sizeof a) != 0) {
        LOG("bind 127.0.0.1:%u failed (%d): another process with BTWSTEAM_MOCK_ID=%u?", PORT_BASE + g_id, WSAGetLastError(), g_id);
        if (err) snprintf(err, 1024, "mock steam: cannot bind 127.0.0.1:%u (id %u in use?)", PORT_BASE + g_id, g_id);
        closesocket(g_sock); g_sock = INVALID_SOCKET; WSACleanup();
        unlock(); return k_ESteamAPIInitResult_FailedGeneric;
    }
    ioctlsocket(g_sock, FIONBIO, &nb);
    g_running = 1;
    g_thread = CreateThread(NULL, 0, backend_thread, NULL, 0, NULL);
    if (!g_thread) { g_running = 0; closesocket(g_sock); g_sock = INVALID_SOCKET; WSACleanup(); goto fail; }
    g_inited = 1;
    unlock();
    return k_ESteamAPIInitResult_OK;
fail:
    LOG("InitFlat failed: %d", WSAGetLastError());
    if (err) snprintf(err, 1024, "mock steam: winsock setup failed");
    unlock();
    return k_ESteamAPIInitResult_FailedGeneric;
}

void __cdecl SteamAPI_Shutdown(void)
{
    HANDLE th;
    lock();
    if (!g_inited) { unlock(); return; }
    LOG("Shutdown (sent %lu, received %lu, dropped by loss %lu)", g_sent, g_recv, g_dropped);
    do_leave();
    g_running = 0; th = g_thread; g_thread = NULL;
    unlock();
    if (th) { WaitForSingleObject(th, 2000); CloseHandle(th); }
    lock();
    closesocket(g_sock); g_sock = INVALID_SOCKET; WSACleanup();
    while (g_cb_head) { cbnode *n = g_cb_head; g_cb_head = n->next; free(n->data); free(n); }
    g_cb_tail = NULL;
    if (g_cb_current) { free(g_cb_current->data); free(g_cb_current); g_cb_current = NULL; }
    while (g_res) { resnode *n = g_res; g_res = n->next; free(n->data); free(n); }
    while (g_ready_head) { msgnode *n = g_ready_head; g_ready_head = n->next; free(n->data); free(n); }
    while (g_held_head) { msgnode *n = g_held_head; g_held_head = n->next; free(n->data); free(n); }
    g_ready_tail = g_held_tail = NULL;
    while (g_pkt_head) { pktnode *n = g_pkt_head; g_pkt_head = n->next; free(n); }
    g_pkt_tail = NULL;
    { int i; for (i = 0; i < g_npeers; i++) free(g_peers[i].r_buf); g_npeers = 0; }
    g_inited = 0; g_dispatch = 0; g_join_pending = 0;
    unlock();
}

bool __cdecl SteamAPI_IsSteamRunning(void) { return true; }
HSteamPipe __cdecl SteamAPI_GetHSteamPipe(void) { return g_inited ? 1 : 0; }

void __cdecl SteamAPI_ManualDispatch_Init(void) { lock(); g_dispatch = 1; LOG("ManualDispatch_Init"); unlock(); }
void __cdecl SteamAPI_ManualDispatch_RunFrame(HSteamPipe pipe)
{
    if (pipe != 1) LOG("WARNING RunFrame: pipe %d", pipe);
    /* the backend runs on its own thread; nothing to pump here */
}

bool __cdecl SteamAPI_ManualDispatch_GetNextCallback(HSteamPipe pipe, CallbackMsg_t *msg)
{
    cbnode *n;
    lock();
    if (!g_inited || pipe != 1 || !msg) { unlock(); return false; }
    if (!g_dispatch) LOG("WARNING GetNextCallback before ManualDispatch_Init");
    if (g_cb_current) {
        LOG("WARNING GetNextCallback without FreeLastCallback; freeing callback %d", g_cb_current->id);
        free(g_cb_current->data); free(g_cb_current); g_cb_current = NULL;
    }
    n = g_cb_head;
    if (!n) { unlock(); return false; }
    g_cb_head = n->next; if (!g_cb_head) g_cb_tail = NULL;
    g_cb_current = n;
    msg->m_hSteamUser = 1; msg->m_iCallback = n->id; msg->m_pubParam = n->data; msg->m_cubParam = n->size;
    unlock();
    return true;
}

void __cdecl SteamAPI_ManualDispatch_FreeLastCallback(HSteamPipe pipe)
{
    (void)pipe;
    lock();
    if (g_cb_current) { free(g_cb_current->data); free(g_cb_current); g_cb_current = NULL; }
    unlock();
}

bool __cdecl SteamAPI_ManualDispatch_GetAPICallResult(HSteamPipe pipe, SteamAPICall_t call, void *out,
                                                      int cub, int expected, bool *failed)
{
    resnode **pp, *r;
    (void)pipe;
    lock();
    for (pp = &g_res; *pp; pp = &(*pp)->next) if ((*pp)->call == call) break;
    r = *pp;
    if (!r) { LOG("GetAPICallResult(%llu): unknown call", (unsigned long long)call); unlock(); if (failed) *failed = true; return false; }
    if (r->id != expected || r->size != cub || !out) {
        LOG("GetAPICallResult(%llu): mismatch, have callback %d size %d, asked %d size %d",
            (unsigned long long)call, r->id, r->size, expected, cub);
        unlock(); if (failed) *failed = true; return false;
    }
    memcpy(out, r->data, cub);
    *pp = r->next; free(r->data); free(r);
    if (failed) *failed = false;
    unlock();
    return true;
}

/* ---- ISteamUser ------------------------------------------------------------------------- */
ISteamUser * __cdecl SteamAPI_SteamUser_v023(void) { return g_inited ? (ISteamUser *)&g_user : NULL; }
CSteamID_u64 __cdecl SteamAPI_ISteamUser_GetSteamID(ISteamUser *self)
{
    check_self(self, &g_user, "ISteamUser_GetSteamID");
    return self_sid();
}

/* ---- ISteamMatchmaking ------------------------------------------------------------------ */
ISteamMatchmaking * __cdecl SteamAPI_SteamMatchmaking_v009(void) { return g_inited ? (ISteamMatchmaking *)&g_mm : NULL; }

SteamAPICall_t __cdecl SteamAPI_ISteamMatchmaking_CreateLobby(ISteamMatchmaking *self, int type, int max)
{
    SteamAPICall_t call; LobbyCreated_t c;
    check_self(self, &g_mm, "CreateLobby");
    lock();
    if (!g_inited) { unlock(); return 0; }
    if (g_lobby.in) do_leave();
    call = g_next_call++;
    lobby_clear();
    g_lobby.in = 1; g_lobby.id = LOBBY_BASE + g_id; g_lobby.owner = self_sid();
    g_lobby.max = max > 0 ? max : 1; g_lobby.joinable = 1; g_lobby.type = type;
    g_lobby.n = 1; g_lobby.members[0] = self_sid(); g_lobby.seen[0] = now_ms();
    g_redirect_lobby = 0;
    LOG("CreateLobby(type=%d, max=%d) -> lobby %llu, call %llu", type, max,
        (unsigned long long)g_lobby.id, (unsigned long long)call);
    memset(&c, 0, sizeof c); c.m_eResult = 1; c.m_ulSteamIDLobby = g_lobby.id;
    push_call_result(call, k_iLobbyCreated, &c, sizeof c);   /* 703 -> LobbyCreated_t */
    push_cb(k_iLobbyCreated, &c, sizeof c);                  /* real Steam also broadcasts 513 */
    cb_lobby_enter(0, g_lobby.id, k_EChatRoomEnterResponseSuccess); /* 504 */
    unlock();
    return call;
}

SteamAPICall_t __cdecl SteamAPI_ISteamMatchmaking_JoinLobby(ISteamMatchmaking *self, CSteamID_u64 lobby)
{
    SteamAPICall_t call; unsigned owner;
    check_self(self, &g_mm, "JoinLobby");
    lock();
    if (!g_inited) { unlock(); return 0; }
    call = g_next_call++;
    LOG("JoinLobby(%llu) call %llu", (unsigned long long)lobby, (unsigned long long)call);
    if (g_lobby.in && g_lobby.id != lobby) do_leave();
    owner = lobby_to_mock(lobby);
    g_join_call = call; g_join_lobby = lobby;
    if (!owner) {
        LOG("JoinLobby: %llu is not a mock lobby id", (unsigned long long)lobby);
        cb_lobby_enter(call, lobby, k_EChatRoomEnterResponseDoesntExist);
        unlock(); return call;
    }
    g_join_pending = 1; g_join_redirects = 0; g_join_start = now_ms();
    send_u64(owner, W_JOIN_REQ, lobby);
    unlock();
    return call;
}

void __cdecl SteamAPI_ISteamMatchmaking_LeaveLobby(ISteamMatchmaking *self, CSteamID_u64 lobby)
{
    check_self(self, &g_mm, "LeaveLobby");
    lock();
    LOG("LeaveLobby(%llu)", (unsigned long long)lobby);
    if (g_lobby.in && g_lobby.id == lobby) do_leave();
    if (g_join_pending && g_join_lobby == lobby) g_join_pending = 0;
    unlock();
}

static int kv_find(const char *key)
{
    int i;
    for (i = 0; i < g_lobby.nkv; i++) if (!strcmp(g_lobby.kv[i].key, key)) return i;
    return -1;
}

bool __cdecl SteamAPI_ISteamMatchmaking_SetLobbyData(ISteamMatchmaking *self, CSteamID_u64 lobby, const char *key, const char *val)
{
    int i; static wbuf w;
    check_self(self, &g_mm, "SetLobbyData");
    if (!key || !val) return false;
    lock();
    if (!g_lobby.in || g_lobby.id != lobby || g_lobby.owner != self_sid() || !key[0] ||
        strlen(key) > 255 || strlen(val) > 8192) {
        LOG("SetLobbyData(%llu, \"%s\") refused (not owner / not in lobby / bad key)", (unsigned long long)lobby, key);
        unlock(); return false;
    }
    i = kv_find(key);
    if (i >= 0 && !strcmp(g_lobby.kv[i].val, val)) { unlock(); return true; }   /* unchanged: no callback */
    if (i < 0) {
        if (!val[0]) { unlock(); return true; }
        if (g_lobby.nkv >= MAX_KV) { LOG("SetLobbyData: too many keys"); unlock(); return false; }
        i = g_lobby.nkv++;
        g_lobby.kv[i].key = _strdup(key); g_lobby.kv[i].val = _strdup("");
    }
    free(g_lobby.kv[i].val); g_lobby.kv[i].val = _strdup(val);
    w.n = 0; w.err = 0; lobby_ser(&w, &g_lobby);
    if (w.err || w.n > MAX_STATE - 32) {
        LOG("SetLobbyData: lobby data too large for the mock");
        free(g_lobby.kv[i].val); g_lobby.kv[i].val = _strdup("");
        unlock(); return false;
    }
    if (!val[0]) {     /* empty value deletes the key */
        free(g_lobby.kv[i].key); free(g_lobby.kv[i].val);
        g_lobby.kv[i] = g_lobby.kv[--g_lobby.nkv];
    }
    LOG("SetLobbyData \"%s\" = \"%s\"", key, val);
    cb_lobby_data();
    broadcast_state(EV_DATA, 0, 0, 0);
    unlock();
    return true;
}

const char * __cdecl SteamAPI_ISteamMatchmaking_GetLobbyData(ISteamMatchmaking *self, CSteamID_u64 lobby, const char *key)
{
    char *out; int i;
    check_self(self, &g_mm, "GetLobbyData");
    lock();
    out = g_getdata_ring[g_getdata_idx]; g_getdata_idx = (g_getdata_idx + 1) % 8;
    out[0] = 0;
    if (key && g_lobby.in && g_lobby.id == lobby && (i = kv_find(key)) >= 0)
        snprintf(out, sizeof g_getdata_ring[0], "%s", g_lobby.kv[i].val);
    unlock();
    return out;
}

bool __cdecl SteamAPI_ISteamMatchmaking_SetLobbyJoinable(ISteamMatchmaking *self, CSteamID_u64 lobby, bool joinable)
{
    check_self(self, &g_mm, "SetLobbyJoinable");
    lock();
    if (!g_lobby.in || g_lobby.id != lobby || g_lobby.owner != self_sid()) { unlock(); return false; }
    g_lobby.joinable = joinable ? 1 : 0;
    LOG("SetLobbyJoinable(%d)", g_lobby.joinable);
    broadcast_state(EV_SYNC, 0, 0, 0);
    unlock();
    return true;
}

CSteamID_u64 __cdecl SteamAPI_ISteamMatchmaking_GetLobbyOwner(ISteamMatchmaking *self, CSteamID_u64 lobby)
{
    uint64_t o = 0;
    check_self(self, &g_mm, "GetLobbyOwner");
    lock(); if (g_lobby.in && g_lobby.id == lobby) o = g_lobby.owner; unlock();
    return o;
}

int __cdecl SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(ISteamMatchmaking *self, CSteamID_u64 lobby)
{
    int n = 0;
    check_self(self, &g_mm, "GetNumLobbyMembers");
    lock(); if (g_lobby.in && g_lobby.id == lobby) n = g_lobby.n; unlock();
    return n;
}

CSteamID_u64 __cdecl SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(ISteamMatchmaking *self, CSteamID_u64 lobby, int idx)
{
    uint64_t m = 0;
    check_self(self, &g_mm, "GetLobbyMemberByIndex");
    lock(); if (g_lobby.in && g_lobby.id == lobby && idx >= 0 && idx < g_lobby.n) m = g_lobby.members[idx]; unlock();
    return m;
}

bool __cdecl SteamAPI_ISteamMatchmaking_InviteUserToLobby(ISteamMatchmaking *self, CSteamID_u64 lobby, CSteamID_u64 who)
{
    check_self(self, &g_mm, "InviteUserToLobby");
    LOG("InviteUserToLobby(lobby=%llu, user=%llu) (logged only)", (unsigned long long)lobby, (unsigned long long)who);
    return true;
}

/* ---- ISteamFriends ---------------------------------------------------------------------- */
ISteamFriends * __cdecl SteamAPI_SteamFriends_v018(void) { return g_inited ? (ISteamFriends *)&g_friends : NULL; }
bool __cdecl SteamAPI_ISteamFriends_SetRichPresence(ISteamFriends *self, const char *k, const char *v)
{
    check_self(self, &g_friends, "SetRichPresence");
    LOG("SetRichPresence(\"%s\", \"%s\")", k ? k : "(null)", v ? v : "(null)");
    return k != NULL;
}
void __cdecl SteamAPI_ISteamFriends_ClearRichPresence(ISteamFriends *self)
{
    check_self(self, &g_friends, "ClearRichPresence");
    LOG("ClearRichPresence");
}
void __cdecl SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog(ISteamFriends *self, CSteamID_u64 lobby)
{
    check_self(self, &g_friends, "ActivateGameOverlayInviteDialog");
    LOG("ActivateGameOverlayInviteDialog(%llu) (logged only)", (unsigned long long)lobby);
}

/* ---- ISteamNetworkingMessages ----------------------------------------------------------- */
ISteamNetworkingMessages * __cdecl SteamAPI_SteamNetworkingMessages_SteamAPI_v002(void)
{
    return g_inited ? (ISteamNetworkingMessages *)&g_netmsg : NULL;
}

EResult __cdecl SteamAPI_ISteamNetworkingMessages_SendMessageToUser(ISteamNetworkingMessages *self,
    const SteamNetworkingIdentity *ident, const void *data, uint32_t size, int flags, int channel)
{
    uint64_t sid = ident_get(ident); unsigned to = sid_to_mock(sid);
    uint32_t seq, nfr, i; peer_t *p;
    check_self(self, &g_netmsg, "SendMessageToUser");
    lock();
    if (!g_inited) { unlock(); return 2; }
    if (!to || (size && !data)) { LOG("SendMessageToUser: bad identity %llu", (unsigned long long)sid); unlock(); return 8; }
    if (size > k_cbMaxSteamNetworkingSocketsMessageSizeSend) {
        LOG("SendMessageToUser: %u bytes > 512 KB, k_EResultInvalidParam", size); unlock(); return 8;
    }
    p = peer_get(sid, 1);
    if (p && !p->accepted) {          /* sending to someone opens/accepts the session */
        p->accepted = 1; p->requested = 0;
    }
    if (!(flags & k_nSteamNetworkingSend_Reliable) && g_loss > 0 && (double)rng() / 4294967296.0 < g_loss) {
        g_dropped++;
        unlock(); return 1;
    }
    seq = ++g_send_seq;
    nfr = size ? (size + CHUNK - 1) / CHUNK : 1;
    for (i = 0; i < nfr; i++) {
        uint32_t off = i * CHUNK, len = size - off < CHUNK ? size - off : CHUNK;
        if (!size) len = 0;
        if (send_raw(to, W_MSG, flags & 0xff, channel, seq, (int)i, (int)nfr, size, (const char *)data + off, (int)len) != 0) {
            unlock(); return 2;
        }
    }
    g_sent++;
    unlock();
    return 1;
}

int __cdecl SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(ISteamNetworkingMessages *self, int ch,
    SteamNetworkingMessage_t **out, int max)
{
    msgnode **pp, *m; int n = 0;
    check_self(self, &g_netmsg, "ReceiveMessagesOnChannel");
    if (!out || max <= 0) return 0;
    lock();
    pp = &g_ready_head; g_ready_tail = NULL;
    while ((m = *pp) != NULL) {
        if (n < max && m->channel == ch) {
            SteamNetworkingMessage_t *o = calloc(1, sizeof *o);
            peer_t *p = peer_get(m->from, 1);
            LARGE_INTEGER q, f;
            *pp = m->next;
            o->m_pData = m->data; o->m_cbSize = m->size;
            o->m_conn = p ? (uint32_t)(p - g_peers) + 1 : 0;
            ident_set(&o->m_identityPeer, m->from);
            QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
            o->m_usecTimeReceived = (int64_t)((double)q.QuadPart * 1e6 / (double)f.QuadPart);
            o->m_nMessageNumber = p ? ++p->msgnum : 0;
            o->m_pfnFreeData = free_msg_data; o->m_pfnRelease = release_msg;
            o->m_nChannel = ch; o->m_nFlags = m->flags;
            out[n++] = o;
            free(m);
            continue;
        }
        g_ready_tail = m;
        pp = &m->next;
    }
    unlock();
    return n;
}

bool __cdecl SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(ISteamNetworkingMessages *self, const SteamNetworkingIdentity *ident)
{
    uint64_t sid = ident_get(ident); peer_t *p; msgnode **pp, *m; int moved = 0;
    check_self(self, &g_netmsg, "AcceptSessionWithUser");
    lock();
    p = peer_get(sid, 0);
    if (!p || (!p->accepted && !p->requested)) {
        LOG("AcceptSessionWithUser(%llu): no pending session", (unsigned long long)sid);
        unlock(); return false;
    }
    p->accepted = 1; p->requested = 0;
    pp = &g_held_head; g_held_tail = NULL;
    while ((m = *pp) != NULL) {
        if (m->from == sid) { *pp = m->next; msg_append(&g_ready_head, &g_ready_tail, m); moved++; continue; }
        g_held_tail = m; pp = &m->next;
    }
    LOG("AcceptSessionWithUser(%llu): %d held messages released", (unsigned long long)sid, moved);
    unlock();
    return true;
}

static void drop_from(msgnode **head, msgnode **tail, uint64_t sid)
{
    msgnode **pp = head, *m;
    *tail = NULL;
    while ((m = *pp) != NULL) {
        if (m->from == sid) { *pp = m->next; free(m->data); free(m); continue; }
        *tail = m; pp = &m->next;
    }
}

bool __cdecl SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(ISteamNetworkingMessages *self, const SteamNetworkingIdentity *ident)
{
    uint64_t sid = ident_get(ident); peer_t *p; bool had;
    check_self(self, &g_netmsg, "CloseSessionWithUser");
    lock();
    p = peer_get(sid, 0);
    had = p && (p->accepted || p->requested);
    if (p) { p->accepted = 0; p->requested = 0; free(p->r_buf); p->r_buf = NULL; }
    drop_from(&g_ready_head, &g_ready_tail, sid);
    drop_from(&g_held_head, &g_held_tail, sid);
    LOG("CloseSessionWithUser(%llu) -> %d", (unsigned long long)sid, had);
    unlock();
    return had;
}

void __cdecl SteamAPI_SteamNetworkingMessage_t_Release(SteamNetworkingMessage_t *m)
{
    /* like the real DLL: jump through m_pfnRelease (offset 180) */
    if (m && m->m_pfnRelease) m->m_pfnRelease(m);
}

void __cdecl SteamAPI_SteamNetworkingIdentity_SetSteamID64(SteamNetworkingIdentity *self, uint64_t sid)
{
    if (self) ident_set(self, sid);
}
uint64_t __cdecl SteamAPI_SteamNetworkingIdentity_GetSteamID64(SteamNetworkingIdentity *self) { return ident_get(self); }

/* ---- ISteamNetworkingUtils -------------------------------------------------------------- */
ISteamNetworkingUtils * __cdecl SteamAPI_SteamNetworkingUtils_SteamAPI_v004(void)
{
    return g_inited ? (ISteamNetworkingUtils *)&g_netutils : NULL;
}
bool __cdecl SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32(ISteamNetworkingUtils *self, int e, int32_t v)
{
    check_self(self, &g_netutils, "SetGlobalConfigValueInt32");
    LOG("SetGlobalConfigValueInt32(%d, %d)", e, v);
    return true;
}
