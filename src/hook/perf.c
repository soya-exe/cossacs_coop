/*
 * Two optional in-memory performance patches (tools/PERF-RESULTS.md, sections 8 and 9).
 * Both are applied from DllMain, after every byte they depend on has been checked. On any
 * mismatch they log it and leave the code alone. No file on disk is ever written.
 *
 * 1. "novsync" (Wine/Proton only; OPT-IN since 0.1.4, BTWSTEAM_NOVSYNC=1 turns it on).
 *    csemu.dll's render thread presents with Flip(NULL, DDFLIP_WAIT). Under wined3d the vsync'd
 *    flip makes the next Lock of the back buffer spin-wait (3-93 % of a core). One byte in memory,
 *    csemu+0x3188: 0x01 -> 0x09 (DDFLIP_WAIT | DDFLIP_NOVSYNC). Frame pacing is not affected:
 *    csemu's own limiter on the game thread paces the frames. csemu.dll is a static import of
 *    dmcr.exe, so it is mapped before our DllMain runs, and its render thread starts much
 *    later (IDirectDraw::CreatePalette), so nothing executes the byte while we change it.
 *    It was on by default in 0.1.3 and froze the picture at the main menu for a player on
 *    Proton Experimental (Wine 11.0) + nested gamescope on Hyprland + NVIDIA GLX; with
 *    BTWSTEAM_NOVSYNC=0 the same setup worked. The headless test harness (system Wine and
 *    Proton in the Steam Linux Runtime, gamescope --backend headless) never froze, so the
 *    unthrottled present path of a real display cannot be tested here: opt-in only.
 *
 * 2. "nospin" (BTWSTEAM_NOSPIN=0 turns it off). Two busy-wait loops in the in-game frame
 *    function 0x4b1530 call ProcessMessages (0x4717a0) and the game clock (0x5250e0) until a
 *    deadline passes:
 *      MP tick wait 0x4b158c-0x4b15e7: deadline in EDI (next tick, 0x8c0e7c), and BL = "render
 *        extra frames" (fixed for the loop). With BL=0 the loop only spins.
 *      SP end-of-frame wait 0x4b1964-0x4b199b: until speed/2 ms (0x6dd7e4) have passed since
 *        the last frame (0x8c0e54), and for as long as the game is paused (0x8c0e14).
 *    Each loop's `call ProcessMessages` is pointed at a wrapper that first, when the loop is
 *    only waiting, waits about 1 ms in MsgWaitForMultipleObjectsEx (it returns at once when
 *    input arrives or is already queued), and then calls the real ProcessMessages. The loop
 *    conditions stay the game's own. A timed wait starts only if it ends before the game
 *    clock can reach the deadline: under Wine GetTickCount() is the monotonic clock as of the
 *    last wineserver request and QueryPerformanceCounter is the same clock now, so the
 *    wrapper knows the exact time left (until_us). It reads the game clock's offset without
 *    calling 0x5250e0, which would slew the MP clock offset.
 *    Timed waits need that fresh clock, so they are Wine-only (the calibration checks it). On
 *    Windows GetTickCount steps by ~15.6 ms and a 1 ms wait can take 15.6 ms, so a timed wait
 *    could move a lockstep tick; there only the paused wait, which has no deadline, sleeps.
 */
#include "btwsteam.h"
#include <string.h>

#define EXE_BASE 0x00400000u

/* ---- helpers ---------------------------------------------------------------------- */

static const char *wine_version(void)
{
    typedef const char *(__cdecl *wgv_t)(void);
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    wgv_t f = nt ? (wgv_t)(void (*)(void))GetProcAddress(nt, "wine_get_version") : NULL;
    return f ? f() : NULL;
}

/* 1 if the switch is set to exactly "0" (default: on). */
static int env_off(const char *name)
{
    char v[16];
    return strcmp(btw_env(name, v, sizeof v), "0") == 0;
}

/* 1 if the switch is set to exactly "1" (default: off). */
static int env_on(const char *name)
{
    char v[16];
    return strcmp(btw_env(name, v, sizeof v), "1") == 0;
}

