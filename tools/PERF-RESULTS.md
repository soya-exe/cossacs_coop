# Performance: FPS, stutter and CPU use (measurement pass)

Date: 2026-10-28. Game: `bin/dmcr.exe` v1.35 with the shipped `csemu.dll` (sha256 `45a032b0...`).
Test copies c and d only (`tools/mkcopies.sh c d`), started headless through
`tools/run_instance.sh` with audio off (after commit 3ce2309 every launch was checked:
`/proc/<dmcr pid>/fd` showed no `/dev/snd`). Runner: system wine 11.20, wined3d on NVIDIA GL
(libGLX_nvidia 615.78) through gamescope's headless Xwayland (`-r 60`), i7-13620H.
No third-party code was used. No source file in `src/` was changed.

## Summary

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
  - Single player: end-of-frame wait **0x4b1964-0x4b199b**, a **busy loop** on
    `call ProcessMessages` (0x4b1964) and GetTickCount until `speed/2` ms have passed since the
    last frame, **or for as long as the game is paused** (flag 0x8c0e14). `speed` is 0x6dd7e4 =
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

## 5. Proposals (not implemented)

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
