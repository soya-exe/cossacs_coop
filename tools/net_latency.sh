#!/bin/bash
# Two-copy latency run over the mock Steam: host + joiner, room, match, timing summary.
#
# usage: tools/net_latency.sh <label> [host joiner]      (default copies: a b)
#
# Needs the copies installed with tools/test_install.sh <host> <joiner> --hook --mock.
# Runs both copies headless and silent (only through tools/run_instance.sh, BTW_AUDIO=off), with BTWSTEAM_TIMING=1, drives
# them through the menus (Multiplayer -> Deathmatch -> GSC-Game.Net -> Create), samples the
# room's "max ping" status line, readies, starts the match, lets it run, and collects:
#   ~/btw-test/latency/<label>-<copy>-btwsteam.log, -mock.log  the logs
#   ~/btw-test/latency/<label>-ping.png    the room status line of both copies, PING_SAMPLES x
#   ~/btw-test/latency/<label>-<copy>-game.png  the match at the end (game time, players)
#   ~/btw-test/latency/<label>-cpu.txt     CPU of dmcr.exe and of its btwsteam-worker thread
# and prints tools/timing_summary.py over the match's timing lines.
#
# Env: LAG (BTWSTEAM_MOCK_LAG_MS on both, default 0), MATCH_SECS (90), PING_SAMPLES (8),
#      KILL_JOINER=1 (after the match: stop the joiner, screenshot the host at +6, +15, +40, +55 s),
#      KEEP=1 (leave both copies running at the end), EXTRA_ENV ("VAR=value ..." for both).
set -uo pipefail
TOOLS=$(cd "$(dirname "$0")" && pwd)
. "$TOOLS/btwtest-env.sh"

label=${1:?usage: $0 <label> [host joiner]}
# Tests are silent: refuse anything that could turn the game's sound on.
[ "$BTW_AUDIO" = off ] || _btw_die "BTW_AUDIO must stay off (got '$BTW_AUDIO')"
case " ${EXTRA_ENV:-} " in *BTW_AUDIO*) _btw_die "EXTRA_ENV must not set BTW_AUDIO" ;; esac
H=${2:-a}; J=${3:-b}
LAG=${LAG:-0}; MATCH_SECS=${MATCH_SECS:-90}; PING_SAMPLES=${PING_SAMPLES:-8}
OUT="$BTW_TEST_ROOT/latency"; mkdir -p "$OUT"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
btw_inst "$H"; HBIN=$BIN; HID=$MOCK_ID
btw_inst "$J"; JBIN=$BIN
LOBBY=$((109775240000000000 + HID))
say() { echo "[$(date +%T)] $*"; }
wait_log() { # <file> <regex> <seconds>
    local i
    for ((i = 0; i < $3 * 2; i++)); do grep -qE "$2" "$1" 2>/dev/null && return 0; sleep 0.5; done
    return 1
}
die() { say "FAIL: $*"; [ "${KEEP:-0}" = 1 ] || "$TOOLS/stop_instance.sh" "$H" "$J" >/dev/null; exit 1; }

"$TOOLS/stop_instance.sh" "$H" "$J" >/dev/null 2>&1
rm -f "$HBIN"/steam_mock_*.log "$JBIN"/steam_mock_*.log
common=(BTW_TIMEOUT=$((MATCH_SECS + 400)) BTW_SHOT_EVERY=0 BTWSTEAM_TIMING=1 BTWSTEAM_MOCK_LAG_MS=$LAG ${EXTRA_ENV:-})
say "$label: starting $H (host) and $J (joiner), lag ${LAG} ms"
env "${common[@]}" BTWSTEAM_NICK=HostA "$TOOLS/run_instance.sh" "$H" > "$TMP/run-$H.out" 2>&1 &
sleep 2
env "${common[@]}" BTWSTEAM_NICK=JoinB BTWSTEAM_JOIN=$LOBBY "$TOOLS/run_instance.sh" "$J" > "$TMP/run-$J.out" 2>&1 &
wait_log "$HBIN/btwsteam.log" "LOBBY CREATED" 60 || die "host lobby"
wait_log "$JBIN/btwsteam.log" "LOBBY JOINED" 60 || die "joiner lobby"
sleep 18   # main menu (it starts its attract demo only after ~30 s idle)

menu() { "$TOOLS/btwinput.py" "$1" click 290 262 sleep 2.5 click 290 288 sleep 2.5 click 300 352 sleep 1.5 click 157 489; }
menu "$H"
wait_log "$JBIN/btwsteam.log" "host_ready=1" 30 || die "host room"
menu "$J"
wait_log "$JBIN/btwsteam.log" "ChatProcess called" 60 || die "joiner did not reach the room"
say "both in the room; sampling the ping line"
sleep 12
shots=()
for ((k = 1; k <= PING_SAMPLES; k++)); do
    for c in "$H" "$J"; do "$TOOLS/shot.sh" "$c" "$TMP/rp-$c-$k.png" >/dev/null 2>&1 && shots+=("$TMP/rp-$c-$k.png"); done
    sleep 2