static int write_code(void *p, const void *bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(p, bytes, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return 1;
}

static void hex(char *out, const BYTE *p, size_t n)
{
    static const char d[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        out[i * 3] = d[p[i] >> 4];
        out[i * 3 + 1] = d[p[i] & 15];
        out[i * 3 + 2] = ' ';
    }
    out[n ? n * 3 - 1 : 0] = 0;
}

static int64_t qpc_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t)(c.QuadPart / f.QuadPart * 1000000 + c.QuadPart % f.QuadPart * 1000000 / f.QuadPart);
}

/* ---- 1. csemu flip without vsync -------------------------------------------------- */

#define CSEMU_SIZE      357056u     /* bin/csemu.dll as Steam installs it (sha256 45a032b0...) */
#define CSEMU_TIMESTAMP 0x4d26ff86u
#define CSEMU_IMAGESIZE 0x0005c000u
#define CSEMU_CHECKSUM  0x00059355u
#define CSEMU_CTX_RVA   0x317fu     /* render loop 0x1000317f: mov ecx,[eax]; mov edx,[ecx+2c] ... */
#define CSEMU_FLAG_RVA  0x3188u     /* the imm8 of `push 1` (Flip dwFlags) */
/* mov ecx,[eax]; mov edx,[ecx+0x2c] (Flip); add esp,0xc; push 1; push 0; push eax; call edx */
static const BYTE k_csemu_ctx[15] = {
    0x8B, 0x08, 0x8B, 0x51, 0x2C, 0x83, 0xC4, 0x0C, 0x6A, 0x01, 0x6A, 0x00, 0x50, 0xFF, 0xD2
};
#define DDFLIP_WAIT_   0x01
#define DDFLIP_NOVSYNC_ 0x08

static void patch_novsync(const char *wine)
{
    if (!wine) {
        btw_log("novsync: not running under Wine/Proton; csemu's vsync'd flip left as is");
        return;
    }
    if (!env_on("BTWSTEAM_NOVSYNC")) {
        /* Default since 0.1.4: on a real display (nested gamescope, NVIDIA) the unthrottled
         * flip froze the picture at the main menu; see the comment at the top. */
        btw_log("novsync: off (default since 0.1.4; BTWSTEAM_NOVSYNC=1 turns it on), csemu's flip left with vsync");
        return;
    }
    BYTE *base = (BYTE *)GetModuleHandleA("csemu.dll");
    if (!base) {
        btw_log("novsync: csemu.dll is not loaded yet (expected as a static import of dmcr.exe); skipped");
        return;
    }
    char path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    DWORD n = GetModuleFileNameA((HMODULE)base, path, sizeof path);
    if (n == 0 || n >= sizeof path || !GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) {
        btw_log("novsync: cannot stat csemu.dll (error %lu); skipped", (unsigned long)GetLastError());
        return;
    }
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        fa.nFileSizeHigh != 0 || fa.nFileSizeLow != CSEMU_SIZE ||
        nt->FileHeader.TimeDateStamp != CSEMU_TIMESTAMP ||
        nt->OptionalHeader.SizeOfImage != CSEMU_IMAGESIZE ||
        nt->OptionalHeader.CheckSum != CSEMU_CHECKSUM) {
        btw_log("novsync: csemu.dll is not the known build (file size %lu, timestamp %08lx, image size "
                "%lx, checksum %lx; want %u, %08x, %x, %x); NOT patched",
                (unsigned long)fa.nFileSizeLow, (unsigned long)nt->FileHeader.TimeDateStamp,
                (unsigned long)nt->OptionalHeader.SizeOfImage, (unsigned long)nt->OptionalHeader.CheckSum,
                CSEMU_SIZE, CSEMU_TIMESTAMP, CSEMU_IMAGESIZE, CSEMU_CHECKSUM);
        return;
    }
    BYTE *ctx = base + CSEMU_CTX_RVA, *flag = base + CSEMU_FLAG_RVA;
    BYTE want[sizeof k_csemu_ctx];
    memcpy(want, k_csemu_ctx, sizeof want);
    want[CSEMU_FLAG_RVA - CSEMU_CTX_RVA] = DDFLIP_WAIT_ | DDFLIP_NOVSYNC_;
    if (memcmp(ctx, want, sizeof want) == 0) {
        btw_log("novsync: csemu+0x%x already DDFLIP_WAIT|DDFLIP_NOVSYNC", CSEMU_FLAG_RVA);
        return;
    }
    if (memcmp(ctx, k_csemu_ctx, sizeof k_csemu_ctx) != 0) {
        char got[sizeof k_csemu_ctx * 3], exp[sizeof k_csemu_ctx * 3];
        hex(got, ctx, sizeof k_csemu_ctx);
        hex(exp, k_csemu_ctx, sizeof k_csemu_ctx);
        btw_log("novsync: bytes at csemu+0x%x are %s, expected %s; NOT patched", CSEMU_CTX_RVA, got, exp);
        return;
    }
    BYTE v = DDFLIP_WAIT_ | DDFLIP_NOVSYNC_;
    if (!write_code(flag, &v, 1)) {
        btw_log("novsync: VirtualProtect failed (%lu); NOT patched", (unsigned long)GetLastError());
        return;
    }
    btw_log("novsync: csemu.dll at %p (mapped before our DllMain), Flip flags at csemu+0x%x 01 -> 09 "
            "(DDFLIP_WAIT|DDFLIP_NOVSYNC), in memory only", (void *)base, CSEMU_FLAG_RVA);
}

