# One-PC test harness

Runs two or more copies of Cossacks: Back to War (`dmcr.exe`) at the same time on this Linux
PC, headless: no window on the desktop, no display mode change, no sound on the speakers.
Each copy has its own game folder, Wine prefix, virtual display, log and screenshots. The real
Steam install is only read.

## Quick start

```sh
tools/mkcopies.sh                      # ~/btw-test/a and ~/btw-test/b (216 MB each, once)
tools/run_instance.sh a &              # headless, mock id 1, blocks until exit / BTW_TIMEOUT
tools/run_instance.sh b &              # second copy, mock id 2
tools/shot.sh a                        # PNG of what copy a shows -> prints the path
tools/btwinput.py a click 290 262      # click "Multiplayer" in copy a
tools/stop_instance.sh a b             # kill only those copies (their wineserver + gamescope)
tools/verify_install.sh                # the real install is unchanged
```

With our DLLs:

```sh
tools/build.sh && tools/build_mock.sh
tools/test_install.sh a b --hook --mock      # build/ichat.dll (+ ichat_orig.dll) + mock steam_api.dll + steam_appid.txt=480
tools/test_install.sh a b                    # status
tools/test_install.sh a b --restore          # back to the original files
```

The hook writes `~/btw-test/<n>/bin/btwsteam.log`.

## Headless display: gamescope `--backend headless`

