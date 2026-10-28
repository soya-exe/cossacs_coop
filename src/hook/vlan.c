/*
 * The virtual LAN: hooked WS2_32 functions (DESIGN.md "Virtual addressing").
 *
 * Every AF_INET/SOCK_DGRAM socket the game creates is tracked. One becomes "hooked"
 * when the game binds it to port 34000 (the GSC native UDP transport). For a hooked
 * socket:
 *   - the real socket is bound to 127.0.0.1:0 instead, so it exists (closesocket,
 *     setsockopt, FIONBIO keep working) and two game copies on one PC don't clash,
 *     but no real traffic ever reaches the game through it;
 *   - sendto(10.77.0.N) goes to the lobby member with slot N; anything else is dropped
 *     (the game never broadcasts: 255.255.255.255 only appears when inet_addr of an
 *     empty/bad host string fails, see re/FINDINGS.md);
 *   - recvfrom / ioctlsocket(FIONREAD) read a queue that the Steam worker fills; the
 *     source is always 10.77.0.<sender slot>:34000.
 * All other sockets (e.g. the lobby ping socket) go to the real ws2_32 untouched.
 *
 * The socket is created and set non-blocking before bind, so it only becomes virtual
 * at bind(:34000); the earlier calls go to the real socket.
 *
 * gethostname/gethostbyname make the game believe its only IPv4 address is
 * 10.77.0.<own slot> (it sends that list in its join request). The host name must
 * stay under 32 chars: it is written into the game's 32-byte nick field.
 */
#include "btwsteam.h"
#include <stdlib.h>
#include <string.h>

typedef struct dgram {
    struct dgram *next;
    int      slot;        /* sender's slot -> from = 10.77.0.slot */
    uint16_t src_port;    /* sender's bound port (host order) */
    int      len;
    int64_t  t_pick, t_steam; /* BTWSTEAM_TIMING: queued here / received by Steam (0 = unknown) */
    uint8_t  data[];
} dgram_t;

typedef struct {
    int      used;
    SOCKET   s;
    int      hooked;
    int      nonblocking;
    uint16_t vport;       /* the port the game asked for (34000), host order */
    dgram_t *head, *tail;
    int      count;
    int      logged_send, logged_recv, logged_qfull;
} vsock_t;

static vsock_t g_socks[BTW_MAX_SOCKETS];   /* protected by btw_lock */
static int g_debug;                        /* BTWSTEAM_DEBUG=1: log every datagram */

static uint32_t g_nonvlan_logged[16];      /* destinations already logged as dropped */
static int g_nonvlan_nlogged;

void btw_vlan_init(void)
{
    g_debug = btw_env_flag("BTWSTEAM_DEBUG");
}

static void fmt_ip(uint32_t ip_host, char out[16])
{
    /* small, allocation-free inet_ntoa */
    char *p = out;
    for (int i = 3; i >= 0; i--) {
        unsigned b = (ip_host >> (i * 8)) & 0xFF;
        if (b >= 100) *p++ = (char)('0' + b / 100);
        if (b >= 10)  *p++ = (char)('0' + (b / 10) % 10);
        *p++ = (char)('0' + b % 10);
        if (i) *p++ = '.';
    }
    *p = 0;
}

static vsock_t *find_locked(SOCKET s)
{
    for (int i = 0; i < BTW_MAX_SOCKETS; i++)
        if (g_socks[i].used && g_socks[i].s == s)
            return &g_socks[i];
    return NULL;
}

static void free_queue_locked(vsock_t *v)
{
    while (v->head) {
        dgram_t *d = v->head;
        v->head = d->next;
        free(d);
    }
    v->tail = NULL;
    v->count = 0;
}

static vsock_t *add_locked(SOCKET s)
{
    vsock_t *v = find_locked(s);    /* a stale entry for a reused handle value */
    if (!v)
        for (int i = 0; i < BTW_MAX_SOCKETS && !v; i++)
            if (!g_socks[i].used)
                v = &g_socks[i];
    if (!v) {
        btw_log("vlan: socket table full, socket %u not tracked", (unsigned)s);
        return NULL;
    }
    free_queue_locked(v);
    memset(v, 0, sizeof *v);
    v->used = 1;
    v->s = s;
    return v;
}

