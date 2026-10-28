/*
 * Optional latency instrumentation (BTWSTEAM_TIMING=1).
 *
 * Every 10 s the worker logs one `timing:` line with p50 / p95 / max (microseconds,
 * histogram bins of 10 us below 2 ms, 100 us below 100 ms, 1 ms above) of:
 *   tx       game sendto -> the SendMessageToUser call for that datagram
 *   txcall   duration of SendMessageToUser itself
 *   rxpoll   Steam received it (SteamNetworkingMessage_t::m_usecTimeReceived, compared with
 *            ISteamNetworkingUtils::GetLocalTimestamp) -> our ReceiveMessagesOnChannel
 *            returned it. This is the hook's polling delay.
 *   rxgame   our receive queue -> the game's recvfrom (the game's own polling)
 *   rxtotal  Steam received it -> the game's recvfrom (rxpoll + rxgame per datagram)
 *   rxwait   Steam received it -> the game's first FIONREAD poll after that: the part of
 *            rxtotal that is the game's own loop (it polls once per frame)
 *   rxhook   that first poll -> the recvfrom that got it: the part the hook adds (0 when
 *            the datagram was ready at the first poll; a frame or more when it was not)
 *   gap      time between two FIONREAD calls of the game on its hooked socket (game loop)
 *   gtcall   duration of one receive from Steam made on the game thread (its cost to the game)
 * and the CPU time of the worker thread, of the hook's Steam calls on the game thread, and of
 * the whole process over the window. Disabled, every entry point is one branch.
 */
#include "btwsteam.h"
#include <stdio.h>
#include <string.h>

int g_timing;

static LARGE_INTEGER g_qpf;

/* bins: [0,2ms) 10 us each, [2,100ms) 100 us, [100,1000ms) 1 ms, then one overflow bin */
#define B1_N   200
#define B2_N   980
#define B3_N   900
#define NBINS  (B1_N + B2_N + B3_N + 1)

typedef struct {
    volatile LONG bins[NBINS];
    volatile LONG n;
    volatile LONG max_us;
} hist_t;

static hist_t g_h[BTW_T_N];
static const char *const k_names[BTW_T_N] = { "tx", "txcall", "rxpoll", "rxgame", "rxtotal",
                                              "rxwait", "rxhook", "gap", "gtcall" };

static volatile LONG g_gt_calls;        /* Steam calls made on the game thread */
static volatile LONG g_gt_us;           /* ... and their total duration */
static volatile LONG g_gt_hits;         /* game-thread receive calls that returned messages */
static volatile LONG g_polls;           /* FIONREAD calls on the hooked socket */
static int64_t g_last_poll_us;          /* game thread only */
#define POLL_RING 256                     /* recent FIONREAD times (game thread only) */
static int64_t g_poll_ring[POLL_RING];
static unsigned g_poll_n;

static ULONGLONG g_prev_wall_us, g_prev_worker_100ns, g_prev_proc_100ns;

int64_t btw_now_us(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    /* split to avoid overflow of c * 1e6 */
    int64_t s = c.QuadPart / g_qpf.QuadPart, r = c.QuadPart % g_qpf.QuadPart;
    return s * 1000000 + r * 1000000 / g_qpf.QuadPart;
}

void btw_timing_init(void)
{
    QueryPerformanceFrequency(&g_qpf);
    if (!g_qpf.QuadPart)
        g_qpf.QuadPart = 1000;
    g_timing = btw_env_flag("BTWSTEAM_TIMING");
    if (g_timing)
        btw_log("timing: BTWSTEAM_TIMING=1, latency percentiles every 10 s (QPC %lld Hz)",
                (long long)g_qpf.QuadPart);
}

static int bin_of(int64_t us)
{
    if (us < 0)
        us = 0;
    if (us < 2000)
        return (int)(us / 10);
    if (us < 100000)
        return B1_N + (int)((us - 2000) / 100);
    if (us < 1000000)
        return B1_N + B2_N + (int)((us - 100000) / 1000);
    return NBINS - 1;
}

/* upper edge of a bin, in us */
static int64_t bin_hi(int b)
{
    if (b < B1_N)
        return (int64_t)(b + 1) * 10;
    if (b < B1_N + B2_N)
        return 2000 + (int64_t)(b - B1_N + 1) * 100;
    if (b < NBINS - 1)
        return 100000 + (int64_t)(b - B1_N - B2_N + 1) * 1000;
    return 1000000;
}

void btw_timing_add(int metric, int64_t us)
{
    if (!g_timing || metric < 0 || metric >= BTW_T_N)
        return;
    hist_t *h = &g_h[metric];
    if (us < 0)
        us = 0;
    InterlockedIncrement(&h->bins[bin_of(us)]);
    InterlockedIncrement(&h->n);
    LONG v = us > 0x7fffffff ? 0x7fffffff : (LONG)us, cur;
    while ((cur = h->max_us) < v && InterlockedCompareExchange(&h->max_us, v, cur) != cur)
        ;
}