/* ---- 2. sleep instead of spin --------------------------------------------------------- */

#define EXE_TIMESTAMP 0x4d2c8aafu   /* dmcr.exe 1.35 */
#define EXE_IMAGESIZE 0x02317000u

/* The game's globals (dmcr.exe always loads at 0x400000: no DYNAMIC_BASE; checked below). */
#define G_CLOCK_OFS  (*(volatile LONG *)0x026ad7a4)  /* game clock offset, 1/128 ms */
#define G_SP_LAST    (*(volatile LONG *)0x008c0e54)  /* game clock at the end of the last frame */
#define G_SP_SPEED   (*(volatile LONG *)0x006dd7e4)  /* mode.dat speed: SP frame >= speed/2 ms */
#define G_PAUSED     (*(volatile BYTE *)0x008c0e14)
#define G_FRAMES     (*(volatile LONG *)0x008c0e5c)  /* +1 per in-game frame (0x4b1530) */
#define PM_VA        0x004717a0u                     /* ProcessMessages */
#define MP_CALL_VA   0x004b15d7u
#define SP_CALL_VA   0x004b1964u

/* 0x4b1570..0x4b15e9: the MP tick wait, from `mov edi,[0x8c0e7c]` to `jg 0x4b158c` */
static const BYTE k_mp_loop[121] = {
    0x8B, 0x3D, 0x7C, 0x0E, 0x8C, 0x00, 0xE8, 0x65, 0x3B, 0x07, 0x00, 0x8B, 0xCF, 0x2B, 0xC8, 0xA1,
    0x8C, 0xC1, 0x5A, 0x00, 0xC1, 0xF8, 0x02, 0x3B, 0xC8, 0x0F, 0x9F, 0xC3, 0x84, 0xDB, 0x74, 0x47,
    0xE8, 0x4B, 0x3B, 0x07, 0x00, 0x8B, 0xF0, 0x8B, 0xCE, 0x2B, 0xCF, 0x79, 0x3A, 0xE8, 0x9E, 0x71,
    0x01, 0x00, 0xE8, 0x59, 0x46, 0xFD, 0xFF, 0xE8, 0x34, 0x3B, 0x07, 0x00, 0x8B, 0x0D, 0x8C, 0xC1,
    0x5A, 0x00, 0x2B, 0xC6, 0x8D, 0x14, 0x48, 0x03, 0xCA, 0xC1, 0xF9, 0x02, 0x89, 0x0D, 0x8C, 0xC1,
    0x5A, 0x00, 0xE8, 0x19, 0x3B, 0x07, 0x00, 0x8B, 0x0D, 0x78, 0x0E, 0x8C, 0x00, 0x2B, 0xC6, 0x03,
    0xC8, 0x89, 0x0D, 0x78, 0x0E, 0x8C, 0x00, 0xE8, 0xC4, 0x01, 0xFC, 0xFF, 0xE8, 0xFF, 0x3A, 0x07,
    0x00, 0x8B, 0xCF, 0x2B, 0xC8, 0x85, 0xC9, 0x7F, 0xA3
};
/* 0x4b1948..0x4b199d: the SP end-of-frame / pause wait */
static const BYTE k_sp_loop[85] = {
    0x39, 0x2D, 0x54, 0x0E, 0x8C, 0x00, 0x75, 0x0A, 0xE8, 0x8B, 0x37, 0x07, 0x00, 0xA3, 0x54, 0x0E,
    0x8C, 0x00, 0x66, 0x83, 0x3D, 0x08, 0x6F, 0x5D, 0x00, 0x02, 0x73, 0x39, 0xE8, 0x37, 0xFE, 0xFB,
    0xFF, 0xA0, 0x14, 0x0E, 0x8C, 0x00, 0x84, 0xC0, 0x74, 0x05, 0xE8, 0x89, 0xD6, 0xFF, 0xFF, 0xE8,
    0x64, 0x37, 0x07, 0x00, 0x8B, 0x1D, 0x54, 0x0E, 0x8C, 0x00, 0x8B, 0xC8, 0xA1, 0xE4, 0xD7, 0x6D,
    0x00, 0x2B, 0xCB, 0x99, 0x2B, 0xC2, 0xD1, 0xF8, 0x3B, 0xC8, 0x7C, 0xD0, 0xA0, 0x14, 0x0E, 0x8C,
    0x00, 0x84, 0xC0, 0x75, 0xC7
};
/* 0x4717a0..0x4717b8: ProcessMessages' prologue */
static const BYTE k_pm_entry[24] = {
    0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x90, 0xB6, 0x59, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00,
    0x50, 0x64, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00
};
/* 0x5250e0..0x525108 and 0x525268..0x52526f: the game clock = GetTickCount() + offset/128 */
static const BYTE k_clock_a[40] = {
    0x57, 0xFF, 0x15, 0xB4, 0xE1, 0x59, 0x00, 0x8B, 0xF8, 0xA1, 0xAC, 0xD7, 0x6A, 0x02, 0x85, 0xC0,
    0x0F, 0x84, 0x67, 0x01, 0x00, 0x00, 0x8B, 0xCF, 0x2B, 0xC8, 0xA1, 0xA4, 0xD7, 0x6A, 0x02, 0x83,
    0xF9, 0x05, 0x0F, 0x8E, 0x60, 0x01, 0x00, 0x00
};
static const BYTE k_clock_b[7] = { 0xC1, 0xF8, 0x07, 0x03, 0xC7, 0x5F, 0xC3 };

