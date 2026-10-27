#!/bin/bash
# Stop a test instance started by run_instance.sh: kill that prefix's wineserver (all wine
# processes of prefix-<name> only), then the gamescope we recorded. Never kills by image name.
# usage: tools/stop_instance.sh <name>...
set -u
TOOLS=$(cd "$(dirname "$0")" && pwd)
. "$TOOLS/btwtest-env.sh"
[ $# -ge 1 ] || { echo "usage: $0 <name>..." >&2; exit 2; }
for n in "$@"; do
    btw_inst "$n"
    if [ -f "$PFX/system.reg" ]; then
        WINEPREFIX="$PFX" "$BTW_WINESERVER" -k 2>/dev/null
    fi
    gs_pid=$(cat "$STATE/gamescope.pid" 2>/dev/null || true)
    if btw_pid_is "$gs_pid" gamescope; then
        kill "$gs_pid" 2>/dev/null
        for _ in $(seq 1 25); do btw_pid_is "$gs_pid" gamescope || break; sleep 0.2; done
        btw_pid_is "$gs_pid" gamescope && kill -9 "$gs_pid" 2>/dev/null
    fi
    rm -f "$STATE/env" "$STATE/gamescope.pid"
    echo "[$n] stopped"
done