static vsock_t *find_hooked_port_locked(uint16_t port)
{
    for (int i = 0; i < BTW_MAX_SOCKETS; i++)
        if (g_socks[i].used && g_socks[i].hooked && g_socks[i].vport == port)
            return &g_socks[i];
    return NULL;
}

static int fail(int wsa_error)
{
    WSASetLastError(wsa_error);
    return SOCKET_ERROR;
}

void btw_vlan_deliver_locked(int from_slot, uint16_t src_port, uint16_t dst_port,
                             const uint8_t *data, int len, int64_t t_steam_us)
{
    vsock_t *v = find_hooked_port_locked(dst_port);
    if (!v) {
        BTW_INC(rx_drop_nosock);   /* nothing listening yet, as on a real LAN */
        return;
    }
    if (v->count >= BTW_RXQ_MAX) {
        dgram_t *old = v->head;
        v->head = old->next;
        if (!v->head)
            v->tail = NULL;
        v->count--;
        free(old);
        BTW_INC(rx_drop_qfull);
        if (!v->logged_qfull) {
            v->logged_qfull = 1;
            btw_log("vlan: receive queue of socket %u full (%d), dropping oldest (game not reading?)",
                    (unsigned)v->s, BTW_RXQ_MAX);
        }
    }
    dgram_t *d = malloc(sizeof *d + (size_t)len);
    if (!d)
        return;
    d->next = NULL;
    d->slot = from_slot;
    d->src_port = src_port;
    d->len = len;
    d->t_pick = g_timing ? btw_now_us() : 0;
    d->t_steam = t_steam_us;
    memcpy(d->data, data, (size_t)len);
    if (v->tail)
        v->tail->next = d;
    else
        v->head = d;
    v->tail = d;
    v->count++;
}

/* ------------------------------------------------------------------------ */

SOCKET WSAAPI btw_hook_socket(int af, int type, int proto)
{
    SOCKET s = g_real.socket(af, type, proto);
    if (s != INVALID_SOCKET && af == AF_INET && type == SOCK_DGRAM) {
        btw_lock();
        add_locked(s);
        btw_unlock();
        btw_log("vlan: socket(AF_INET, SOCK_DGRAM, %d) = %u (tracked; hooked if bound to :%d)",
                proto, (unsigned)s, BTW_GAME_PORT);
    }
    return s;
}

int WSAAPI btw_hook_bind(SOCKET s, const struct sockaddr *name, int namelen)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)name;
    int game_port = name && namelen >= (int)sizeof(struct sockaddr_in) &&
                    name->sa_family == AF_INET && ntohs(sin->sin_port) == BTW_GAME_PORT;
    char ip[16];

    if (game_port) {
        btw_lock();
        vsock_t *v = find_locked(s);
        if (!v) {
            /* created before our hook (or by someone else): accept it if it is UDP */
            int type = 0, tl = sizeof type;
            if (getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &tl) == 0 && type == SOCK_DGRAM)
                v = add_locked(s);
        }
        if (v) {
            if (find_hooked_port_locked(BTW_GAME_PORT)) {
                btw_unlock();
                btw_log("vlan: bind(%u, :%d) refused, another hooked socket owns the port",
                        (unsigned)s, BTW_GAME_PORT);
                return fail(WSAEADDRINUSE);
            }
            btw_unlock();

            struct sockaddr_in lo;
            memset(&lo, 0, sizeof lo);
            lo.sin_family = AF_INET;
            lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            int r = g_real.bind(s, (const struct sockaddr *)&lo, sizeof lo);
            fmt_ip(ntohl(sin->sin_addr.s_addr), ip);
            if (r != 0) {
                btw_log("vlan: bind(%u, %s:%d) -> real bind to 127.0.0.1:0 failed, error %d",
                        (unsigned)s, ip, BTW_GAME_PORT, WSAGetLastError());
                return r;
            }
            btw_lock();
            v = find_locked(s);
            if (v) {
                v->hooked = 1;
                v->vport = BTW_GAME_PORT;
            }
            btw_unlock();
            struct sockaddr_in real;
            int rl = sizeof real;
            memset(&real, 0, sizeof real);
            g_real.getsockname(s, (struct sockaddr *)&real, &rl);
            btw_log("vlan: HOOKED socket %u: game bind(%s:%d), real socket on 127.0.0.1:%u, "
                    "own virtual address 10.77.0.%d (0 = not known yet)",
                    (unsigned)s, ip, BTW_GAME_PORT, ntohs(real.sin_port), btw_own_slot());
            return 0;
        }
        btw_unlock();
    }

    int r = g_real.bind(s, name, namelen);
    btw_lock();
    int tracked = find_locked(s) != NULL;
    btw_unlock();
    if (tracked && name && name->sa_family == AF_INET) {
        fmt_ip(ntohl(sin->sin_addr.s_addr), ip);
        btw_log("vlan: bind(%u, %s:%u) = %d (not hooked, real network)", (unsigned)s, ip,
                ntohs(sin->sin_port), r);
    }
    return r;
}