static const struct { DWORD va; const BYTE *b; size_t n; const char *what; } k_checks[] = {
    { 0x004b1570, k_mp_loop, sizeof k_mp_loop, "MP tick wait" },
    { 0x004b1948, k_sp_loop, sizeof k_sp_loop, "SP frame wait" },
    { PM_VA, k_pm_entry, sizeof k_pm_entry, "ProcessMessages" },
    { 0x005250e0, k_clock_a, sizeof k_clock_a, "game clock" },
    { 0x00525268, k_clock_b, sizeof k_clock_b, "game clock tail" },
};

typedef char (__cdecl *pm_fn)(void);
static pm_fn g_pm;
static int g_exe_ok;                  /* dmcr.exe 1.35 verified: its globals may be read */
static int g_nospin;                  /* the two calls are redirected */
static volatile LONG g_timed_ok;      /* timed waits allowed (set by the calibration) */
static DWORD g_wait_ms = 1;           /* MsgWait timeout giving a ~1 ms wait (calibrated) */
static volatile LONG g_cost_us = 1000000; /* longest recent wait, microseconds */

enum { W_MP, W_SP, W_PAUSED, W_N };
static struct {
    volatile LONG calls[2];           /* wrapper calls: MP, SP */
    volatile LONG waits[W_N];
    volatile LONG wait_ms;            /* total time in waits */
    volatile LONG input_wakes;        /* a wait returned early for input */
    volatile LONG late;               /* the fresh clock passed the deadline during a wait */
    volatile LONG late_max_us;
} g_ns;
static LONG g_wait_us_carry;          /* game thread only */

