#!/bin/bash
# Install / remove our DLLs in test copies (never in the real install).
#
# usage: tools/test_install.sh <name>... [options]
#   --hook [PATH]    install the ichat.dll proxy (default build/ichat.dll); the original is
#                    renamed to ichat_orig.dll (only if it is the pristine GSC file)
#   --mock           install build/mock/steam_api.dll (mock Steam, one-PC tests)
#   --steam PATH     install that steam_api.dll (e.g. third_party/steam_api.dll, the real one)
#   --appid N        write steam_appid.txt with N (default 480 when a steam_api.dll is installed)
#   --restore        back to the original files: pristine ichat.dll, no ichat_orig.dll,
#                    no steam_api.dll / steam_appid.txt / btwsteam.log
#   --status         show what is installed (default when no option is given)
# Example: tools/test_install.sh a b --hook --mock
set -euo pipefail
TOOLS=$(cd "$(dirname "$0")" && pwd)
PROJ=$(dirname "$TOOLS")
. "$TOOLS/btwtest-env.sh"

names=(); hook=""; steam=""; appid=""; restore=0; status=0
while [ $# -gt 0 ]; do
    case $1 in
        --hook) if [ $# -ge 2 ] && [[ $2 != -* ]] && [[ $2 == *.dll ]]; then hook=$2; shift; else hook="$PROJ/build/ichat.dll"; fi ;;
        --mock) steam="$PROJ/build/mock/steam_api.dll" ;;
        --steam) steam=$2; shift ;;
        --appid) appid=$2; shift ;;
        --restore) restore=1 ;;
        --status) status=1 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        -*) _btw_die "unknown option $1" ;;
        *) names+=("$1") ;;
    esac
    shift
done
[ ${#names[@]} -gt 0 ] || _btw_die "no instance name (e.g. a b)"
[ -z "$hook$steam$appid" ] && [ $restore -eq 0 ] && status=1

short() { [ -f "$1" ] && sha256sum "$1" | cut -c1-12 || echo "-"; }

for n in "${names[@]}"; do
    btw_inst "$n"
    [ -d "$BIN" ] || _btw_die "$BIN missing; run tools/mkcopies.sh $n"
    case "$BIN" in "$BTW_GAME_DIR"*) _btw_die "refusing to touch the real install" ;; esac
    if [ -f "$STATE/env" ]; then echo "[$n] warning: instance seems to be running" >&2; fi
    # The pristine GSC ichat.dll: the copy mkcopies.sh kept, not the install's current file
    # (a player who installed the mod in the real install has our DLL there).
    orig=$STATE/ichat.dll.orig; [ -f "$orig" ] || orig=$BTW_GAME_DIR/bin/ichat.dll
    orig_sum=$(sha256sum "$orig" | cut -d' ' -f1)

    if [ $restore -eq 1 ]; then
        cp -f "$STATE/ichat.dll.orig" "$BIN/ichat.dll"
        rm -f "$BIN/ichat_orig.dll" "$BIN/steam_api.dll" "$BIN/steam_appid.txt" "$BIN/btwsteam.log"
        echo "[$n] restored original files"
    fi
    if [ -n "$hook" ]; then
        [ -f "$hook" ] || _btw_die "hook DLL not found: $hook (build it first)"
        if [ ! -f "$BIN/ichat_orig.dll" ]; then
            [ "$(sha256sum "$BIN/ichat.dll" | cut -d' ' -f1)" = "$orig_sum" ] \
                || _btw_die "[$n] bin/ichat.dll is not the original and there is no ichat_orig.dll; use --restore first"
            mv "$BIN/ichat.dll" "$BIN/ichat_orig.dll"
        fi
        cp -f "$hook" "$BIN/ichat.dll"
        echo "[$n] hook installed: $hook (original -> ichat_orig.dll)"
    fi
    if [ -n "$steam" ]; then
        [ -f "$steam" ] || _btw_die "steam_api.dll not found: $steam"
        cp -f "$steam" "$BIN/steam_api.dll"
        [ -z "$appid" ] && [ ! -f "$BIN/steam_appid.txt" ] && appid=480
        echo "[$n] steam_api.dll installed: $steam"
    fi
    if [ -n "$appid" ]; then
        printf '%s\n' "$appid" > "$BIN/steam_appid.txt"
        echo "[$n] steam_appid.txt = $appid"
    fi
    if [ $status -eq 1 ]; then
        ich=$(short "$BIN/ichat.dll")
        [ "$(sha256sum "$BIN/ichat.dll" | cut -d' ' -f1)" = "$orig_sum" ] && ich="$ich (original)" || ich="$ich (ours)"
        echo "[$n] ichat.dll $ich | ichat_orig.dll $(short "$BIN/ichat_orig.dll") |" \
             "steam_api.dll $(short "$BIN/steam_api.dll") | steam_appid.txt $(cat "$BIN/steam_appid.txt" 2>/dev/null || echo -)" \
             "| mock id $MOCK_ID"
    fi
done