int WSAAPI btw_hook_closesocket(SOCKET s)
{
    btw_lock();
    vsock_t *v = find_locked(s);
    int hooked = v && v->hooked, queued = v ? v->count : 0;
    btw_unlock();

    int r = g_real.closesocket(s);
    int err = WSAGetLastError();
    if (v && (r == 0 || err == WSAENOTSOCK)) {
        btw_lock();
        v = find_locked(s);
        if (v) {
            free_queue_locked(v);
            memset(v, 0, sizeof *v);
        }
        btw_unlock();
        if (hooked)
            btw_log("vlan: closesocket(%u) on hooked socket (%d datagrams discarded)", (unsigned)s,
                    queued);
    }
    WSASetLastError(err);
    return r;
}

int WSAAPI btw_hook_getsockname(SOCKET s, struct sockaddr *name, int *namelen)
{
    btw_lock();
    vsock_t *v = find_locked(s);
    int hooked = v && v->hooked;
    uint16_t port = hooked ? v->vport : 0;
    btw_unlock();
    if (!hooked)
        return g_real.getsockname(s, name, namelen);
    if (!name || !namelen || *namelen < (int)sizeof(struct sockaddr_in))
        return fail(WSAEFAULT);
    struct sockaddr_in *sin = (struct sockaddr_in *)name;
    memset(sin, 0, sizeof *sin);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    sin->sin_addr.s_addr = htonl(BTW_VNET | (uint32_t)btw_own_slot_wait());
    *namelen = sizeof *sin;
    return 0;
}

int WSAAPI btw_hook_ioctlsocket(SOCKET s, long cmd, u_long *argp)
{
    if (cmd == (long)FIONREAD) {
        btw_lock();
        vsock_t *v = find_locked(s);
        if (v && v->hooked) {
            /* size of the next datagram, as the game reads one datagram per call */
            u_long n = v->head ? (u_long)v->head->len : 0;
            btw_unlock();
            if (!n) {
                /* nothing queued: ask Steam directly instead of waiting for the worker */
                btw_session_rx_from_game();
                btw_lock();
                v = find_locked(s);
                n = v && v->hooked && v->head ? (u_long)v->head->len : 0;
                btw_unlock();
            }
            btw_timing_poll();   /* after the receive: what Steam had by now was offered */
            if (!argp)
                return fail(WSAEFAULT);
            *argp = n;
            return 0;
        }
        btw_unlock();
    }
    int r = g_real.ioctlsocket(s, cmd, argp);
    if (r == 0 && cmd == (long)FIONBIO && argp) {
        btw_lock();
        vsock_t *v = find_locked(s);
        if (v)
            v->nonblocking = *argp != 0;
        btw_unlock();
    }
    return r;
}

int WSAAPI btw_hook_setsockopt(SOCKET s, int level, int opt, const char *val, int len)
{
    int r = g_real.setsockopt(s, level, opt, val, len);
    btw_lock();
    vsock_t *v = find_locked(s);
    int tracked = v != NULL, hooked = v && v->hooked;
    btw_unlock();
    if (tracked) {
        int iv = 0;
        if (val && len >= (int)sizeof iv)
            memcpy(&iv, val, sizeof iv);
        btw_log("vlan: setsockopt(%u%s, level 0x%x, opt 0x%x, %d) = %d", (unsigned)s,
                hooked ? " hooked" : "", level, opt, iv, r);
    }
    return r;
}