done
python3 - "$OUT/$label-ping.png" "${shots[@]}" <<'EOF'
import sys
from PIL import Image
crops = [Image.open(p).crop((0, 693, 520, 722)) for p in sys.argv[2:]]
out = Image.new('RGB', (1040, 58 * len(crops)))
for i, c in enumerate(crops):
    out.paste(c.resize((1040, 58)), (0, i * 58))
out.save(sys.argv[1])
EOF

"$TOOLS/btwinput.py" "$J" click 912 605      # joiner: ready
sleep 10
for try in 1 2 3; do
    "$TOOLS/btwinput.py" "$H" click 912 605  # host: start
    wait_log "$HBIN/btwsteam.log" "StartGSCGame" 15 && break
done
wait_log "$HBIN/btwsteam.log" "StartGSCGame" 1 || die "the match did not start"
wait_log "$JBIN/btwsteam.log" "StartGSCGame" 20 || die "the joiner did not start"
start_ts=$(date +%T)
sum_ts=$(date -d "@$(($(date +%s) + 15))" +%T)   # summary skips the match's loading hitch
say "match started ($start_ts); running ${MATCH_SECS}s"

cpu() { # <copy> <seconds>
    local pfx="$BTW_TEST_ROOT/prefix-$1" pid="" p tid hz=$(getconf CLK_TCK)
    for p in $(pgrep -x dmcr.exe); do
        tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "WINEPREFIX=$pfx" && pid=$p
    done
    [ -n "$pid" ] || { echo "$1: no dmcr.exe"; return; }
    tid=$(grep -lx btwsteam-worker /proc/$pid/task/*/comm 2>/dev/null | head -1 | cut -d/ -f5)
    rd() { awk '{print $14 + $15}' "$1" 2>/dev/null || echo 0; }
    local p0 t0=0 p1 t1=0
    p0=$(rd /proc/$pid/stat); [ -n "$tid" ] && t0=$(rd /proc/$pid/task/$tid/stat)
    sleep "$2"
    p1=$(rd /proc/$pid/stat); [ -n "$tid" ] && t1=$(rd /proc/$pid/task/$tid/stat)
    awk -v c="$1" -v p=$((p1 - p0)) -v t=$((t1 - t0)) -v hz="$hz" -v s="$2" 'BEGIN {
        printf "%s: dmcr.exe %.1f%% of one core, btwsteam-worker thread %.2f%% (over %d s, /proc)\n",
               c, 100 * p / hz / s, 100 * t / hz / s, s }'
}
sleep 15
cs=$((MATCH_SECS > 60 ? 45 : MATCH_SECS / 2))
cpu "$H" "$cs" > "$TMP/cpu-$H" & c1=$!
cpu "$J" "$cs" > "$TMP/cpu-$J" & c2=$!
wait $c1 $c2     # only these two: the copies themselves are background jobs too
cat "$TMP/cpu-$H" "$TMP/cpu-$J" > "$OUT/$label-cpu.txt"
sleep $((MATCH_SECS - 15 - cs > 0 ? MATCH_SECS - 15 - cs : 0))
end_ts=$(date +%T)
for c in "$H" "$J"; do "$TOOLS/shot.sh" "$c" "$OUT/$label-$c-game.png" >/dev/null 2>&1; done

if [ "${KILL_JOINER:-0}" = 1 ]; then
    say "stopping the joiner $J"
    "$TOOLS/stop_instance.sh" "$J" >/dev/null
    for t in 6 15 40 55; do
        sleep $((t - ${prev:-0})); prev=$t
        "$TOOLS/shot.sh" "$H" "$OUT/$label-$H-after-kill-${t}s.png" >/dev/null 2>&1
    done
fi

for c in "$H" "$J"; do
    btw_inst "$c"
    cp "$BIN/btwsteam.log" "$OUT/$label-$c-btwsteam.log"
    cat "$BIN"/steam_mock_*.log > "$OUT/$label-$c-mock.log" 2>/dev/null
done
[ "${KEEP:-0}" = 1 ] || "$TOOLS/stop_instance.sh" "$H" "$J" >/dev/null
say "$label done; files in $OUT/$label-*"
cat "$OUT/$label-cpu.txt"
for c in "$H" "$J"; do
    echo "--- $c (match started $start_ts; summary $sum_ts - $end_ts)"
    python3 "$TOOLS/timing_summary.py" --since "$sum_ts" --until "$end_ts" "$OUT/$label-$c-btwsteam.log" "$OUT/$label-$c-mock.log"
done
