# Performance: FPS, stutter and CPU use (measurement pass)

Date: 2026-10-28. Game: `bin/dmcr.exe` v1.35 with the shipped `csemu.dll` (sha256 `45a032b0...`).
Test copies c and d only (`tools/mkcopies.sh c d`), started headless through
`tools/run_instance.sh` with audio off (after commit 3ce2309 every launch was checked:
`/proc/<dmcr pid>/fd` showed no `/dev/snd`). Runner: system wine 11.20, wined3d on NVIDIA GL
(libGLX_nvidia 615.78) through gamescope's headless Xwayland (`-r 60`), i7-13620H.
No third-party code was used. No source file in `src/` was changed.

## Summary

- **0.1.3 implements proposals 1 and 2** (in memory, from `ichat.dll`, each with an off switch):
  see section 8 for what was patched and the before/after measurements.
- **0.1.4 makes proposal 1 (novsync) opt-in** (`BTWSTEAM_NOVSYNC=1`): with it on, a player's
  picture froze at the main menu (Proton Experimental, nested gamescope on Hyprland, NVIDIA);
  `BTWSTEAM_NOVSYNC=0` fixed it. Section 9.
- The game draws an **8-bit** paletted frame in system memory. `csemu.dll` (GSC's "Cossacks
  Emulator", 2011) is a complete DirectDraw wrapper. It forces a **32-bpp** display mode,
  converts 8 -> 32 bit on the CPU in its own render thread, and paces the game thread with an
  adaptive `Sleep` limiter at the monitor refresh rate (clamped to 40-100 Hz, else 60). Wine
  therefore never sees 8- or 16-bit DirectDraw. The "16-bit" assumption was wrong.
- The game runs at **60 presents/s** in every phase: menu, attract demo, random-map setup and
  skirmish. Real work on the game thread is only 2-7 ms per frame. There is a lot of headroom,
  and FPS is not GPU-bound.
- The large and erratic CPU cost is **csemu's render thread busy-waiting inside wined3d**. It
  locks the back buffer right after a vsync'd `Flip(DDFLIP_WAIT)`, and wined3d spin-waits
  until the previous present is done. The thread uses 3 % to 93 % of a core, changing from
  run to run and from screen to screen.
- **A one-byte change** (`DDFLIP_WAIT` -> `DDFLIP_WAIT|DDFLIP_NOVSYNC` in csemu's render loop),
  tested in copy c only, removes this. The render thread drops to 2-3 % in every phase, the
  menu total drops from 22-54 % to 14-16 % of a core, and in-game from 27-39 % to 22-27 %.
  Frame pacing is unchanged. It can be applied in memory from `ichat.dll` (proposal below).
- Stutter: 5 % of game frames take two refresh intervals (p95 frame interval 21-31 ms against
  a 16.7 ms target). This comes from csemu's limiter: integer-millisecond `Sleep` that drifts
  by +/-1 ms per frame and is not locked to vblank, while a second thread flips on vblank. Two
  unsynchronised 60 Hz clocks give periodic doubled frames.
- None of the Wine/Proton knobs helps as much. `csmt=0` makes CPU steady but costs more
  in-game. The `vulkan` renderer has the same spin. The `gdi` renderer does not start.
  `__GL_SYNC_TO_VBLANK=0` has no effect.
- **A ddraw wrapper such as cnc-ddraw cannot be dropped in.** csemu loads
  `GetSystemDirectory()\DDRAW.DLL` by full path. Verified: with `WINEDLLOVERRIDES=ddraw=n,b`
  and a test `ddraw.dll` in `bin/`, the game still loaded Wine's builtin ddraw.

## 1. Method and what to trust

- CPU: two snapshots of `/proc/<pid>/task/*/stat` (utime+stime) over a 12-25 s window per
  phase. Values are % of one core per thread class: game main thread, csemu render thread
  (the other `dmcr.exe` thread), `wined3d_cs`, wineserver, the instance's gamescope+Xwayland.
- Phases, all driven by XTEST input: main menu 16-28 s after start (before the attract demo
  starts after about 30 s idle); attract demo, a recorded battle, 20 s; Single Player ->
  Random Map setup screen, 12 s; skirmish against 2 Easy AIs, 25 s from 20 s after Start.
- Frame timing (traced runs only): `WINEDEBUG=+tid,+timestamp,+fps,+relay` with
  `RelayInclude=KERNEL32.Sleep;KERNEL32.SetEvent` in the copy's prefix. Each game present is
  csemu's `SetEvent` (return address `1000324c`), followed by csemu's limiter `Sleep(n)`
  (`100027d9`) on the game thread. That gives the present rate, the frame interval
  distribution, the limiter's sleep, and the game thread's wall time per frame. Wine's `+fps`
  channel gives the render thread's actual flips. Relay tracing adds some CPU, so CPU numbers
  below come from untraced runs unless marked.
- **Trustworthy here**: relative CPU between settings in the same phase (repeat runs given);
  the game-side present rate and frame-interval distribution (set by csemu's QPC/Sleep
  limiter, not by the display); every code-path finding.
- **Not trustworthy here**: GPU-bound FPS (there is none: the game is capped at 60 by csemu);
  how tearing or vsync look on a real display; the exact size of the render-thread spin.
  The spin depends on how long a vsync'd present blocks, which on a real display depends on
  the monitor refresh and the compositor (the user runs Proton Experimental 11.0 with
  `gamescope -w 1024 -h 768 -W 1920 -H 1080 -F fsr -f`). The spin mechanism is in wined3d
  and csemu, not in the headless setup, so it should exist on real hardware too. Its size
  there is unmeasured.
- Copies a and b of another agent were running during most measurements. The machine has 16
  threads, so contention is small, but it adds noise.
- The first runs, before commit 3ce2309, had Wine's ALSA driver loaded (HDMI device open).
  Those numbers are listed separately and not compared with later runs.

## 2. Baseline

Untraced, % of one core, three runs after the audio fix (base2, base3, base4):

| Phase | dmcr.exe total | main thread | csemu render thread | wined3d_cs |
|---|---|---|---|---|
| main menu | 29.7 / 22.2 / 30.3 | 3.3-3.7 | 18.0 / 11.5 / 18.7 | 7-8 |
| attract demo | 44.4 / **101.7** / 49.5 | 32-37 | 2.6 / **57.0** / 10.5 | 7-8 |
| random-map setup | 48.7 / 59.5 / 15.0 | 3.8-4.9 | 37.8 / 45.4 / 2.6 | 7-9 |
| skirmish | 26.9 / 27.0 / 38.7 | 15-22 | 3.0 / 2.8 / 5.4 | 9-12 |

Plus 1-6 % for wineserver, about 2 % for gamescope+Xwayland, and 2-12 % for the prefix's
other wine processes (explorer, services). Before the audio fix, one run gave menu 53.7,
demo 103.1, setup 103.5 and game 35.4, with the render thread at 40 / 52 / 93 / 8.

Frame timing, traced baseline run:

| Phase | presents/s | frame interval mean / p50 / p95 / p99 / max (ms) | csemu Sleep(n) mean | game thread wall time per frame, mean / p95 (ms) |
|---|---|---|---|---|
| menu | 60.1 | 16.7 / 17 / 21 / 23 / 28 | 15.9 | 2.4 / 6 |
| demo | 58.2 | 17.2 / 17 / 26 / 33 / 44 | 9.6 | 7.3 / 19 |
| setup | 60.4 | 16.6 / 17 / 30 / 32 / 32 | 15.7 | 5.3 / 14 |
| skirmish | 59.8 | 16.7 / 17 / 30 / 31 / 32 | 14.7 | 7.4 / 15 |

Wine `+fps` (render thread flips, `wglSwapBuffers`): 57.9-60.0 fps in every phase.

The game's own "FPS:%d" string (0x5acd98, printed at 0x4c798d in a debug panel in
FUN_004c5b00) reads 0x81d8c8. That variable is only ever written with 0 at start-up
(0x4b256b, 0x4b0fd3), so it is a dead debug counter, not a usable FPS display.

## 3. How the game renders and paces frames

### dmcr.exe

- `FlipPages` (export, **0x44f970**). If the GDI path flag (0x659904) is set, it uses
  `StretchDIBits`. Otherwise it copies the 8-bit back buffer `ScreenPtr` (0x65a9b0) into
  `RealScreenPtr` (0x65a9b4) row by row (`rep movsd`, 1024x768 = 768 KB), then at
  **0x44fac9** calls `call [edx+0x28]` on the DirectDraw object (global **0x65a1c0**).
  Slot 0x28 is `IDirectDraw::FlipToGDISurface`. GSC uses it as the "frame done" signal for
  csemu.
- `ProcessMessages` (export, 0x4717a0) is a non-blocking `PeekMessageA(PM_REMOVE)` pump. It
  never sleeps.
- `Sleep` (IAT 0x59e090) is used only by the intro videos (0x4b0b10, `Sleep(30)`) and the
  GameSpy SDK (wrapper 0x551f10). `GetTickCount` (IAT 0x59e1b4, wrapper 0x5250e0) is used
  everywhere. QPC/QPF appear only in lobby code (0x47d450). There is no `timeGetTime`. The game
  imports no ddraw functions itself: only `DirectDrawCreate` from csemu.dll (IAT
  **0x59e024**).
- In-game frame function **0x4b1530** (called from 0x48a8c0 / 0x48aed0):
  - Multiplayer (`players >= 2`, 0x5d6f08): wait loop **0x4b158c-0x4b15e7**. Until
    `GetTickCount()` reaches the next tick (0x8c0e7c, advanced by 0x5ac188 per frame), it
    renders extra frames (0x4c8740 draw + 0x485c00 RefreshView -> FlipPages, each paced by
    csemu's Sleep) while the remaining time exceeds a moving average of their cost (0x5ac18c).
    Otherwise it **spins** on `call ProcessMessages` at **0x4b15d7** plus GetTickCount.
    (Section 8: the loop runs only when the room's max ping is >= 50 ms, 0x8c0e68.)
  - Single player: end-of-frame wait **0x4b1964-0x4b199b**, a **busy loop** on
    `call ProcessMessages` (0x4b1964) and GetTickCount until `speed/2` ms have passed since the
    last frame, **or for as long as the game is paused** (flag 0x8c0e14; correction in section 8:
    nothing sets that flag, and the Pause key does not use this loop). `speed` is 0x6dd7e4 =
    mode.dat field 7 (default 50 without mode.dat, clamped to <= 100 at 0x484452; 7 in this
    install, so the wait is 3.5 ms and is normally already over after csemu's sleep).
  - Single-player simulation appears to advance once per call, so **SP game speed follows the
    present rate** that csemu's limiter sets. Any limiter change must not lower the in-game
    frame rate in SP.
- Another busy wait: 0x47d450 (lobby) shows a message for up to 3 s in a `ProcessMessages`
  loop with no sleep.
- mode.dat (`1024 768 85 0 0 18 7 6 1 2`): width, height, 0x6d8a7c, 0x6dbc64 (twice), 0x6da6e0
  (18; csemu also reads this 6th number as the CD-music volume %), **game speed delay 0x6dd7e4
  (7)**, 0x5acc74, 0x5ac17c, 0x5aa01c. The mode must be in the enumerated mode list, else
  1024x768. There is no colour depth field.

### csemu.dll ("Cossacks Emulator" 1.0.0.3, "Intel Laptop Gaming TDK v2.1, libOgg 1.2.0, libVorbis 1.3.1")

Ghidra project `re/ghidra-csemu` (gitignored). Exports: `DirectDrawCreate`, winmm `mci*`,
`mixer*`, `mmio*` and `waveInGetDevCapsA` (CD audio -> `Track_%02u.ogg` through DirectSound),
and the IntelLaptopGamingTDKInterface class.

- `DirectDrawCreate` **0x10001200**: LauncherInterfaceCheck (0x10001140, the `CSBTW_LAUNCHER_*`
  event), then LoadNativeDirectDraw **0x10001090**: `GetSystemDirectoryA() + "\DDRAW.DLL"`,
  **`LoadLibraryA` by full path**, `GetProcAddress("DirectDrawCreate")`. The real object is
  wrapped in `IMyDirectDraw` (0x60 bytes, ctor 0x10002cc0, vtable **0x1003a3ec**).
- Ctor: reads the monitor EDID from `HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY\...` (via
  `EnumDisplayDevicesA`, 0x10001660/0x100017f0). If valid, it scales to the native resolution
  (+0x20/+0x24) and hooks the mouse (WH_GETMESSAGE 0x10001c90). This is probably inactive
  under Wine (no EDID key); not verified.
- `SetDisplayMode` **0x100027f0**: always calls the real `SetDisplayMode(w, h, 32)` and ignores
  the game's bpp. Then `GetMonitorFrequency`: outside 40..100 Hz -> 60. The limiter period is
  `QPF / refresh` (0x100025d0).
- `EnumDisplayModes` 0x100026c0: passes through, or only 1024x768 plus native when scaling.
- `CreateSurface` **0x100028e0**: creates the real primary flip chain (caps
  PRIMARY|FLIP|COMPLEX, 1 back buffer), loads `data\overlay.dat` into an offscreen surface,
  picks an 8->32 converter by CPUID/alignment (0x10001d60, 0x10001eb0, SSE2 0x10002010,
  generic 0x100021a0), and mallocs an 8-bit buffer `width*(height+128)`. The game's surface is
  `IMyDirectDrawSurface` (vtable 0x1003a44c): `Lock` (0x10002f90) returns that buffer with
  pitch = width. `Unlock`, `Flip` (0x10002e40), `SetPalette` and others are no-ops.
- `CreatePalette` 0x100024e0: stores the palette (BGRA table 0x10054e50) and starts the render
  thread (0x100031e0). `SetEntries` 0x10002350 updates it.
- **Present + limiter** = vtable slot 10 (`FlipToGDISurface`, slot address **0x1003a414** ->
  **0x10002760**): QPC since the last call. If shorter than one refresh period, the sleep
  value (+0x2c) goes up by 1 ms; if longer, down by 1 ms. Then `SetEvent` (wake the render
  thread) and `Sleep(n)`. There is no vsync lock and it works in integer milliseconds.
- **Render thread** **0x10003040**, loop 0x100030b0-0x100031b1, under a critical section:
  `GetAttachedSurface(BACKBUFFER)` (0x100030c9, never Released: a reference leak each frame),
  `Lock(backbuffer, DDLOCK_WAIT)` (0x100030f8), convert 8 -> 32 bit (0x10003120), `Unlock`,
  optional stretch `Blt`, laptop overlay 0x100019e0, then
  **`Flip(NULL, DDFLIP_WAIT)` at 0x1000318c** (`6a 01` at **0x10003187**, file offset 0x2587).
- Power / laptop: csemu imports POWRPROF, Wlanapi and IPHLPAPI. `SetCurrentPowerScheme`
  (0x10004d00 -> 0x10005350) is only exported: neither csemu nor dmcr.exe calls it, so **no
  power-scheme change happens**. The battery / Wi-Fi overlay is drawn only after the `.` key
  (VK_OEM_PERIOD, hook 0x10001c10 toggles 0x10054e34) and only on a laptop (TDK `IsLaptop`).
  Off by default.

### Why the render thread burns CPU

On each game present the render thread `Lock`s the back buffer it flipped one frame earlier.
In wined3d (CSMT on, the default) a map of a resource still used by queued commands waits
for the command stream. Those commands are the previous vsync'd present. The wait is a
`YieldProcessor` spin, not a sleep. How long it spins depends on the phase between csemu's
Sleep-paced signal and the display's vblank, which is why it ranges from 3 % to 93 % and
changes between screens and runs. Evidence: `DDFLIP_NOVSYNC` alone brings the thread to 2-3 %
everywhere (table below); `csmt=0` replaces the spin with a steady ~20 %. The NVIDIA GL
variable `__GL_SYNC_TO_VBLANK=0` changes nothing, because wined3d sets the swap interval
itself from the Flip flags. A `winedbg` attach cannot be used to sample stacks: it trips
BugTrap's crash handler.

## 4. Settings tried

Before = baseline untraced ranges above (base2-4). One cell per run.

| Setting | menu | demo | RM setup | skirmish | render thread | Notes |
|---|---|---|---|---|---|---|
| baseline (wined3d GL, CSMT on) | 29.7 / 22.2 / 30.3 | 44.4 / 101.7 / 49.5 | 48.7 / 59.5 / 15.0 | 26.9 / 27.0 / 38.7 | 2.6-57 | erratic |
| **csemu Flip `DDFLIP_NOVSYNC`** (1 byte, test copy only) | **14.2 / 15.4 / 15.8** | 50.2 / 51.4 / 43.6 | **16.2 / 15.7 / 12.4** | **26.8 / 25.1 / 22.0** | **2.1-3.1** | 60 presents/s, renders correctly; traced p95 24-31 ms, p99 30-33 ms (same as baseline) |
| `HKCU\Software\Wine\Direct3D` `csmt`=0 | 21.8 / 23.5 / 30.2 | 53.9 / 54.5 / 65.2 | 22.8 / 21.2 / 28.5 | 39.1 / 38.5 / 40.2 | 15.8-24.3 | steady, no spikes, but in-game +10 points; traced p95 20-23 ms, p99 24-29 ms (slightly better) |
| `renderer`=vulkan (1 traced run, before the audio fix) | 94.2 | 55.2 | 16.7 | 71.0 | 2.3-80.6 | same spin, worse in-game |
| `renderer`=gdi | - | - | - | - | - | game does not start: "Direct Draw Init Failed (88760233)" (DDERR_NODIRECTDRAWHW) |
| `__GL_SYNC_TO_VBLANK=0` (1 traced run, before the audio fix) | 61.7 | 61.4 | 107.7 | 37.4 | 9.9-94.7 | no effect: wined3d sets the swap interval itself |
| `WINE_FULLSCREEN_FSR` | not testable | | | | | Proton's fullscreen hack only; system wine 11.20 lacks it, and headless the game mode equals the output. The user already scales with gamescope `-F fsr`, which costs no game CPU. |
| `PROTON_USE_WINED3D` | no-op | | | | | ddraw always goes through wined3d in Proton (DXVK has no ddraw) |
| mode.dat speed (field 7) | not a perf knob | | | | | changes SP game speed only when > 33 (speed/2 > 16.7 ms) |
| mode.dat resolution | not measured | | | | | csemu converts w*h bytes per frame; 800x600 would cut that by 39 % but changes the game view |

## 5. Proposals (1 and 2 implemented in 0.1.3, see section 8)

All of these would live in our `ichat.dll`, which dmcr.exe loads as an import, so csemu.dll is
already mapped when it initialises. Apply them only under Wine (`ntdll!wine_get_version`
present), behind an opt-out environment variable, and verify the bytes before patching. Never
ship a patched csemu.dll: the file stays as Steam installed it.

1. **csemu Flip without vsync (in memory).** `base = GetModuleHandleA("csemu.dll")`. Check
   that the bytes at `base+0x3187` are `6a 01 6a 00 50 ff d2` (`push 1; push 0; push eax;
   call edx` = `Flip(NULL, DDFLIP_WAIT)`). Then `VirtualProtect` and write `0x09` at
   `base+0x3188` (`DDFLIP_WAIT|DDFLIP_NOVSYNC`). It is one byte, idempotent, and needs no
   relocation (RVA-relative). Do it before the first `DirectDrawCreate`, which is any time in
   ichat's init. Effect measured above. Pacing is unchanged because the game is paced by
   csemu's limiter, not by the flip. Risk: possible tearing on an X11 session without a
   compositor. Under gamescope (the user's launch options) or a compositing desktop, frames
   are composited whole. On Windows the patch must stay off (real fullscreen ddraw would tear).
2. **Sleep instead of spin in the game's wait loops.** Redirect the two `e8 rel32` calls to
   `ProcessMessages` at **0x4b15d7** (MP tick wait) and **0x4b1964** (SP end of frame / pause)
   to an ichat function. It calls `ProcessMessages()` (0x4717a0) and returns its result. If no
   message was handled and the loop's deadline is at least 2 ms away, it first waits
   `MsgWaitForMultipleObjects(0, NULL, FALSE, 1, QS_ALLINPUT)`. Deadlines: MP `0x8c0e7c -
   GetTickCount()`; SP `0x6dd7e4/2 - (GetTickCount() - 0x8c0e54)`; SP paused (0x8c0e14 != 0)
   always 1 ms. Loop conditions stay untouched, so lockstep timing keeps millisecond accuracy.
   Benefit: removes the MP waiting spin (not measured here; needs a c+d match) and the 100 %
   core spin while an SP game is paused. Risk: low; at worst 1 ms extra latency per wait.
3. **Better pacing (stutter).** Replace csemu's limiter by hooking the present. Either patch
   the vtable slot at `csemu+0x3a414` (.rdata) or the call at dmcr 0x44fac9 to an ichat
   function. That function calls the original 0x10002760 for the SetEvent, but paces with a
   QPC deadline (`deadline += period`, sleep to deadline-1 ms, then a short spin or
   `Sleep(0)`) instead of the +/-1 ms integer controller. That would bring p95 frame time
   close to 16.7 ms. It keeps 60 Hz, so SP game speed is unchanged. Risk: medium (timing
   code); test SP speed against wall clock.
4. **Menu-only 30 fps cap**, in the same present hook, when `GameInProgress` (dmcr export
   `?GameInProgress@@3_NA`) is false. Halves menu frame work. After (1) the menu is already
   at about 15 % of a core, so the gain is small. Low priority.
5. If (1) is rejected: write `HKCU\Software\Wine\AppDefaults\dmcr.exe\Direct3D` `csmt`=dword:0
   (from ichat.dll before csemu loads wined3d, or from install.sh). Measured: removes the
   spikes but raises in-game CPU (+10 points). Only worth it if menus matter more than games.

### Ranking (expected benefit vs risk)

| # | Change | Benefit | Risk |
|---|---|---|---|
| 1 | csemu Flip NOVSYNC byte, in memory, Wine only | high: render thread 3-93 % -> 2-3 %; menus about -50 %, game about -20 % | low (tearing only without a compositor) |
| 2 | Sleep in the MP/SP wait loops | medium for MP and paused SP (unmeasured) | low |
| 3 | QPC deadline limiter | fixes the 5 % doubled frames (stutter) | medium |
| 4 | Menu 30 fps cap | small | low |
| 5 | `csmt=0` AppDefaults | steadier, but worse in game | low |
| - | Proton/Wine renderer and env knobs | none or negative | - |

## 6. A DirectDraw wrapper such as cnc-ddraw (UNVERIFIED, from documentation only)

What cnc-ddraw (MIT, FunkyFr3sh) normally gives old 8-bit ddraw games: palette conversion on
the GPU (OpenGL / D3D9 shader), its own render thread with a configurable `maxfps` limiter and
vsync, windowed / borderless mode, aspect-correct and integer scaling with shaders, and fixes
for mouse clipping and alt-tab.

What it would mean for this game:

- **It cannot be dropped in.** Verified in copy d: with a test `ddraw.dll` (our own marker
  DLL, not cnc-ddraw) in `bin/` and `WINEDLLOVERRIDES=ddraw=n,b`, the process mapped only
  `/usr/lib/wine/i386-windows/ddraw.dll` and the marker DLL never loaded. csemu loads
  `system32\DDRAW.DLL` by absolute path. The Proton launch option alone does nothing.
- Placing it under csemu (in the prefix's system32) is not ours to ship, and cnc-ddraw would
  only see csemu's 32-bpp flip chain. CPU conversion and the limiter would stay in csemu.
- Replacing csemu: `ichat.dll` would have to point dmcr's `DirectDrawCreate` IAT slot
  (0x59e024) at cnc-ddraw. Then:
  - csemu's launcher check (the `CSBTW_LAUNCHER_*` event) is bypassed. Decide whether that is
    acceptable.
  - The game's present is `FlipToGDISurface` on a primary it writes through `RealScreenPtr`.
    Whether cnc-ddraw handles that cleanly is unknown.
  - The game thread would no longer sleep in present. SP speed follows the frame rate (speed
    delay 7 -> 3.5 ms per frame), so **SP would run several times too fast** unless cnc-ddraw's
    limiter throttles exactly this call. This is a high-risk area.
  - CD music (csemu's winmm exports) would be unaffected.
- Costs: shipping a third-party DLL plus its MIT notice in both installers (Windows and
  Linux); the IAT redirect code; a `ddraw.ini` to maintain; testing on Windows and Proton;
  support load. Scaling is already covered for the user by gamescope `-F fsr`.
- Expected gain over proposal 1 is small for CPU (the 8 -> 32 conversion is about 2 % of a
  core) and uncertain for pacing. Recommendation: do not adopt unless proposals 1-3 prove
  insufficient.

## 7. Side findings

- The harness was not silent on wine 11.20: with `BTW_AUDIO=off`, Wine's ALSA driver still
  loaded and opened the NVIDIA HDMI PCM devices directly. This was reported and fixed in
  3ce2309 (`Audio=""`, null ALSA config, `/dev/snd` guard). Every later launch in this pass
  was checked with no `/dev/snd` handle.
- csemu calls `GetAttachedSurface` every frame without `Release` (reference leak, harmless in
  practice).
- Copies c and d were restored: original `csemu.dll` (sha256 45a032b0...) and `ichat.dll`, test
  `ddraw.dll` removed, prefix-c `csmt` / `renderer` / `RelayInclude` values removed.
  `Audio=""` is left in place.

## 8. Implemented (0.1.3)

Proposals 1 and 2 are in `src/hook/perf.c` (details and byte lists in `src/hook/README.md`,
"How it works" 6). Both run from `ichat.dll`'s DllMain, change memory only, check their bytes
first, and log and skip on any mismatch. Proposal 3 is not implemented; see the design note at
the end of this section.

| Switch | Default | What it does |
|---|---|---|
| `BTWSTEAM_NOVSYNC=0` | on under Wine/Proton, never on Windows (**0.1.4: off; `BTWSTEAM_NOVSYNC=1` turns it on**, section 9) | csemu+0x3188 `01` -> `09` (`Flip(NULL, DDFLIP_WAIT\|DDFLIP_NOVSYNC)`) |
| `BTWSTEAM_NOSPIN=0` | on | `call ProcessMessages` at 0x4b15d7 (MP tick wait) and 0x4b1964 (SP frame wait) -> wrappers that wait ~1 ms when only waiting |

### What was patched

- **novsync.** `GetModuleHandleA("csemu.dll")` in our DllMain returns `10000000`: csemu.dll is a
  static import of dmcr.exe, so it is mapped before our DllMain runs (confirmed in every run's
  log: `novsync: csemu.dll at 10000000 (mapped before our DllMain)`). Its render thread only
  starts in `CreatePalette`, much later. Checks: file size 357056, PE timestamp `4d26ff86`, image
  size `5c000`, PE checksum `59355` (that is the build with sha256 `45a032b0...`), and the 15
  bytes at csemu+0x317f `8B 08 8B 51 2C 83 C4 0C 6A 01 6A 00 50 FF D2` (no relocated address
  inside). Then `VirtualProtect`, write `09` at +0x3188, restore, `FlushInstructionCache`. Wine
  is detected by `ntdll!wine_get_version`.
- **nospin.** Checks: dmcr.exe at 0x400000 with timestamp `4d2c8aaf` and image size `2317000`,
  and the bytes of the MP loop 0x4b1570-0x4b15e9 (121), the SP loop 0x4b1948-0x4b199d (85),
  `ProcessMessages`' prologue (24) and the clock function's head and tail (40 + 7). Then the two
  `E8 rel32` (`E8 C4 01 FC FF`, `E8 37 FE FB FF`) get new targets: an asm thunk that passes EDI
  (the tick deadline) and BL (the "render extra frames" flag) to the MP wrapper, and a plain
  cdecl SP wrapper (that loop reads only globals). A wrapper waits only when the loop is just
  waiting (MP: BL=0; with BL=1 the loop renders frames and never spins), then calls the real
  `ProcessMessages`. The loop conditions are untouched.

### Findings made while implementing

- **Wine's `GetTickCount` is only as fresh as the last wineserver request.** It reads the tick
  from the shared user data, which wineserver updates when it handles a request from any
  process of the prefix. In an idle test program it stepped by exactly 16 ms; in the same
  prefix while the game ran at the menu it stepped by 1-6 ms (about 340 updates/s), whatever
  the program itself did (spin, `PeekMessage`, `Sleep(1)`, `timeBeginPeriod`). Wine's
  `QueryPerformanceCounter` (10 MHz) is the same monotonic clock read directly: `QPC/10000`
  equals `GetTickCount()` plus 0-16 ms. So the game clock (0x5250e0 = `GetTickCount()` + a
  slewed MP offset/128) lags real time by up to a few ms, and the original loops leave "at the
  deadline" only to within that lag.
- **Wine 11's `MsgWaitForMultipleObjectsEx` with a 1 ms timeout returns at once** (WAIT_TIMEOUT
  after 4-14 us on average); with 2 ms it waits ~1.0 ms (mean 996 us, max 1053 us). `Sleep(1)`
  and `WaitForSingleObject(1 ms)` take 1.03-1.07 ms. The hook measures this at start and uses
  the shortest timeout that really waits (2 ms under Wine 11.20).
- **The SP "paused" branch is dead code in v1.35.** 0x8c0e14 is only read in the loop
  (0x4b1969, 0x4b1994) and cleared in 0x4af000 (0x4af102, 0x4afced); nothing sets it (Ghidra
  xrefs and a byte search agree). The game's Pause key (`PAUSE` banner) keeps calling the frame
  function at 60 frames/s, paced by csemu's limiter; it does not spin. Section 3 and proposal 2
  assumed a 100 % spin while paused; that was wrong. The wrapper still sleeps if the flag is
  ever set.
- **The SP end-of-frame wait only spins when mode.dat's speed (field 7) is above ~33.** With
  this install's 7 it waits 3 ms, which csemu's `Sleep` has already covered: the SP wrapper is
  called once per frame and never waits (59 calls/s, 0 waits). With speed 50 (25 ms per frame,
  as without a mode.dat, or a slower game speed setting) it spins ~9 ms of every 26 ms frame and
  the main thread runs at 99 % of a core, in game and while paused.
- **The MP tick wait runs only when the room's max ping is >= 50 ms** (0x8c0e68, see the
  multiplayer results below), and even then it spins only when less than a quarter of the
  extra-frame cost (0x5ac18c/4, a few ms) is left before the tick; otherwise it renders extra
  frames (BL=1, paced by csemu).

### How a timed wait keeps the game's timing

A wait may start only if it ends before the game clock can reach the deadline. Under Wine the
wrapper computes the exact time left from the fresh clock: `(deadline - offset/128) * 1000 -
QPC_us` (the game's own clock is never ahead of that). It waits only if that is at least the
longest recent wait plus 0.36 ms (0.2 ms margin + one 0.16 ms slewing step of the MP offset).
The longest recent wait is measured on every wait (it jumps up at once and decays by 1/64 per
wait). The game's clock function is not called (it would slew the MP offset more often); its
result is reproduced as `GetTickCount() + (offset >> 7)`. Timed waits switch on only if the
worker's start-up check finds `0 <= QPC_ms - GetTickCount() <= 50` and a wait under 3 ms, which
is the case under Wine. **On Windows only the paused wait sleeps** (no deadline there): its
`GetTickCount` steps by ~15.6 ms and a 1 ms wait can take up to 15.6 ms, depending on the
system timer resolution, so a timed wait could move a lockstep tick; the hook cannot know the
time left precisely enough there. `MWMO_INPUTAVAILABLE` makes every wait return at once when
input is queued, so input is handled as fast as before. The `perf:` line counts waits that end
after the fresh deadline (`late`).

### Measurements

Same method as sections 1-2: copies c and d with our hook and the mock `steam_api.dll`, headless
and silent (`tools/run_instance.sh`, `BTW_AUDIO=off`; every run checked: no `/dev/snd` handle at
start and end, no "AUDIO GUARD" in run.log), per-thread CPU from `/proc` (12-25 s per phase,
% of one core), XTEST input. "off" = `BTWSTEAM_NOVSYNC=0 BTWSTEAM_NOSPIN=0` (same as 0.1.2),
"on" = defaults. The off and on copies ran at the same time, swapping copies between runs.
No other test copies ran. From 15:24 the user played another Proton game on this PC: SP runs 1-3
were before that; the speed-50, traced and multiplayer runs ran alongside it (more noise).

**Menus and single player (mode.dat speed 7, as installed)**, dmcr.exe total (csemu render
thread), three runs each:

| Phase | off | on |
|---|---|---|
| main menu | 55.7 (43.6) / 35.2 (18.7) / 30.0 (12.2) | **16.9 (3.0) / 19.9 (3.3) / 20.7 (3.5)** |
| attract demo | 113.4 (76.4) / 56.1 (5.7) / 95.9 (54.0) | **37.1 (1.9) / 54.4 (2.1) / 44.5 (2.0)** |
| random-map setup | 24.0 (5.8) / 23.6 (4.0) / 24.6 (4.2) | 20.7 (3.5) / 24.3 (4.2) / 22.4 (3.7) |
| skirmish vs 2 Easy AI | 34.7 (3.1) / 41.8 (12.9) / 32.8 (4.3) | **29.1 (3.0) / 33.4 (3.1) / 27.9 (2.9)** |
| skirmish, paused (Pause key) | 43.8 (16.1) / 32.8 (3.8) / 24.3 (2.6) | 27.4 (2.9) / 33.6 (3.2) / 22.6 (2.5) |
| traced run (relay): menu / demo / setup / game / paused | 20.2 / 83.6 / 91.4 / 33.6 / 28.4 | 17.2 / 42.8 / 14.2 / 29.0 / 29.0 |

The render thread is at 1.9-4.2 % in every phase with the patch, against 2.6-80 % without; the
off column is as erratic as in section 2. The main thread is unchanged (demo 28-44 %, game
13.6-20.6 %, the game's own work). Plus 2-3 % wineserver and ~2 % gamescope+Xwayland in both.

**SP game speed** (2 min 10 s of skirmish, screenshots of the in-game clock 130.4 s of wall time
apart; game frames from the hook's `perf:` lines over the whole ~3.5 min in game):

| Run | game time | bracket counter | game frames/s |
|---|---|---|---|
| off1 / on1 | 0:00:44 -> 0:02:54 / same | 107 -> 418 / 107 -> 418 | 59.09 / 59.04 |
| on2 / off2 | 0:00:44 -> 0:02:54 / same | 107 -> 418 / 107 -> 420 | 59.01 / 58.99 |
| off3 / on3 | 0:00:44 -> 0:02:54 / same | 107 -> 418 / 107 -> 419 | 58.84 / 58.98 |
| speed 50: off / on | 0:00:44 -> 0:02:54 / same | 68 -> 270 / 69 -> 270 | **38.63 / 38.66** |

The displayed game time follows wall time; the bracketed number after it advances with the
simulation and is the same with and without the patches. Frames/s per 10 s window stayed within
38.4-38.8 in both speed-50 runs.

**Slow SP speed (mode.dat speed 50; exercises the SP timed wait)**, novsync off in both:

| | off | on |
|---|---|---|
| skirmish: dmcr.exe / main thread | 105.6 / 98.9 | **24.3 / 17.6** |
| paused: dmcr.exe / main thread | 105.5 / 99.3 | **26.3 / 19.9** |
| SP wrapper calls/s, waits/s | - | 79676, 844 (81.5 % of the time waiting) |
| waits past the fresh deadline | - | 1 of ~160 000 (35 us) |

**Frame pacing** (traced runs, relay of csemu's `SetEvent`/`Sleep`, as in section 2): unchanged.

| Phase | off: presents/s, interval p50 / p95 / p99 / max ms | on |
|---|---|---|
| menu | 60.1, 17 / 19 / 21 / 21 | 60.1, 17 / 19 / 21 / 22 |
| demo | 59.6, 16 / 21 / 25 / 33 | 59.4, 16 / 22 / 25 / 31 |
| setup | 60.1, 17 / 19 / 21 / 22 | 60.0, 17 / 19 / 21 / 23 |
| skirmish | 59.9, 16 / 20 / 21 / 23 | 59.8, 16 / 20 / 22 / 23 |
| paused | 60.0, 16 / 20 / 21 / 22 | 60.0, 16 / 20 / 21 / 23 |

**Multiplayer** (copies a = host, b = joiner over the hook and the mock Steam,
`tools/net_latency.sh` through `MATCH_SECS=130`, plus per-thread CPU 22-52 s into the match; the
user was playing another Proton game on this PC during these runs, so CPU figures carry a few
points of noise):

| | no lag: off (mpoff2) | no lag: on (mpon2) | lag 40 ms: off | lag 40 ms: on |
|---|---|---|---|---|
| room max ping | 14-16 ms (earlier off run: 16-20) | **14-16 ms** | 96-99 ms | 96-98 ms |
| match length, both copies at the end | 0:02:09 [150] / [151] | 0:02:09 [147] / [147] | 0:02:04 [149] / 0:02:05 [148] | 0:02:05 [148] / 0:02:04 [149] |
| game frames/s, a / b (frame function, 10 s windows) | 28.91 / 28.93 | 25.9 (includes the joiner kill) / 28.26 (one window 26.4) | 32.3 in every steady window | 32.3 in every steady window |
| drop counters (tx and rx, both copies) | all 0 | all 0 | all 0 | all 0 |
| receive Steam -> game p50 / p95 (a) | 10.3 / 16.1 ms | 8.3 / 16.4 ms | 8.3 / 16.2 ms | 9.3 / 16.0 ms |
| dmcr.exe, net_latency 45 s window (a / b) | 24.9 / 24.8 % | 23.5 / 30.0 % | 32.5 / 31.2 % | 35.0 / 33.5 % |
| main / render thread, cpu2 (a) | 9.9 / 2.9 | 10.1 / 2.9 | 12.1 / 10.3 | 18.8 / 3.3 |
| main / render thread, cpu2 (b) | 9.6 / 3.2 | 17.5 / 2.6 | 14.0 / 4.7 | 17.5 / 3.3 |
| MP wrapper calls/s, waits/s | - | 0, 0 | - | ~5 700, 2 (0.2 % of the time) |

- **No lag: the MP tick wait never runs.** The frame function enters it only when 0x8c0e68 is
  set, and the room code (0x47fbc3, 0x483292, copied to the joiners in the start packet at
  0x408b3c) sets it to `max(max ping, 300) + 500` ms only when the room's max ping is >= 50 ms;
  below that it is 0 and the game's tick pacing is elsewhere. So on one PC (ping 15 ms) only
  novsync acts in a match.
- **With 40 ms lag (ping ~97 ms) the loop runs** but spins briefly: it spins only when less
  than a quarter of the extra-frame cost is left before the tick, and most of those spins are
  shorter than the ~2.2 ms a wait needs (in game, Wine's 2 ms `MsgWait` took up to 1.8-1.9 ms).
  The wrapper waited twice a second; the game ran exactly as without it (32.3 frames/s,
  same match clock, 0 late waits). The CPU saving in MP is therefore small; see the A/B pair
  below for the main thread.
- The kill-joiner check (`KILL_JOINER=1`, patches on) behaves as in 0.1.2: the host shows the
  game's PAUSE "HostA: Ready / JoinB: Not responding." at +6 s and +15 s, "Press F3 to
  disconnect from the game." at +40 s, and plays on at +55 s.
- **A/B for nospin alone** (lag 40 ms, 100 s match, `BTWSTEAM_NOVSYNC=0` in both runs so only
  the wait loop differs): main thread 14.0 / 12.6 % (a / b) with nospin, 15.6 / 16.8 % without;
  32.3 game frames/s in every steady window in both; 1 wait/s, 0 late; all drop counters 0.
  (The dmcr.exe totals of this pair, 26-44 %, follow the vsync spin of the render thread, which
  was on in both.)

### Design note: proposal 3, an exact-deadline frame limiter (not implemented)

- **Hook point.** Redirect the `call [edx+0x28]` at dmcr 0x44fac9 (FlipPages -> csemu
  `FlipToGDISurface`, 0x10002760) to an ichat function, checking the bytes as in the two
  patches above. The vtable slot csemu+0x3a414 would work too, but it is in .rdata and is
  shared by every caller; the call site affects only the game's own present.
- **What the hook does.** Call the original first, with csemu's sleep value (object +0x2c)
  forced to 0 before the call, so csemu still wakes its render thread (`SetEvent`) and keeps
  its bookkeeping, but its `Sleep(n)` becomes `Sleep(0)`. Then pace on a QPC deadline:
  `next += period`; if `now > next + period` (a load hitch), set `next = now` instead of
  catching up with a burst. Wait in 1 ms steps while more than ~1.5 ms remain (Wine:
  `Sleep(1)` = 1.07 ms, measured above), then `SwitchToThread`/`Sleep(0)` until `next`.
  Never use GetTickCount for this: under Wine it lags by 0-16 ms (see above).
- **Period.** Exactly csemu's: `QPF / refresh`, with `refresh` from `GetMonitorFrequency`
  clamped to 40-100 Hz as in `SetDisplayMode` 0x100027f0 (read the value csemu stored rather
  than recomputing it). SP game speed follows the present rate, so the long-run rate must
  stay at 60.0/s; the deadline arithmetic (no `+/-1 ms` controller) keeps it exact.
- **Expected effect.** The game-side frame interval would become 16.7 ms +/- ~0.3 ms instead
  of the controller's 15-18 ms oscillation. With novsync the render thread flips as soon as it
  is woken, so there is no second (vblank) clock left to beat against; the remaining doubled
  frames (p95 21-31 ms) should mostly go. On a real display the compositor's own vblank is
  still a second clock: gamescope/Hyprland latch the newest frame, so an occasional frame may
  still be held for two refreshes, but much less often than with two drifting 60 Hz clocks.
- **Cost.** The last ~0.5 ms per frame spins (about 3 % of a core at 60 Hz), roughly what
  csemu's `Sleep` jitter costs today.
- **Tests.** Frame interval p50/p95/p99 from the relay trace (method in section 1); SP game
  clock against wall time over 2+ minutes (as in the table above, must stay 1.000); MP match in
  sync; a run with `mode.dat` refresh 85 to check the period follows csemu's. Switch:
  `BTWSTEAM_PACING=0`.
- **Risk.** Medium: it replaces the game's only pacing, so a bug changes SP speed. The
  byte checks, the switch and the speed test above cover that.

## 9. 0.1.4: novsync made opt-in (main-menu freeze on a real display)

**Report.** With 0.1.3 the picture froze at the main menu right after start, four launches in a
row. Setup: Arch Linux, Hyprland, NVIDIA RTX 5060 Laptop (10de:2d19, driver 615.78.08; the game's
GL is NVIDIA GLX through gamescope's Xwayland, `__GLX_VENDOR_LIBRARY_NAME=nvidia`), Proton
Experimental 11.0 in SteamLinuxRuntime_4 (32-bit wine, `i386-unix`), launch options
`gamescope -w 1024 -h 768 -W 1920 -H 1080 -F fsr -f -- %command%` (nested gamescope; its own
Vulkan device is the Intel iGPU), Steam overlay loaded. 0.1.0 worked. The logs end after
`LOBBY CREATED`; each run was killed 10-15 s after start (Steam's console log), so a hung thread
is not established. Then the player's two runs with `BTWSTEAM_NOVSYNC=0` (nospin still on) both
worked: 10 min, and a 17-min network game over real Steam (below).

**Harness, no reproduction.** Every run 60-80 s on the main menu / attract demo, mouse moved by
XTEST every 3 s, a screenshot every 3-4 s, thread states from `/proc`:

| Runner | gamescope (headless) | novsync / nospin | Result |
|---|---|---|---|
| system wine 11.20 (WoW64) | `-w 1024 -h 768 -W 1920 -H 1080 -F fsr -f` | on / on | every screenshot different; demo after ~30 s; `perf:` 59.8 frames/s |
| Proton Experimental 11.0 in SteamLinuxRuntime_4 (`tools/wine-proton-slr.sh`) | same | on / on | same: every screenshot different, demo plays |
| same | default 1024x768 | off / off | main menu, normal |
| Proton's wine on the host (no runtime) | same | any, also `BTWSTEAM_DISABLE=1` | not usable: stalls on a white screen (32-bit libXrandr/Xi/Xrender/Xcomposite/Xcursor missing on this host), and without `bin/Tracks` moved away csemu's CD-music thread overflows its stack (0xc00000fd, also with the hook disabled) |

What the harness cannot reproduce: a real display's present path. Headless gamescope completes
every present on its own timer; the player's nested gamescope latches frames from Hyprland's
frame callbacks, through NVIDIA GLX on Xwayland with explicit sync and PRIME to the Intel GPU,
plus the Steam overlay's GL hooks. Nested gamescope cannot be run without a window on the
user's display, so it was not tried.

**Code review (perf.c and csemu), what can and cannot freeze:**

- *nospin* is not active in the menus: the SP wrapper (0x4b1964) is inside the in-game frame
  function 0x4b1530, which the main menu does not call (no `perf:` line while on the menu; it
  starts with the attract demo). Each wrapper call does at most one
  `MsgWaitForMultipleObjectsEx` of 2 ms and always calls the real `ProcessMessages` afterwards, so
  it cannot wait forever or starve the pump. `until_us` takes the DWORD difference as LONG, so a
  deadline in the past is negative (no wait) and the 49.7-day `GetTickCount` wrap is handled;
  `qpc_us` splits quotient and remainder, so any QPC frequency works without overflow. Before the
  worker's calibration `g_timed_ok` is 0 and `g_cost_us` is 1 s, so no timed wait happens early.
- *novsync and csemu's lock* (Ghidra, `re/ghidra-csemu`): the render thread 0x10003040 holds
  csemu's critical section 0x10054e38 around GetAttachedSurface, `Lock(DDLOCK_WAIT)`, the 8->32
  conversion, `Unlock`, `Blt` and `Flip`, releases it, and waits for the auto-reset event
  0x1005525c. The game thread's present (`FlipToGDISurface` 0x10002760, called from dmcr
  0x44fac9) only does `SetEvent` + `Sleep(n)` and never takes the lock, so the render thread
  cannot block the game thread there, and an auto-reset event cannot make it loop on its own.
  The game thread takes the lock only in 0x10003250 (stop the render thread: from
  `SetDisplayMode`, `CreateSurface`, `CreatePalette` and releases): it sets a stop flag and
  polls with `Sleep(100)` until the render thread clears it at the top of its loop. **If a flip
  or the next back-buffer `Lock` never returns, the picture freezes, and the game thread hangs at
  its next mode change or surface release.** With `DDFLIP_NOVSYNC` wined3d presents with swap
  interval 0; ddraw's thread (here csemu's render thread) waits in `wined3d_cs_emit_present`
  while the CS thread is `max_frame_latency` presents behind. A display path that does not
  complete interval-0 swaps would block exactly there. That is the best-supported hypothesis; it
  could not be checked on this hardware without a window on the user's display.

**Decision.** The risk cannot be removed from our side, and the gain is CPU only (render thread
3-93 % -> 2-3 % of a core; pacing unchanged either way). novsync is off by default from 0.1.4;
`BTWSTEAM_NOVSYNC=1` turns it on (Wine/Proton only, same byte checks). nospin stays on.

**Network game, mouse-lag report.** A joiner on 0.1.3 reported clear mouse lag in a game over
real Steam (ping probably >= 50 ms, where the MP tick wait and its wrapper run). Measured:

- The player's own 17-min network game over real Steam (0.1.3, `BTWSTEAM_NOVSYNC=0`, nospin on;
  103 `perf:` windows): 32.25 game frames/s, 4 690 MP wrapper calls/s, **2.5 waits/s, 2.2 ms
  waited per second (0.2 % of the time)**, 0.5 waits/s ended by input, 1 late wait in total.
- Harness, copies a (host) and b (joiner), system wine, `BTWSTEAM_MOCK_LAG_MS=40` (room ping
  ~97 ms), `BTWSTEAM_NOVSYNC=0` in all runs, 70 s matches; presents/s from Wine's `+fps` channel
  (`wined3d_cs_exec_present`, last ~40 one-second samples), the rest from `perf:` / `timing:`:

| | nospin on | `BTWSTEAM_NOSPIN=0` | nospin on, `BTWSTEAM_GAME_THREAD_RX=0` |
|---|---|---|---|
| presents/s a / b (min-p50-max) | 58.5-59.5-60.3 / 58.2-59.5-60.1 | 58.8-60.0-60.4 / 58.5-59.5-60.2 | 58.3-59.5-60.0 / 57.9-59.3-60.0 |
| game frames per 10 s, steady (a, b) | 323, 322-323 | 323, 322-323 | 322-323, 322-323 |
| MP wrapper waits per 10 s (a / b) | 26-40 / 32-36 | - | 26-30 / 25-32 |
| time waited per 10 s (a / b) | 24-36 / 29-34 ms | - | 24-27 / 20-29 ms |
| late waits | 0 | - | 0 |
| receive Steam -> game p50 / p95 (a) | 7.5 / 15.1 ms | 7.3 / 15.3 ms | 13.0 / 23.4 ms |
| game-thread receive call p50 / p95 / max | 10 / 20 / 295 us | 10 / 10 / 271 us | - |

- The MP wrapper waits only when the loop would spin anyway (BL=0: no extra frame fits before
  the tick, so nothing is drawn until the tick), each wait ends before the tick and at once on
  input (`MWMO_INPUTAVAILABLE`), and it waits 0.2-0.4 % of the time. Present rate and game frame
  rate are the same with and without it. It cannot explain a visible mouse lag.
- The game-thread receive never waits for a lock (`TryEnter`), its calls take 10-20 us, and
  turning it off only makes receive latency worse (+5.5 ms p50).
- Not measured: the frame-interval distribution under lag (the relay trace of section 1), and
  the joiner's own setup (its novsync state, display path, real ping). If the lag persists on
  0.1.4, `BTWSTEAM_NOSPIN=0` and `BTWSTEAM_GAME_THREAD_RX=0` on the joiner are the next bisect
  steps; the game's own MP pacing at high ping (32.3 game frames/s, extra frames only when they
  fit before the tick) is the same with every switch.