int WSAAPI btw_hook_sendto(SOCKET s, const char *buf, int len, int flags,
                           const struct sockaddr *to, int tolen)
{
    btw_lock();
    vsock_t *v = find_locked(s);
    if (!v || !v->hooked) {
        btw_unlock();
        return g_real.sendto(s, buf, len, flags, to, tolen);
    }
    int err = 0;
    if (len < 0 || (len > 0 && !buf))
        err = WSAEFAULT;
    else if (!to)
        err = WSAEDESTADDRREQ;
    else if (tolen < (int)sizeof(struct sockaddr_in))
        err = WSAEFAULT;
    else if (to->sa_family != AF_INET)
        err = WSAEAFNOSUPPORT;
    else if (len > BTW_MAX_DGRAM)
        err = WSAEMSGSIZE;
    if (err) {
        btw_unlock();
        return fail(err);
    }

    const struct sockaddr_in *sin = (const struct sockaddr_in *)to;
    uint32_t ip = ntohl(sin->sin_addr.s_addr);
    uint16_t dport = ntohs(sin->sin_port);
    uint16_t sport = v->vport;
    int own = btw_own_slot();
    const uint8_t *data = (const uint8_t *)buf;
    char ips[16];
    fmt_ip(ip, ips);

    if (!v->logged_send) {
        v->logged_send = 1;
        btw_log("vlan: first sendto on hooked socket %u: %s:%u, %d bytes", (unsigned)s, ips, dport, len);
    }

    if (ip == INADDR_BROADCAST || (ip & 0xFF) == 0xFF) {
        /* The game has no broadcast discovery; 255.255.255.255 means inet_addr() of
         * the host string failed (INADDR_NONE). Drop it, but say so. */
        BTW_INC(tx_bcast);
        static int nlogged;
        if (nlogged < 5) {
            nlogged++;
            btw_log("vlan: dropping sendto %s:%u (%d bytes): broadcast / INADDR_NONE, the host "
                    "address string was probably empty or invalid", ips, dport, len);
        }
    } else if ((ip & BTW_VNET_MASK) == BTW_VNET) {
        int slot = (int)(ip & 0xFF);
        if (slot == 0) {
            BTW_INC(tx_drop_noslot);
        } else if (slot == own) {
            /* to ourselves: a real stack loops this back to our own socket */
            btw_vlan_deliver_locked(own, sport, dport, data, len, 0);
            BTW_INC(tx_loop);
        } else {
            btw_peers_send_locked(slot, sport, dport, data, len);
            BTW_INC(tx_uni);
            BTW_ADD(tx_bytes, len);
        }
        if (g_debug)
            btw_log("dbg: tx %s:%u %d bytes", ips, dport, len);
    } else {
        BTW_INC(tx_drop_nonvlan);
        int seen = 0;
        for (int i = 0; i < g_nonvlan_nlogged; i++)
            seen |= g_nonvlan_logged[i] == ip;
        if (!seen && g_nonvlan_nlogged < (int)(sizeof g_nonvlan_logged / sizeof g_nonvlan_logged[0])) {
            g_nonvlan_logged[g_nonvlan_nlogged++] = ip;
            btw_log("vlan: dropping sendto %s:%u from hooked socket (not 10.77.0.x); logged once",
                    ips, dport);
        }
    }
    btw_unlock();
    return len; /* UDP: "sent" even if nobody is there */
}