/* Microseconds until the game clock (0x5250e0 = GetTickCount() + offset/128) reaches
 * `deadline`, measured on the fresh clock. Under Wine GetTickCount() is the monotonic clock in
 * ms as of the last wineserver request (it lags by 0-16 ms), and QueryPerformanceCounter is
 * the same clock read now (10 MHz); the calibration checks this before it allows timed
 * waits. The loop itself still decides with the game's own clock, which is never ahead of
 * the fresh one, so a wait that ends before the fresh deadline cannot delay the loop's exit. */
static int64_t until_us(LONG deadline)
{
    int64_t now = qpc_us();
    DWORD target = (DWORD)(deadline - (G_CLOCK_OFS >> 7));
    LONG rem_ms = (LONG)(target - (DWORD)(now / 1000));
    return (int64_t)rem_ms * 1000 - now % 1000;
}

/* Wait only if the wait ends before the deadline: the longest recent wait + 0.2 ms margin
 * + 0.16 ms (one slewing step of the MP clock offset). */
static int room_for_wait(int64_t rem_us)
{
    return g_timed_ok && rem_us >= (int64_t)g_cost_us + 360;
}

static void do_wait(int kind)
{
    int64_t t0 = qpc_us();
    DWORD r = MsgWaitForMultipleObjectsEx(0, NULL, g_wait_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    LONG us = (LONG)(qpc_us() - t0);
    if (r == WAIT_TIMEOUT) {
        LONG c = g_cost_us;
        g_cost_us = us > c ? us : c - (c - us) / 64;   /* jumps up at once, decays slowly */
    } else
        g_ns.input_wakes++;
    g_ns.waits[kind]++;
    g_wait_us_carry += us;
    if (g_wait_us_carry >= 1000) {
        g_ns.wait_ms += g_wait_us_carry / 1000;
        g_wait_us_carry %= 1000;
    }
}

static void after_timed_wait(LONG deadline)
{
    int64_t rem = until_us(deadline);
    if (rem < 0) {
        g_ns.late++;
        if (-rem > g_ns.late_max_us)
            g_ns.late_max_us = (LONG)-rem;
    }
}

/* MP: replaces `call ProcessMessages` at 0x4b15d7. deadline = EDI, extra = BL (asm thunk). */
char __cdecl btw_nospin_mp(LONG deadline, LONG extra_frames);
char __cdecl btw_nospin_mp(LONG deadline, LONG extra_frames)
{
    g_ns.calls[0]++;
    if (!extra_frames && room_for_wait(until_us(deadline))) {
        do_wait(W_MP);
        after_timed_wait(deadline);
    }
    return g_pm();
}

/* The game calls with BL and EDI live; pass them on. All other registers the loop uses are
 * callee-saved in our C code (EBX, ESI, EDI, EBP); EAX/ECX/EDX are dead after the call. */
void btw_nospin_mp_thunk(void);
__asm__(".text\n"
        ".globl _btw_nospin_mp_thunk\n"
        "_btw_nospin_mp_thunk:\n"
        "\tmovzbl %bl, %eax\n"
        "\tpushl %eax\n"
        "\tpushl %edi\n"
        "\tcall _btw_nospin_mp\n"
        "\taddl $8, %esp\n"
        "\tret\n");

/* SP: replaces `call ProcessMessages` at 0x4b1964 (the loop reads only globals). */
static char __cdecl nospin_sp(void)
{
    g_ns.calls[1]++;
    if (G_PAUSED) {
        do_wait(W_PAUSED);            /* no deadline while paused; input ends the wait */
    } else {
        LONG deadline = G_SP_LAST + G_SP_SPEED / 2;   /* cdq; sub eax,edx; sar eax,1 */
        if (room_for_wait(until_us(deadline))) {
            do_wait(W_SP);
            after_timed_wait(deadline);
        }
    }
    return g_pm();
}

static int check_exe(void)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if ((DWORD)(uintptr_t)base != EXE_BASE || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != EXE_TIMESTAMP ||
        nt->OptionalHeader.SizeOfImage != EXE_IMAGESIZE) {
        btw_log("nospin: main module is not dmcr.exe 1.35 at 0x400000 (base %p, timestamp %08lx, "
                "image size %lx); NOT patched", (void *)base,
                (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)nt->OptionalHeader.SizeOfImage);
        return 0;
    }
    for (size_t i = 0; i < sizeof k_checks / sizeof k_checks[0]; i++) {
        const BYTE *p = (const BYTE *)(uintptr_t)k_checks[i].va;
        if (memcmp(p, k_checks[i].b, k_checks[i].n) != 0) {
            size_t j = 0;
            while (p[j] == k_checks[i].b[j])
                j++;
            btw_log("nospin: %s at 0x%08lx differs from dmcr.exe 1.35 at +%u (%02X, expected %02X); "
                    "NOT patched", k_checks[i].what, (unsigned long)k_checks[i].va, (unsigned)j, p[j],
                    k_checks[i].b[j]);
            return 0;
        }
    }
    return 1;
}

