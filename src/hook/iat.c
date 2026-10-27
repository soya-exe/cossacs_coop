/*
 * Import Address Table patching of the main module (dmcr.exe).
 *
 * dmcr.exe imports WS2_32 by ORDINAL (the lookup thunks have IMAGE_ORDINAL_FLAG32
 * set), so we match on ordinals, not names. By the time our DllMain runs, the
 * loader has already resolved the exe's imports, so each IAT slot holds the real
 * ws2_32 function; we keep that value as the "real" pointer (so any earlier hook in
 * the chain keeps working) and write our hook in its place.
 *
 * Fallbacks: a by-name import of the same function is matched by name, and an exe
 * without an import lookup table (OriginalFirstThunk == 0) is matched by comparing
 * the slot with GetProcAddress(ws2_32, ordinal). The worker re-runs the patch every
 * second, so a slot that something else rewrites later gets hooked again (logged).
 */
#include "btwsteam.h"
#include <string.h>

btw_ws2_t g_real;

typedef struct {
    WORD        ord;
    const char *name;
    void       *hook;
    size_t      real_off;   /* offset of the matching pointer in g_real */
} hook_def_t;

#define H(o, n) { o, #n, (void *)btw_hook_##n, offsetof(btw_ws2_t, n) }
static const hook_def_t k_hooks[] = {
    H(2, bind),       H(3, closesocket), H(6, getsockname), H(10, ioctlsocket),
    H(17, recvfrom),  H(20, sendto),     H(21, setsockopt),  H(23, socket),
    H(52, gethostbyname), H(57, gethostname),
};
#undef H
#define NHOOKS (int)(sizeof k_hooks / sizeof k_hooks[0])

static void set_real(const hook_def_t *h, DWORD value)
{
    memcpy((char *)&g_real + h->real_off, &value, sizeof value);
}

static DWORD get_real(const hook_def_t *h)
{
    DWORD v;
    memcpy(&v, (const char *)&g_real + h->real_off, sizeof v);
    return v;
}

static int is_our_hook(DWORD v)
{
    for (int i = 0; i < NHOOKS; i++)
        if (v == (DWORD)(uintptr_t)k_hooks[i].hook)
            return 1;
    return 0;
}

/* Make sure every real pointer is usable even if the IAT patch fails. */
static void init_real_defaults(void)
{
    /* Loaded already: both dmcr.exe and this DLL import it. */
    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    for (int i = 0; i < NHOOKS; i++) {
        if (get_real(&k_hooks[i]))
            continue;
        FARPROC p = ws2 ? GetProcAddress(ws2, (LPCSTR)(uintptr_t)k_hooks[i].ord) : NULL;
        set_real(&k_hooks[i], (DWORD)(uintptr_t)p);
    }
}

static const hook_def_t *match(const BYTE *base, const IMAGE_THUNK_DATA32 *lookup, DWORD iat_value,
                               HMODULE ws2)
{
    for (int i = 0; i < NHOOKS; i++) {
        const hook_def_t *h = &k_hooks[i];
        if (lookup) {
            if (IMAGE_SNAP_BY_ORDINAL32(lookup->u1.Ordinal)) {
                if (IMAGE_ORDINAL32(lookup->u1.Ordinal) == h->ord)
                    return h;
            } else {
                const IMAGE_IMPORT_BY_NAME *ibn =
                    (const IMAGE_IMPORT_BY_NAME *)(base + lookup->u1.AddressOfData);
                if (strcmp((const char *)ibn->Name, h->name) == 0)
                    return h;
            }
        } else if (ws2) {
            if (iat_value == get_real(h) ||
                iat_value == (DWORD)(uintptr_t)GetProcAddress(ws2, (LPCSTR)(uintptr_t)h->ord))
                return h;
        }
    }
    return NULL;
}

int btw_iat_patch(int initial)
{
    static int defaults_done;
    if (!defaults_done) {
        init_real_defaults();
        defaults_done = 1;
    }

    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    const IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress)
        return 0;

    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    int changed = 0;
    for (const IMAGE_IMPORT_DESCRIPTOR *d = (const IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);
         d->Name; d++) {
        if (_stricmp((const char *)(base + d->Name), "ws2_32.dll") != 0 &&
            _stricmp((const char *)(base + d->Name), "ws2_32") != 0)
            continue;
        IMAGE_THUNK_DATA32 *iat = (IMAGE_THUNK_DATA32 *)(base + d->FirstThunk);
        const IMAGE_THUNK_DATA32 *lookup =
            d->OriginalFirstThunk ? (const IMAGE_THUNK_DATA32 *)(base + d->OriginalFirstThunk) : NULL;

        for (int i = 0; lookup ? lookup[i].u1.AddressOfData != 0 : iat[i].u1.Function != 0; i++) {
            DWORD cur = iat[i].u1.Function;
            const hook_def_t *h = match(base, lookup ? &lookup[i] : NULL, cur, ws2);
            if (!h || cur == (DWORD)(uintptr_t)h->hook)
                continue;
            if (IMAGE_SNAP_BY_ORDINAL32(cur) || cur == 0) {
                /* not resolved yet: nothing to chain to, try again later */
                if (initial)
                    btw_log("IAT: ws2_32 #%u (%s) not resolved yet", h->ord, h->name);
                continue;
            }
            if (!is_our_hook(cur))
                set_real(h, cur);
            DWORD old;
            if (!VirtualProtect(&iat[i].u1.Function, sizeof(DWORD), PAGE_READWRITE, &old)) {
                btw_log("IAT: VirtualProtect failed for #%u (%s), error %lu", h->ord, h->name,
                        (unsigned long)GetLastError());
                continue;
            }
            InterlockedExchange((volatile LONG *)&iat[i].u1.Function, (LONG)(uintptr_t)h->hook);
            VirtualProtect(&iat[i].u1.Function, sizeof(DWORD), old, &old);
            changed++;
            if (initial)
                btw_log("IAT: ws2_32 #%u %s: %08lx -> hook", h->ord, h->name, (unsigned long)cur);
            else
                btw_log("IAT: ws2_32 #%u %s was rewritten to %08lx by someone else; hooked again",
                        h->ord, h->name, (unsigned long)cur);
        }
    }
    return changed;
}