int WSAAPI btw_hook_recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from,
                             int *fromlen)
{
    btw_lock();
    vsock_t *v = find_locked(s);
    if (!v || !v->hooked) {
        btw_unlock();
        return g_real.recvfrom(s, buf, len, flags, from, fromlen);
    }
    if (len < 0 || (len > 0 && !buf) ||
        (from && (!fromlen || *fromlen < (int)sizeof(struct sockaddr_in)))) {
        btw_unlock();
        return fail(WSAEFAULT);
    }
    if (!v->head) {
        /* nothing queued: ask Steam directly (the game normally asks FIONREAD first) */
        btw_unlock();
        btw_session_rx_from_game();
        btw_lock();
        v = find_locked(s);
        if (!v || !v->hooked) {
            btw_unlock();
            return fail(WSAEINTR);
        }
    }
    while (!v->head) {
        if (v->nonblocking) {
            btw_unlock();
            return fail(WSAEWOULDBLOCK);
        }
        /* blocking socket (the game never uses one, but keep the semantics) */
        btw_unlock();
        Sleep(1);
        btw_lock();
        v = find_locked(s);
        if (!v || !v->hooked) {
            btw_unlock();
            return fail(WSAEINTR);
        }
    }

    dgram_t *d = v->head;
    int n = d->len < len ? d->len : len;
    int truncated = d->len > len;
    int full = d->len, slot = d->slot;
    if (n > 0)
        memcpy(buf, d->data, (size_t)n);
    if (from) {
        struct sockaddr_in *sin = (struct sockaddr_in *)from;
        memset(sin, 0, sizeof *sin);
        sin->sin_family = AF_INET;
        sin->sin_port = htons(BTW_GAME_PORT); /* stable peer address: always :34000 */
        sin->sin_addr.s_addr = htonl(BTW_VNET | (uint32_t)d->slot);
        *fromlen = sizeof *sin;
    }
    if (!(flags & MSG_PEEK)) {
        if (g_timing)
            btw_timing_read(d->t_pick, d->t_steam);
        /* UDP: a datagram is consumed even when it did not fit */
        v->head = d->next;
        if (!v->head)
            v->tail = NULL;
        v->count--;
        free(d);
    }
    int first = !v->logged_recv;
    v->logged_recv = 1;
    btw_unlock();

    BTW_INC(rx_game_reads);
    if (first)
        btw_log("vlan: first recvfrom on hooked socket %u: %d bytes from 10.77.0.%d", (unsigned)s,
                full, slot);
    else if (g_debug)
        btw_log("dbg: rx %d bytes from 10.77.0.%d", full, slot);
    if (truncated)
        return fail(WSAEMSGSIZE);
    return n;
}

/* ------------------------------------------------------------------------ */

static char g_real_host[256];

static int is_self_name(const char *name)
{
    if (!name || !*name || _stricmp(name, BTW_HOSTNAME) == 0)
        return 1;
    if (!g_real_host[0] && g_real.gethostname(g_real_host, sizeof g_real_host) != 0)
        g_real_host[0] = 0;
    return g_real_host[0] && _stricmp(name, g_real_host) == 0;
}

int WSAAPI btw_hook_gethostname(char *name, int namelen)
{
    static int logged;
    if (!name || namelen < (int)sizeof BTW_HOSTNAME)
        return fail(WSAEFAULT);
    memcpy(name, BTW_HOSTNAME, sizeof BTW_HOSTNAME);
    if (!logged) {
        logged = 1;
        btw_log("vlan: gethostname -> \"" BTW_HOSTNAME "\"");
    }
    return 0;
}

struct hostent *WSAAPI btw_hook_gethostbyname(const char *name)
{
    static struct hostent he;
    static struct in_addr addr;
    static char *addr_list[2], *aliases[1];
    static char hname[] = BTW_HOSTNAME;
    static int nlogged;

    if (!is_self_name(name)) {
        struct hostent *r = g_real.gethostbyname(name);
        if (nlogged < 20) {
            nlogged++;
            btw_log("vlan: gethostbyname(\"%s\") -> real resolver (%s)", name, r ? "ok" : "failed");
        }
        return r;
    }

    int slot = btw_own_slot_wait();
    addr.s_addr = htonl(BTW_VNET | (uint32_t)slot);
    addr_list[0] = (char *)&addr;
    addr_list[1] = NULL;
    aliases[0] = NULL;
    he.h_name = hname;
    he.h_aliases = aliases;
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = addr_list;
    btw_log("vlan: gethostbyname(\"%s\") -> 10.77.0.%d (own address)", name ? name : "(null)", slot);
    return &he;
}