static int redirect_call(DWORD site, void *target)
{
    BYTE c[5];
    LONG rel = (LONG)((uintptr_t)target - (site + 5));
    c[0] = 0xE8;
    memcpy(c + 1, &rel, 4);
    return write_code((void *)(uintptr_t)site, c, 5);
}

static void patch_nospin(void)
{
    g_exe_ok = check_exe();
    if (!g_exe_ok)
        return;
    if (env_off("BTWSTEAM_NOSPIN")) {
        btw_log("nospin: BTWSTEAM_NOSPIN=0, the game's wait loops left spinning");
        return;
    }
    g_pm = (pm_fn)(uintptr_t)PM_VA;
    if (!redirect_call(MP_CALL_VA, (void *)btw_nospin_mp_thunk)) {
        btw_log("nospin: VirtualProtect failed (%lu); NOT patched", (unsigned long)GetLastError());
        return;
    }
    if (!redirect_call(SP_CALL_VA, (void *)nospin_sp)) {
        static const BYTE orig[5] = { 0xE8, 0xC4, 0x01, 0xFC, 0xFF };
        write_code((void *)(uintptr_t)MP_CALL_VA, orig, 5);
        btw_log("nospin: VirtualProtect failed (%lu); NOT patched", (unsigned long)GetLastError());
        return;
    }
    g_nospin = 1;
    btw_log("nospin: ProcessMessages calls at 0x%08x (MP tick wait) and 0x%08x (SP frame/pause wait) "
            "-> waiting wrappers; timed waits pending the clock check", MP_CALL_VA, SP_CALL_VA);
}

static LONG wait_sample(DWORD ms, LONG *worst)
{
    LONG sum = 0;
    *worst = 0;
    for (int i = 0; i < 8; i++) {
        int64_t t0 = qpc_us();
        MsgWaitForMultipleObjectsEx(0, NULL, ms, QS_ALLINPUT, 0);
        LONG us = (LONG)(qpc_us() - t0);
        sum += us;
        if (us > *worst)
            *worst = us;
    }
    return sum / 8;
}

