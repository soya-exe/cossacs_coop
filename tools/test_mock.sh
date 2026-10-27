#!/usr/bin/env bash
# Two-process test of the mock steam_api.dll under wine, no game needed.
#   tools/test_mock.sh
# Env: WINEPREFIX (default: $TEST_DIR/wineprefix, a throwaway prefix)
#      TEST_DIR   (default: build/test_mock), WINE (default: wine)
#      TEST_HOST_ID / TEST_JOIN_ID: mock ids (default 901 / 902, so the test does not collide
#      with game test instances using ids 1, 2, ...; ports are 47000 + id)
# Scenarios:
#   basic     : host creates a lobby, joiner joins by id (from a file), 100 messages each way
#   lagloss   : same with BTWSTEAM_MOCK_LAG_MS=100 and BTWSTEAM_MOCK_LOSS=0.3, joiner joins
#               through the BTWSTEAM_MOCK_INVITE callback (333); every reliable message must arrive
#   mutation  : the test reads the message sender at offset 16 instead of 12; this MUST fail,
#               which proves the test can catch a wrong SteamNetworkingMessage_t layout
# Prints PASS or FAIL as the last line; exit code 0 / 1.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEST_DIR="${TEST_DIR:-$ROOT/build/test_mock}"
export WINEPREFIX="${WINEPREFIX:-$TEST_DIR/wineprefix}"
export WINEDEBUG=-all
export WINEDLLOVERRIDES="mscoree,mshtml="
WINE="${WINE:-wine}"
HID="${TEST_HOST_ID:-901}"
JID="${TEST_JOIN_ID:-902}"

"$ROOT/tools/build_mock.sh" || { echo FAIL; exit 1; }
mkdir -p "$TEST_DIR"
if [ ! -f "$WINEPREFIX/system.reg" ]; then
    echo "creating wine prefix $WINEPREFIX"
    "$WINE" wineboot -i >/dev/null 2>&1
    wineserver -w 2>/dev/null || true
fi

# run_pair <name> <host env...> -- <join env...>; returns 0 if both instances printed PASS
run_pair() {
    local name="$1"; shift
    local henv=() jenv=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do henv+=("$1"); shift; done
    shift
    jenv=("$@")
    local dir="$TEST_DIR/$name"
    rm -rf "$dir"; mkdir -p "$dir"
    if command -v ss >/dev/null && ss -uan | grep -qE "127\.0\.0\.1:($((47000 + HID))|$((47000 + JID)))\b"; then
        echo "--- $name: UDP port $((47000 + HID)) or $((47000 + JID)) already in use (another mock with the same id?)"; return 1
    fi
    cp "$ROOT/build/mock/steam_api.dll" "$ROOT/build/mock/test_mock.exe" "$dir/"
    ( cd "$dir" && env BTWSTEAM_MOCK_ID=$HID TEST_PEER_ID=$JID "${henv[@]}" timeout 120 "$WINE" ./test_mock.exe host lobby.txt >host.out 2>host.err ) &
    local hp=$!
    ( cd "$dir" && env BTWSTEAM_MOCK_ID=$JID TEST_PEER_ID=$HID "${jenv[@]}" timeout 120 "$WINE" ./test_mock.exe join lobby.txt >join.out 2>join.err ) &
    local jp=$!
    wait $hp; local hrc=$?
    wait $jp; local jrc=$?
    echo "--- $name: host (exit $hrc)"; tr -d '\r' < "$dir/host.out" | sed 's/^/    /'
    echo "--- $name: join (exit $jrc)"; tr -d '\r' < "$dir/join.out" | sed 's/^/    /'
    grep -q 'RESULT: PASS' "$dir/host.out" && grep -q 'RESULT: PASS' "$dir/join.out" && [ $hrc -eq 0 ] && [ $jrc -eq 0 ]
}

ok=1
if run_pair basic -- ; then echo "scenario basic: PASS"; else echo "scenario basic: FAIL"; ok=0; fi

if run_pair lagloss BTWSTEAM_MOCK_LAG_MS=100 BTWSTEAM_MOCK_LOSS=0.3 TEST_EXPECT_LOSS=1 -- \
        BTWSTEAM_MOCK_LAG_MS=100 BTWSTEAM_MOCK_LOSS=0.3 TEST_EXPECT_LOSS=1 TEST_JOIN_VIA_INVITE=1 TEST_MIN_JOIN_MS=200 \
        BTWSTEAM_MOCK_INVITE=$((109775240000000000 + HID)); then
    echo "scenario lagloss: PASS"
else echo "scenario lagloss: FAIL"; ok=0; fi

if run_pair mutation TEST_SENDER_OFFSET=16 TEST_TIMEOUT_S=15 -- TEST_SENDER_OFFSET=16 TEST_TIMEOUT_S=15; then
    echo "scenario mutation (sender read at offset 16): unexpectedly PASSED, the test cannot catch it"; ok=0
else
    if grep -q 'undecodable sender' "$TEST_DIR/mutation/host.out" "$TEST_DIR/mutation/join.out"; then
        echo "scenario mutation (sender read at offset 16): failed as expected"
    else
        echo "scenario mutation: failed, but not because of the sender offset"; ok=0
    fi
fi

echo "logs: $TEST_DIR/{basic,lagloss,mutation}/"
if [ $ok -eq 1 ]; then echo PASS; exit 0; else echo FAIL; exit 1; fi