void btw_timing_poll(void)
{
    if (!g_timing)
        return;
    int64_t now = btw_now_us();
    int64_t prev = g_last_poll_us;
    g_last_poll_us = now;
    g_poll_ring[g_poll_n++ % POLL_RING] = now;
    InterlockedIncrement(&g_polls);
    /* gaps over 1 s are the game being elsewhere (menus), not its loop rate */
    if (prev && now - prev < 1000000)
        btw_timing_add(BTW_T_GAP, now - prev);
}

void btw_timing_read(int64_t t_pick, int64_t t_steam)
{
    if (!g_timing || !t_pick)
        return;
    int64_t now = btw_now_us();
    btw_timing_add(BTW_T_RXGAME, now - t_pick);
    if (!t_steam)
        return;
    btw_timing_add(BTW_T_RXTOTAL, now - t_steam);
    /* the game's first poll at or after t_steam (newest to oldest; unknown if the ring is older) */
    int64_t first = 0;
    unsigned n = g_poll_n < POLL_RING ? g_poll_n : POLL_RING;
    for (unsigned i = 1; i <= n; i++) {
        int64_t t = g_poll_ring[(g_poll_n - i) % POLL_RING];
        if (t < t_steam)
            break;
        first = t;
        if (i == n && n == POLL_RING)
            first = 0;  /* ring exhausted, the true first poll may be older */
    }
    if (first) {
        btw_timing_add(BTW_T_RXWAIT, first - t_steam);
        btw_timing_add(BTW_T_RXHOOK, now - first);
    }
}

void btw_timing_gt_call(int64_t us, int got)
{
    if (!g_timing)
        return;
    InterlockedIncrement(&g_gt_calls);
    InterlockedExchangeAdd(&g_gt_us, (LONG)us);
    if (got)
        InterlockedIncrement(&g_gt_hits);
}

static void fmt_hist(char *out, size_t cap, int metric)
{
    hist_t *h = &g_h[metric];
    LONG bins[NBINS];
    LONG n = InterlockedExchange(&h->n, 0);
    LONG mx = InterlockedExchange(&h->max_us, 0);
    LONG total = 0;
    for (int b = 0; b < NBINS; b++) {
        bins[b] = InterlockedExchange(&h->bins[b], 0);
        total += bins[b];
    }
    (void)n;
    if (!total) {
        snprintf(out, cap, "%s n=0", k_names[metric]);
        return;
    }
    LONG want50 = (total + 1) / 2, want95 = (LONG)(((int64_t)total * 95 + 99) / 100), acc = 0;
    int64_t p50 = -1, p95 = -1;
    for (int b = 0; b < NBINS; b++) {
        acc += bins[b];
        if (p50 < 0 && acc >= want50)
            p50 = bin_hi(b);
        if (p95 < 0 && acc >= want95) {
            p95 = bin_hi(b);
            break;
        }
    }
    /* a bin's upper edge can exceed the true maximum */
    if (p50 > mx)
        p50 = mx;
    if (p95 > mx)
        p95 = mx;
    snprintf(out, cap, "%s n=%ld p50=%lld p95=%lld max=%ld", k_names[metric], (long)total,
             (long long)p50, (long long)p95, (long)mx);
}

static ULONGLONG ft100(const FILETIME *a, const FILETIME *b)
{
    return ((ULONGLONG)a->dwHighDateTime << 32 | a->dwLowDateTime) +
           ((ULONGLONG)b->dwHighDateTime << 32 | b->dwLowDateTime);
}

/* Worker thread only (it reads its own thread times). */
void btw_timing_dump(void)
{
    if (!g_timing)
        return;
    char p[BTW_T_N][96];
    for (int m = 0; m < BTW_T_N; m++)
        fmt_hist(p[m], sizeof p[m], m);

    FILETIME c, e, k, u;
    ULONGLONG worker = 0, proc = 0, wall = (ULONGLONG)btw_now_us();
    if (GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u))
        worker = ft100(&k, &u);
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        proc = ft100(&k, &u);
    double dwall = g_prev_wall_us ? (double)(wall - g_prev_wall_us) : 0;
    double dworker = (double)(worker - g_prev_worker_100ns) / 10.0;  /* us */
    double dproc = (double)(proc - g_prev_proc_100ns) / 10.0;
    LONG polls = InterlockedExchange(&g_polls, 0);
    LONG gtc = InterlockedExchange(&g_gt_calls, 0), gtus = InterlockedExchange(&g_gt_us, 0);
    LONG gth = InterlockedExchange(&g_gt_hits, 0);
    if (dwall > 0)
        btw_log("timing: %s | %s | %s | %s | %s | %s | %s | %s | %s (us) | game polls %.0f/s | "
                "game-thread steam calls %ld (%ld with data) %.1f ms | cpu worker %.2f%% process %.1f%% "
                "of one core",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], polls * 1e6 / dwall, (long)gtc, (long)gth,
                gtus / 1000.0, 100.0 * dworker / dwall, 100.0 * dproc / dwall);
    g_prev_wall_us = wall;
    g_prev_worker_100ns = worker;
    g_prev_proc_100ns = proc;
}
