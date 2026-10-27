#!/bin/bash
# Run one test copy of the game headless (never on the user's display).
#
# usage: tools/run_instance.sh <name> [extra dmcr.exe args]
#
#   Starts `gamescope --backend headless` (its own private Xwayland :N, no window, no DRM,
#   no mode change on the real outputs) and inside it `wine dmcr.exe` from $BTW_TEST_ROOT/<name>/bin
#   with WINEPREFIX=$BTW_TEST_ROOT/prefix-<name> (created on first use) and BTWSTEAM_MOCK_ID
#   (a=1, b=2, ...). Blocks until the game exits or $BTW_TIMEOUT seconds pass, then kills that
#   prefix's wineserver and the gamescope it started. Run it with `&` to drive the game meanwhile.
#
# Files:   <inst>/run.log           wine + game stdout/stderr
#          <inst>/state/gamescope.log
#          <inst>/state/env         GS_DISPLAY=:N, GS_WAYLAND=gamescope-N, GS_PID=... (for shot.sh,
#                                   btwinput.py, stop_instance.sh)
#          <inst>/shots/NNNN.png    every $BTW_SHOT_EVERY s (0 = off)
#
# Env: BTW_TIMEOUT (600), BTW_SHOT_EVERY (10), BTW_RES (1024x768), BTW_AUDIO (off|on),
#      BTW_WINEDEBUG (-all; e.g. "err+all,+ddraw"), BTW_WINE (/usr/bin/wine), BTW_EXE (dmcr.exe),
#      BTW_DLLOVERRIDES (extra WINEDLLOVERRIDES), BTWSTEAM_MOCK_ID (override the id),
#      BTW_LAUNCH (btwlaunch = start through tools/btwlaunch.exe, the stand-in for the Steam-DRM'd
#      csbtw.exe launcher that csemu.dll insists on; direct = run $BTW_EXE as is).
set -uo pipefail
TOOLS=$(cd "$(dirname "$0")" && pwd)
. "$TOOLS/btwtest-env.sh"

# ---- inner mode: runs inside gamescope ------------------------------------------------------
if [ "${1:-}" = "--inner" ]; then
    shift; btw_inst "$1"; shift
    gs_display=$DISPLAY
    gs_wl=${GAMESCOPE_WAYLAND_DISPLAY:-}
    btw_wine_env
    {
        echo "GS_DISPLAY=$gs_display"
        echo "GS_WAYLAND=$gs_wl"
        echo "INNER_PID=$$"
    } > "$STATE/env.tmp" && mv "$STATE/env.tmp" "$STATE/env"
    export DISPLAY=$gs_display
    unset ENABLE_GAMESCOPE_WSI   # 2002 DirectDraw game: no Vulkan WSI layer needed
    cd "$BIN" || exit 1
    echo "=== $(date '+%F %T') start $BTW_EXE $* (DISPLAY=$DISPLAY mock_id=$BTWSTEAM_MOCK_ID WINEDEBUG=$WINEDEBUG)"
    echo "=== WINEDLLOVERRIDES=$WINEDLLOVERRIDES"
    if [ "$BTW_LAUNCH" = btwlaunch ]; then
        # Stand-in for the Steam-DRM'd csbtw.exe: creates csemu.dll's launcher event, runs the exe.
        BTW_GAME_EXE="$BTW_EXE" "$BTW_WINE" "$BTW_TEST_ROOT/tools/btwlaunch.exe" "$@"
    else
        "$BTW_WINE" "$BTW_EXE" "$@"
    fi
    rc=$?
    echo "=== $(date '+%F %T') $BTW_EXE exited rc=$rc"
    "$BTW_WINESERVER" -w
    exit $rc
fi

