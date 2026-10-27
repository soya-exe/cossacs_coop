/*
 * Lobby member table and the Steam wire format.
 *
 * The worker (session.c) rebuilds the table from lobby data whenever the lobby
 * changes; the game thread reads it in sendto to map a virtual address to a
 * SteamID. Nothing here calls Steam: outgoing messages (and session closes) go into
 * a FIFO that the worker drains, and the game thread just wakes the worker.
 *
 * Data message (channel 0, unreliable):
 *   u8 version=1 | u8 kind (0 unicast, 1 broadcast) | u16 src_port BE | u16 dst_port BE | payload
 * Control message (channel 1, reliable):
 *   u8 version=1 | u8 type (1 hello, 2 keep-alive, 3 goodbye) | u8 sender's slot
 */
#include "btwsteam.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t id;
    int      slot;          /* 0 = host has not assigned one yet */
    DWORD    last_heard;    /* GetTickCount of the last message (or of joining) */
    int      alive;         /* 0 after BTW_PEER_TIMEOUT_MS of silence or a goodbye */
    int      logged_send, logged_recv;
} peer_t;

/* all protected by btw_lock */
static peer_t   g_peers[BTW_MAX_PEERS];   /* other members, never ourselves */
static int      g_npeers;
static uint64_t g_self;
static volatile LONG g_own_slot;
static HANDLE   g_slot_event;             /* manual reset: own slot known, or no Steam */
static volatile LONG g_wait_done;         /* the one 5 s wait has happened */
static volatile LONG g_served_slot;       /* the slot the game was told (gethostbyname) */
static btw_out_t *g_outq_head, *g_outq_tail;
static int      g_outq_count;
static DWORD    g_last_keepalive;
static uint64_t g_unknown_logged[16];
static int      g_unknown_nlogged;

HANDLE g_wake_event;

void btw_peers_init(void)
{
    g_slot_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_wake_event = CreateEventA(NULL, FALSE, FALSE, NULL);
}

void btw_peers_set_self(uint64_t id)
{
    btw_lock();
    g_self = id;
    btw_unlock();
}

uint64_t btw_peers_self(void)
{
    return g_self;
}

int btw_own_slot(void)
{
    /* before the lobby gives us a slot, the address the game was told (fallback 1) */
    int s = (int)g_own_slot;
    return s ? s : (int)g_served_slot;
}

int btw_own_slot_wait(void)
{
    int s = (int)g_own_slot;
    if (!s && !InterlockedExchange(&g_wait_done, 1)) {
        btw_log("vlan: own slot not known yet, waiting up to %d ms for the lobby", BTW_SLOT_WAIT_MS);
        WaitForSingleObject(g_slot_event, BTW_SLOT_WAIT_MS);
        s = (int)g_own_slot;
        if (!s)
            btw_log("vlan: WARNING no slot from the lobby (no Steam, no lobby, or host not running "
                    "btwsteam): using 10.77.0.1 alone");
        else
            btw_log("vlan: own slot %d after waiting", s);
    }
    if (!s)
        s = 1;
    InterlockedExchange(&g_served_slot, s);
    return s;
}

void btw_peers_steam_failed(void)
{
    SetEvent(g_slot_event);
}

/* ---- outbound queue ---- */

void btw_outq_push_locked(uint64_t to, int channel, const void *data, int len)
{
    if (g_outq_count >= BTW_TXQ_MAX) {
        btw_out_t *old = g_outq_head;
        g_outq_head = old->next;
        if (!g_outq_head)
            g_outq_tail = NULL;
        g_outq_count--;
        free(old);
        BTW_INC(tx_drop_queue);
    }
    btw_out_t *m = malloc(sizeof *m + (size_t)(len > 0 ? len : 0));
    if (!m)
        return;
    m->next = NULL;
    m->to = to;
    m->channel = channel;
    m->len = len;
    if (len > 0)
        memcpy(m->data, data, (size_t)len);
    if (g_outq_tail)
        g_outq_tail->next = m;
    else
        g_outq_head = m;
    g_outq_tail = m;
    g_outq_count++;
}

btw_out_t *btw_outq_take(void)
{
    btw_lock();
    btw_out_t *m = g_outq_head;
    g_outq_head = g_outq_tail = NULL;
    g_outq_count = 0;
    btw_unlock();
    return m;
}

static void push_ctrl_locked(uint64_t to, int type)
{
    uint8_t msg[3] = { BTW_WIRE_VERSION, (uint8_t)type, (uint8_t)g_own_slot };
    btw_outq_push_locked(to, BTW_CH_CTRL, msg, sizeof msg);
    BTW_INC(ctrl_tx);
}

static peer_t *find_peer_locked(uint64_t id)
{
    for (int i = 0; i < g_npeers; i++)
        if (g_peers[i].id == id)
            return &g_peers[i];
    return NULL;
}

