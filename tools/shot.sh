#!/bin/bash
# Screenshot of a running headless instance (PNG). Only that instance's gamescope is captured,
# never the user's desktop.
# usage: tools/shot.sh <name> [out.png]     (default: <inst>/shots/manual-<time>.png; prints the path)
#
# Method 1: `gamescopectl screenshot` on the instance's own gamescope Wayland socket (what the
#           compositor shows, including popups).
# Method 2 (fallback): tools/btwinput.py shot: XGetImage of every mapped top-level X window on the
#           instance's Xwayland, composed in stacking order.
set -u
TOOLS=$(cd "$(dirname "$0")" && pwd)
. "$TOOLS/btwtest-env.sh"
[ $# -ge 1 ] || { echo "usage: $0 <name> [out.png]" >&2; exit 2; }
btw_inst "$1"
btw_load_display
out=${2:-"$D/shots/manual-$(date +%H%M%S).png"}
mkdir -p "$(dirname "$out")"
out=$(cd "$(dirname "$out")" && pwd)/$(basename "$out")
rm -f "$out"
if [ -n "${GS_WAYLAND:-}" ]; then
    GAMESCOPE_WAYLAND_DISPLAY=$GS_WAYLAND timeout 5 gamescopectl screenshot "$out" >/dev/null 2>&1
    for _ in $(seq 1 30); do
        [ -s "$out" ] && { sleep 0.2; echo "$out"; exit 0; }
        sleep 0.1
    done
fi
python3 "$TOOLS/btwinput.py" "$INST" shot "$out" && echo "$out"