# ---- outer mode -----------------------------------------------------------------------------
[ $# -ge 1 ] || { sed -n '2,25p' "$0"; exit 2; }
btw_inst "$1"; shift
[ -f "$BIN/$BTW_EXE" ] || _btw_die "$BIN/$BTW_EXE missing; run tools/mkcopies.sh $INST"
command -v gamescope >/dev/null || _btw_die "gamescope not installed (needed for the headless display)"
mkdir -p "$STATE" "$D/shots"

if [ -f "$STATE/env" ]; then
    (. "$STATE/env"; btw_pid_is "${GS_PID:-}" gamescope) && _btw_die "instance $INST already running (tools/stop_instance.sh $INST)"
    rm -f "$STATE/env"
fi

# Prefix: created once (marker file), without any display.
if [ ! -f "$PFX/.btwtest-init" ]; then
    echo "[$INST] initialising wine prefix $PFX"
    ( btw_wine_env; "$BTW_WINE" wineboot -i > "$STATE/prefix-init.log" 2>&1; "$BTW_WINESERVER" -w )
    # Force the X11 driver (gamescope's Xwayland), never wine's Wayland driver.
    ( btw_wine_env; "$BTW_WINE" reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d x11 /f >> "$STATE/prefix-init.log" 2>&1
      "$BTW_WINESERVER" -w )
    [ -f "$PFX/system.reg" ] || _btw_die "wineboot failed, see $STATE/prefix-init.log"
    date > "$PFX/.btwtest-init"
fi

if [ "$BTW_LAUNCH" = btwlaunch ]; then
    lexe="$BTW_TEST_ROOT/tools/btwlaunch.exe"
    if [ ! -f "$lexe" ] || [ "$TOOLS/btwlaunch.c" -nt "$lexe" ]; then
        mkdir -p "$BTW_TEST_ROOT/tools"
        i686-w64-mingw32-gcc -O2 -s -Wall -o "$lexe" "$TOOLS/btwlaunch.c" || _btw_die "cannot build btwlaunch.exe"
    fi
fi

W=${BTW_RES%x*}; H=${BTW_RES#*x}
: > "$D/run.log"
rm -f "$D"/shots/[0-9][0-9][0-9][0-9].png   # periodic shots of the previous run
# Remove the env so a stale file is never read; inner mode rewrites it.
rm -f "$STATE/env"
echo "[$INST] starting headless gamescope ${W}x${H}, mock id $MOCK_ID, timeout ${BTW_TIMEOUT}s"
env -u DISPLAY -u WAYLAND_DISPLAY -u XAUTHORITY \
    setsid gamescope --backend headless -W "$W" -H "$H" -w "$W" -h "$H" -r 60 \
        -- "$TOOLS/run_instance.sh" --inner "$INST" "$@" \
    >> "$D/run.log" 2> "$STATE/gamescope.log" &
gs_pid=$!
echo "$gs_pid" > "$STATE/gamescope.pid"

cleanup() {
    trap - EXIT INT TERM
    [ -n "${shot_pid:-}" ] && kill "$shot_pid" 2>/dev/null
    "$TOOLS/stop_instance.sh" "$INST" >/dev/null 2>&1
}
trap cleanup EXIT
trap 'echo "[$INST] interrupted"; exit 130' INT TERM

for _ in $(seq 1 100); do
    [ -f "$STATE/env" ] && break
    kill -0 "$gs_pid" 2>/dev/null || break
    sleep 0.2
done
[ -f "$STATE/env" ] || { echo "[$INST] gamescope did not start; see $STATE/gamescope.log" >&2; tail -5 "$STATE/gamescope.log" >&2; exit 1; }
echo "GS_PID=$gs_pid" >> "$STATE/env"
. "$STATE/env"
echo "[$INST] display $GS_DISPLAY ($GS_WAYLAND), gamescope pid $gs_pid, log $D/run.log"

if [ "$BTW_SHOT_EVERY" != 0 ]; then
    (
        i=0
        while kill -0 "$gs_pid" 2>/dev/null; do
            sleep "$BTW_SHOT_EVERY"
            i=$((i + 1))
            "$TOOLS/shot.sh" "$INST" "$D/shots/$(printf '%04d' $i).png" >/dev/null 2>&1
        done
    ) &
    shot_pid=$!
fi

start=$SECONDS
while kill -0 "$gs_pid" 2>/dev/null; do
    if [ $((SECONDS - start)) -ge "$BTW_TIMEOUT" ]; then
        echo "[$INST] timeout after ${BTW_TIMEOUT}s, stopping"
        exit 124
    fi
    sleep 1
done
wait "$gs_pid" 2>/dev/null
rc=$?
echo "[$INST] game/gamescope exited rc=$rc after $((SECONDS - start))s"
exit $rc