/* ---- membership ---- */

void btw_peers_update(const uint64_t *ids, const int *slots, int n)
{
    DWORD now = GetTickCount();
    peer_t next[BTW_MAX_PEERS];
    int nn = 0, changed = 0, own = 0;

    btw_lock();
    for (int i = 0; i < n; i++) {
        if (ids[i] == g_self) {
            own = slots[i];
            continue;
        }
        if (nn >= BTW_MAX_PEERS)
            break;
        peer_t *old = find_peer_locked(ids[i]);
        if (old) {
            next[nn] = *old;
            if (old->slot != slots[i]) {
                btw_log("peers: %llu slot %d -> %d", (unsigned long long)ids[i], old->slot, slots[i]);
                changed = 1;
            }
        } else {
            memset(&next[nn], 0, sizeof next[nn]);
            next[nn].id = ids[i];
            next[nn].last_heard = now; /* grace period starts now */
            next[nn].alive = 1;
            btw_log("peers: member %llu joined the table, slot %d", (unsigned long long)ids[i], slots[i]);
            push_ctrl_locked(ids[i], BTW_CTRL_HELLO);
            changed = 1;
        }
        next[nn].slot = slots[i];
        nn++;
    }
    for (int i = 0; i < g_npeers; i++) {
        int still = 0;
        for (int j = 0; j < nn; j++)
            still |= next[j].id == g_peers[i].id;
        if (!still) {
            btw_log("peers: member %llu (slot %d) left", (unsigned long long)g_peers[i].id, g_peers[i].slot);
            btw_outq_push_locked(g_peers[i].id, -1, NULL, 0); /* close the session */
            changed = 1;
        }
    }
    memcpy(g_peers, next, sizeof(peer_t) * (size_t)nn);
    g_npeers = nn;

    if (own != (int)g_own_slot) {
        btw_log("peers: own slot %d -> %d (virtual IP 10.77.0.%d)", (int)g_own_slot, own, own);
        if (g_served_slot && own && own != g_served_slot)
            btw_log("peers: WARNING the game was already told it is 10.77.0.%d; leave and re-enter "
                    "the multiplayer menu so it picks up 10.77.0.%d", (int)g_served_slot, own);
        InterlockedExchange(&g_own_slot, own);
        changed = 1;
    }
    if (own)
        SetEvent(g_slot_event);

    if (changed) {
        char line[600];
        int p = 0;
        p += snprintf(line + p, sizeof line - (size_t)p, "self=%llu:%d", (unsigned long long)g_self, own);
        for (int i = 0; i < g_npeers && p < (int)sizeof line - 40; i++)
            p += snprintf(line + p, sizeof line - (size_t)p, " %llu:%d%s",
                          (unsigned long long)g_peers[i].id, g_peers[i].slot,
                          g_peers[i].alive ? "" : "(silent)");
        btw_log("peers: slot table: %s", line);
    }
    btw_unlock();
    SetEvent(g_wake_event);
}

void btw_peers_reset(int send_goodbye)
{
    btw_lock();
    for (int i = 0; i < g_npeers; i++)
        if (send_goodbye)
            push_ctrl_locked(g_peers[i].id, BTW_CTRL_GOODBYE);
    btw_log("peers: table cleared (%d members)%s", g_npeers, send_goodbye ? ", goodbye sent" : "");
    g_npeers = 0;
    InterlockedExchange(&g_own_slot, 0);
    ResetEvent(g_slot_event);
    InterlockedExchange(&g_wait_done, 0); /* next gethostbyname waits for the new lobby */
    btw_unlock();
}