| Candidate | On this PC |
|---|---|
| gamescope 3.17 (`--backend headless`) | installed, **used** |
| Xvfb / xvfb-run, Xephyr, Xvnc | not installed (package `xorg-server-xvfb`) |
| weston, cage, sway, labwc | not installed |
| xdotool, xwd, xwininfo | not installed (python-xlib's XTEST is used instead) |
| ImageMagick `import` | installed, but fails here; also XGetImage on the Xwayland root window gives BadMatch |

`run_instance.sh` starts `gamescope --backend headless -W 1024 -H 768 -w 1024 -h 768 -- ...`
with `DISPLAY`, `WAYLAND_DISPLAY` and `XAUTHORITY` removed. gamescope starts its own Xwayland
(`:1`, `:2`, ... the first free number; the user's session is `:0`) and its own Wayland socket
(`gamescope-0`, `gamescope-1`, ...). Nothing is drawn on a real output. Each copy has its own
gamescope, so displays, focus, input and screenshots are separate. Each gamescope also publishes
a PipeWire video node. This does no harm.

`<inst>/state/env` holds `GS_DISPLAY`, `GS_WAYLAND`, `GS_PID` and `INNER_PID` while a copy is
running. The other tools read it.

- Screenshot: `tools/shot.sh <n> [out.png]` runs `gamescopectl screenshot` on that copy's
  `gamescope-N` socket and gets the composited frame. That is the focused window at its own
  size: the full 1024x768 game, or just the message box when one is open. If that fails, it
  falls back to `tools/btwinput.py <n> shot out.png`, which grabs every mapped top-level X
  window with XGetImage and pastes them onto a 1024x768 canvas.
- Periodic shots: `run_instance.sh` saves `<inst>/shots/0001.png`, `0002.png`, ... every
  `BTW_SHOT_EVERY` s (default 10, 0 = off). They are cleared at the next start.
- Input: `tools/btwinput.py <n|:N> <cmd>...` sends XTEST events to that Xwayland. Commands:
  `move X Y`, `click X Y [btn]`, `dclick X Y`, `down/up BTN`, `key Escape Return ctrl+a ...`,
  `type TEXT`, `sleep S`, `windows`, `shot OUT`. Several can be chained. It refuses `:0` and
  the caller's own `$DISPLAY`. Tested: Escape, menu clicks, typing (in notepad).

## Running the game: system wine 11.20, own prefix per copy

- Runner: `/usr/bin/wine` (wine-11.20, WoW64), with prefix `~/btw-test/prefix-<n>`. The prefix
  is created on first use with `wineboot -i` and no display. The `Graphics=x11` key forces
  wine's X11 driver. `BTW_WINE=<Proton>/files/bin/wine` is possible but untested; Proton's wine
  outside the Steam Linux Runtime is not needed, because system wine runs the game fine.
- DirectDraw: no fixes needed. The game runs fullscreen 1024x768 (`mode.dat`) through its own
  `csemu.dll` shim (ddraw + winmm, which loads wine's ddraw). There is no virtual desktop, no
  depth setting and no CD check. BugTrap stays as shipped.
- `WINEDLLOVERRIDES=mscoree,mshtml=;winemenubuilder.exe=d`: no Mono/Gecko prompts, and no
  `.desktop` files or file associations written into the user's `~/.local/share`. With
  `BTW_AUDIO=off` (the default), `winepulse.drv,winealsa.drv,wineoss.drv=d` is added. The game
  runs silently without complaint. Extra overrides go in `BTW_DLLOVERRIDES`.
- `WINEDEBUG=-all` by default. For debugging use `BTW_WINEDEBUG="err+all,+ddraw"` or similar.
  Output goes to `<inst>/run.log`.
- `BTWSTEAM_MOCK_ID`: a=1, b=2, c=3, ... It can be overridden in the environment. Every other
  variable (`BTWSTEAM_MOCK_LAG_MS`, `_LOSS`, `_INVITE`, ...) passes through to the game.

### The launcher problem: `btwlaunch.exe`

Started directly, `dmcr.exe` shows **"LauncherInterfaceCheck() failed with error 2: File not
found."** and stops. csemu.dll's DirectDrawCreate wrapper calls `OpenEventA` on an event that
the Steam launcher `csbtw.exe` creates. The name is
`"CSBTW_LAUNCHER_" + chr(0x40+month) + chr(0x30+dayOfWeek) + chr(0x60 + day%26)`, from
`GetSystemTime` (UTC). We cannot run `csbtw.exe` itself: it is SteamStub-wrapped and needs a real
Steam client ("Application load error 5:0000065434").

`tools/btwlaunch.c` is built automatically into `~/btw-test/tools/btwlaunch.exe` with
`i686-w64-mingw32-gcc`. It creates that event, for now and for now+10 min in case UTC midnight
passes during start-up. Then it runs `bin\dmcr.exe <all arguments>` and returns its exit code.
So `tools/run_instance.sh a +connect_lobby 123` hands `+connect_lobby 123` to dmcr.exe. Use
`BTW_LAUNCH=direct` to run `BTW_EXE` without the launcher. `BTW_GAME_EXE` selects what
btwlaunch starts.

Note for the shipped mod: real players start through Steam -> csbtw.exe -> dmcr.exe. The
ichat.dll proxy loads in dmcr.exe either way, but the csbtw path can only be tested with real
Steam.

## Test copies (`tools/mkcopies.sh [--force] [names]`)

`~/btw-test/<n>/bin` is an rsync copy of the install's `bin/` with three exceptions. These are
symlinks into the install because the game only reads them: `ALL.GSC` (243 MB), `camp.gs1` and
`Tracks/`. `docs` is a symlink too. `verify_install.sh` checks their hashes.

dmcr.exe writes into its own folder. The strings in dmcr.exe and a run with mtime checks show:
`players.txt`, `Players/*.txt`, `Cossacks.reg` (rewritten at start), `ranks.txt`,
`Data/crc1.dat`, `*.opt`, `mode.dat`, `MuliExOpt.dat`, `*.sav` / `save%d.sav`,
`Autorecord/*.rec`, `MapAutosave*.m3d`, `RN*.m3d` (random maps), `UserMissions/`,
`Internet/*`, `Screenshots/`, `ERR.LOG`, `lox*.log`, `dump.txt`, `tmp*.txt`, `stats.dat`. All
of these are real per-copy files. Each copy gets its own player name: `players.txt` and
`Players/TEST<N>.txt` are cloned from the install's current profile (a = TESTA, b = TESTB).
`state/ichat.dll.orig` keeps the pristine ichat.dll for `--restore`.

## Smoke test results (2026-10-27)

- Original files, copy a alone, then a and b together on `:1` and `:2`: the **main menu
  appears within 10 s** (`~/btw-test/smoke-mainmenu-original.png`). After about 30 s idle on
  the main menu, the game plays an attract demo ("GSC CREDITS" over a battle). Escape returns
  to the menu. A menu driver should not idle there, or should press Escape first.
- Input works: Multiplayer (290,262) -> Deathmatch (290,288) -> "Choose a Connection"
  (`~/btw-test/smoke-choose-connection.png`). That screen lists IPX Network (300,274),
  TCP/IP LAN Connection (300,300), Direct TCP/IP (300,326), GSC-Game.Net Internet Game
  (300,352) and GameSpy Internet Game (300,378), plus the "IP address for Direct TCP/IP" field
  (400,427) and the Create (157,489), Join (302,489) and Cancel (446,489) buttons. Main menu
  buttons are at x=290: Single Player 180, Multiplayer 262, Load/Replay 345, Options 427,
  Editor 509, Exit 591.
- With `--hook --mock` installed in a, the game reaches the main menu. btwsteam.log shows
  10 WS2_32 IAT slots patched, the mock loaded, and a lobby created as SteamID ...001, slot 1
  (`~/btw-test/smoke-mainmenu-hook-mock.png`).
- About 40 % of one CPU core and 330 MB RSS per copy. `verify_install.sh`: install unchanged.

## Safety rules the scripts follow

- Never the user's display: every child has `DISPLAY` and `WAYLAND_DISPLAY` removed, and only
  our gamescope sets `DISPLAY`. `btwinput.py` refuses `:0` and the caller's `$DISPLAY`.
- Kills are scoped: `stop_instance.sh` runs `WINEPREFIX=prefix-<n> wineserver -k` and kills
  the recorded gamescope pid after checking that its comm is gamescope. It never kills by image
  name. Other wine prefixes, such as the mock's unit tests, are left alone.
- `run_instance.sh` always cleans up on exit, timeout (rc 124) or Ctrl-C.
- The install is never written: mkcopies refuses a test root inside it, and test_install
  refuses paths inside it. `verify_install.sh` compares hashes and looks for files newer than
  the record.

## Files

| File | Purpose |
|---|---|
| `tools/btwtest-env.sh` | shared settings (all `BTW_*` variables) and helpers |
| `tools/mkcopies.sh` | create `~/btw-test/<n>/` copies |
| `tools/test_install.sh` | install, remove or show hook / steam_api.dll / steam_appid.txt per copy |
| `tools/run_instance.sh` | headless run with timeout, log and periodic screenshots |
| `tools/stop_instance.sh` | clean kill of one or more copies |
| `tools/shot.sh` | one screenshot of a copy |
| `tools/btwinput.py` | mouse and keyboard input, window list, fallback screenshot |
| `tools/btwlaunch.c` | stand-in for the csbtw.exe launcher (creates csemu's event) |
| `tools/verify_install.sh` | prove the real install is unchanged |

Environment variables: `BTW_TEST_ROOT` (~/btw-test), `BTW_GAME_DIR`, `BTW_TIMEOUT` (600),
`BTW_SHOT_EVERY` (10), `BTW_RES` (1024x768), `BTW_AUDIO` (off), `BTW_WINEDEBUG` (-all),
`BTW_WINE` (/usr/bin/wine), `BTW_LAUNCH` (btwlaunch|direct), `BTW_EXE` (dmcr.exe),
`BTW_DLLOVERRIDES`, `BTWSTEAM_MOCK_ID`.