/* Worker thread, once. Pick the MsgWait timeout that gives about 1 ms (Wine 11 returns at
 * once for 1 ms, and waits ~1 ms for 2 ms), and allow timed waits only under Wine with
 * GetTickCount() <= QPC in ms <= GetTickCount() + 50 (the fresh clock, see until_us). */
void btw_perf_calibrate(void)
{
    if (!g_nospin)
        return;
    LONG w1, w2, m1 = wait_sample(1, &w1), m2 = 0;
    DWORD ms = 1;
    LONG worst = w1;
    if (m1 < 400) {
        m2 = wait_sample(2, &w2);
        if (m2 >= 400 && w2 < 3000) {
            ms = 2;
            worst = w2;
        }
    }
    g_wait_ms = ms;
    g_cost_us = worst;
    LONG lag_min = 0x7fffffff, lag_max = -0x7fffffff;
    for (int i = 0; i < 2000; i++) {
        DWORD t = GetTickCount();
        LONG lag = (LONG)((DWORD)(qpc_us() / 1000) - t);
        if (lag < lag_min)
            lag_min = lag;
        if (lag > lag_max)
            lag_max = lag;
        if (i % 200 == 199)
            Sleep(1);
    }
    int clock_ok = wine_version() && lag_min >= 0 && lag_max <= 50;
    btw_log("nospin: MsgWait 1 ms -> mean %ld us (max %ld); using %lu ms (max %ld us); "
            "GetTickCount lags QPC by %ld..%ld ms",
            (long)m1, (long)w1, (unsigned long)ms, (long)worst, (long)lag_min, (long)lag_max);
    if (m2)
        btw_log("nospin: MsgWait 2 ms -> mean %ld us (max %ld)", (long)m2, (long)w2);
    if (clock_ok && worst < 3000) {
        InterlockedExchange(&g_timed_ok, 1);
        btw_log("nospin: timed waits on (MP tick wait, SP frame wait) and the paused wait");
    } else
        btw_log("nospin: only the paused wait sleeps (timed waits need Wine's fresh clock and a "
                "wait under 3 ms)");
}

/* Worker thread, every 10 s: game frames, game seconds and the wrappers' counters. */
void btw_perf_dump(void)
{
    static LONG last_frames, last_calls[2], last_waits[W_N], last_ms, last_late, last_wakes;
    static DWORD last_t;
    if (!g_exe_ok)
        return;
    DWORD now = GetTickCount();
    LONG frames = G_FRAMES;
    LONG c0 = g_ns.calls[0], c1 = g_ns.calls[1], w[W_N], ms = g_ns.wait_ms, late = g_ns.late;
    LONG wakes = g_ns.input_wakes;
    for (int i = 0; i < W_N; i++)
        w[i] = g_ns.waits[i];
    if (last_t && frames != last_frames) {
        DWORD dt = now - last_t;
        if (g_nospin)
            btw_log("perf: %lu ms: game frames +%ld | nospin calls mp +%ld sp +%ld, waits mp +%ld sp +%ld "
                    "paused +%ld, %ld ms waited, input wakes +%ld, late +%ld (max %ld us), wait cost %ld us",
                    (unsigned long)dt, frames - last_frames, c0 - last_calls[0], c1 - last_calls[1],
                    w[W_MP] - last_waits[W_MP], w[W_SP] - last_waits[W_SP],
                    w[W_PAUSED] - last_waits[W_PAUSED], ms - last_ms, wakes - last_wakes,
                    late - last_late, (long)g_ns.late_max_us, (long)g_cost_us);
        else
            btw_log("perf: %lu ms: game frames +%ld (nospin off)", (unsigned long)dt, frames - last_frames);
    }
    last_t = now;
    last_frames = frames;
    last_calls[0] = c0;
    last_calls[1] = c1;
    memcpy(last_waits, w, sizeof w);
    last_ms = ms;
    last_late = late;
    last_wakes = wakes;
}

void btw_perf_patch(void)
{
    const char *wine = wine_version();
    btw_log("perf: %s%s", wine ? "Wine " : "not under Wine", wine ? wine : "");
    patch_novsync(wine);
    patch_nospin();
}