int btw_peers_send_locked(int dst_slot, uint16_t src_port, uint16_t dst_port,
                          const uint8_t *data, int len)
{
    uint8_t stackbuf[2048];
    uint8_t *msg = (size_t)len + BTW_WIRE_HDR <= sizeof stackbuf ? stackbuf
                                                                 : malloc((size_t)len + BTW_WIRE_HDR);
    if (!msg)
        return 0;
    msg[0] = BTW_WIRE_VERSION;
    msg[1] = dst_slot ? BTW_KIND_UNICAST : BTW_KIND_BCAST;
    msg[2] = (uint8_t)(src_port >> 8);
    msg[3] = (uint8_t)src_port;
    msg[4] = (uint8_t)(dst_port >> 8);
    msg[5] = (uint8_t)dst_port;
    if (len > 0)
        memcpy(msg + BTW_WIRE_HDR, data, (size_t)len);

    int n = 0, matched = 0;
    for (int i = 0; i < g_npeers; i++) {
        peer_t *p = &g_peers[i];
        if (p->slot <= 0 || (dst_slot && p->slot != dst_slot))
            continue;
        matched++;
        if (!p->alive) {
            BTW_INC(tx_drop_dead);
            continue;
        }
        btw_outq_push_locked(p->id, BTW_CH_DATA, msg, len + BTW_WIRE_HDR);
        n++;
        if (!p->logged_send) {
            p->logged_send = 1;
            btw_log("peers: first game datagram to %llu (slot %d): %s, %d bytes, port %u -> %u",
                    (unsigned long long)p->id, p->slot, dst_slot ? "unicast" : "broadcast", len,
                    src_port, dst_port);
        }
    }
    if (dst_slot && !matched) {
        static int logged[BTW_MAX_SLOT + 1];
        BTW_INC(tx_drop_noslot);
        if (dst_slot <= BTW_MAX_SLOT && !logged[dst_slot]) {
            logged[dst_slot] = 1;
            btw_log("peers: sendto 10.77.0.%d but no lobby member has that slot; dropped (logged once)",
                    dst_slot);
        }
    }
    if (msg != stackbuf)
        free(msg);
    if (n)
        SetEvent(g_wake_event);
    return n;
}

void btw_peers_on_message(uint64_t from, int channel, const uint8_t *data, int len)
{
    btw_lock();
    peer_t *p = find_peer_locked(from);
    if (!p) {
        int seen = 0;
        for (int i = 0; i < g_unknown_nlogged; i++)
            seen |= g_unknown_logged[i] == from;
        if (!seen && g_unknown_nlogged < 16) {
            g_unknown_logged[g_unknown_nlogged++] = from;
            btw_log("peers: message on channel %d from %llu, not in the slot table; dropped (logged once)",
                    channel, (unsigned long long)from);
        }
        btw_unlock();
        BTW_INC(rx_drop_unknown);
        return;
    }
    p->last_heard = GetTickCount();
    if (!p->alive) {
        p->alive = 1;
        btw_log("peers: %llu (slot %d) is talking again", (unsigned long long)from, p->slot);
    }

    if (channel == BTW_CH_CTRL) {
        BTW_INC(ctrl_rx);
        if (len >= 2 && data[0] == BTW_WIRE_VERSION) {
            int their_slot = len >= 3 ? data[2] : 0;
            if (data[1] == BTW_CTRL_HELLO)
                btw_log("peers: hello from %llu (they say slot %d, table says %d)",
                        (unsigned long long)from, their_slot, p->slot);
            else if (data[1] == BTW_CTRL_GOODBYE) {
                btw_log("peers: goodbye from %llu (slot %d)", (unsigned long long)from, p->slot);
                p->alive = 0;
            }
        }
        btw_unlock();
        return;
    }

    if (len < BTW_WIRE_HDR || data[0] != BTW_WIRE_VERSION) {
        btw_unlock();
        BTW_INC(rx_drop_bad);
        return;
    }
    if (p->slot <= 0) {
        btw_unlock();
        BTW_INC(rx_drop_noslot);
        return;
    }
    uint16_t sport = (uint16_t)(data[2] << 8 | data[3]);
    uint16_t dport = (uint16_t)(data[4] << 8 | data[5]);
    int plen = len - BTW_WIRE_HDR;
    if (!p->logged_recv) {
        p->logged_recv = 1;
        btw_log("peers: first game datagram from %llu (slot %d): %s, %d bytes, port %u -> %u",
                (unsigned long long)from, p->slot, data[1] == BTW_KIND_BCAST ? "broadcast" : "unicast",
                plen, sport, dport);
    }
    btw_vlan_deliver_locked(p->slot, sport, dport, data + BTW_WIRE_HDR, plen);
    btw_unlock();
    BTW_INC(rx_pkts);
    BTW_ADD(rx_bytes, plen);
}

void btw_peers_tick(DWORD now)
{
    if (now - g_last_keepalive < BTW_KEEPALIVE_MS)
        return;
    g_last_keepalive = now;
    btw_lock();
    for (int i = 0; i < g_npeers; i++) {
        peer_t *p = &g_peers[i];
        /* keep-alives go to every member, also silent ones, so they can come back */
        push_ctrl_locked(p->id, BTW_CTRL_KEEPALIVE);
        /* Only stop forwarding: the game has its own 20 s timeout and decides
         * itself what a lost peer means. We never inject anything. */
        /* signed: last_heard can be a little newer than `now` (taken before dispatch) */
        if (p->alive && (LONG)(now - p->last_heard) > BTW_PEER_TIMEOUT_MS) {
            p->alive = 0;
            btw_log("peers: %llu (slot %d) silent for %d s: not forwarding to it until it talks again",
                    (unsigned long long)p->id, p->slot, BTW_PEER_TIMEOUT_MS / 1000);
        }
    }
    btw_unlock();
}
