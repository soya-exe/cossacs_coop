#!/bin/bash
# Make isolated test copies of Cossacks: Back to War for one-PC multi-instance tests.
#
# usage: tools/mkcopies.sh [--force] [name...]        (default names: a b)
#
# Result per instance N:   $BTW_TEST_ROOT/N/bin/      private game dir (cwd of dmcr.exe)
#                          $BTW_TEST_ROOT/N/docs      symlink to the install's docs/ (read only)
#                          $BTW_TEST_ROOT/N/shots/    screenshots (run_instance.sh)
#                          $BTW_TEST_ROOT/N/run.log   wine output (run_instance.sh)
# The real install is only read. Files the game only reads and that are big (ALL.GSC 243 MB,
# camp.gs1, Tracks/ music) are symlinks into the install; everything else is a real copy,
# because dmcr.exe writes into its own bin/ dir: players.txt, Players/*.txt, *.opt, mode.dat,
# MuliExOpt.dat, *.sav, Autorecord/*.rec, MapAutosave*.m3d, RN*.m3d (random maps),
# UserMissions/, Internet/*, Screenshots/, ERR.LOG, lox*.log, dump.txt, tmp*.txt, stats.dat.
# Hashes of the symlink targets and of dmcr.exe are stored in $BTW_TEST_ROOT/install.sha256;
# tools/verify_install.sh checks them (proves nothing wrote through a link).
#
# Each copy gets its own player name (players.txt + Players/<NAME>.txt cloned from the
# install's current profile): instance a -> TESTA, b -> TESTB, ...
#
# --force   delete and recreate an existing copy (prefix-N is kept).
set -euo pipefail

GAME_DIR=${BTW_GAME_DIR:-"$HOME/.local/share/Steam/steamapps/common/Cossacks Back to War"}
ROOT=${BTW_TEST_ROOT:-"$HOME/btw-test"}
LINKED=(ALL.GSC camp.gs1 Tracks)

force=0
names=()
for arg in "$@"; do
    case $arg in
        --force) force=1 ;;
        -*) echo "unknown option $arg" >&2; exit 2 ;;
        *) names+=("$arg") ;;
    esac
done
[ ${#names[@]} -eq 0 ] && names=(a b)

[ -f "$GAME_DIR/bin/dmcr.exe" ] || { echo "game not found at $GAME_DIR" >&2; exit 1; }
mkdir -p "$ROOT"
ROOT=$(cd "$ROOT" && pwd)

case "$ROOT/" in "$GAME_DIR"/*) echo "test root inside the install, refusing" >&2; exit 1 ;; esac

# Hashes of what must never change (read-only shared files + the exe).
if [ ! -f "$ROOT/install.sha256" ]; then
    (cd "$GAME_DIR/bin" && sha256sum dmcr.exe ichat.dll csemu.dll ALL.GSC camp.gs1 players.txt \
        Players/*.txt *.opt mode.dat MuliExOpt.dat) > "$ROOT/install.sha256"
    echo "recorded install hashes in $ROOT/install.sha256"
fi

# The game's own bin/ichat.dll (GSC), the only acceptable "pristine" one.
ICHAT_GSC_SHA256=8a0d5edcb6a207d25c14c3c2af5e8fbabbc2d08f9ed8f751e7bc9b242e14986a

src_player=$(tr -d '\r\n ' < "$GAME_DIR/bin/players.txt" 2>/dev/null || true)

for n in "${names[@]}"; do
    [[ $n =~ ^[a-z0-9_-]+$ ]] || { echo "bad instance name '$n'" >&2; exit 2; }
    dst="$ROOT/$n"
    if [ -d "$dst/bin" ]; then
        if [ $force -eq 1 ]; then
            echo "[$n] removing old copy"
            rm -rf "$dst/bin" "$dst/docs" "$dst/shots"
        else
            echo "[$n] exists ($dst/bin), skipping (use --force to recreate)"
            continue
        fi
    fi
    mkdir -p "$dst/shots" "$dst/state"
    echo "[$n] copying bin/ (big read-only files symlinked)"
    excl=()
    for l in "${LINKED[@]}"; do excl+=(--exclude "/$l"); done
    rsync -a "${excl[@]}" "$GAME_DIR/bin/" "$dst/bin/"
    for l in "${LINKED[@]}"; do ln -s "$GAME_DIR/bin/$l" "$dst/bin/$l"; done
    ln -s "$GAME_DIR/docs" "$dst/docs"
    # Directories the game creates/writes; make sure they exist as real dirs.
    mkdir -p "$dst/bin/Autorecord" "$dst/bin/Screenshots" "$dst/bin/Save"
    chmod -R u+w "$dst/bin"
    # The real install may have the mod installed (ichat_orig.dll = the game's file): make the
    # copy pristine and drop the mod's files and logs.
    if [ -f "$dst/bin/ichat_orig.dll" ]; then
        mv -f "$dst/bin/ichat_orig.dll" "$dst/bin/ichat.dll"
        rm -f "$dst/bin/steam_api.dll" "$dst/bin/steam_appid.txt" "$dst/bin/btwsteam-installed.txt" \
              "$dst/bin/btwsteam.log" "$dst/bin/btwsteam.prev.log"
        [ -f "$dst/bin/steam_api_orig.dll" ] && mv -f "$dst/bin/steam_api_orig.dll" "$dst/bin/steam_api.dll"
        echo "[$n] real install has the mod: copy restored to the game's own ichat.dll"
    fi
    [ "$(sha256sum < "$dst/bin/ichat.dll" | cut -d' ' -f1)" = "$ICHAT_GSC_SHA256" ] ||
        { echo "[$n] bin/ichat.dll is not the game's original (sha256 mismatch), refusing" >&2; exit 1; }
    # Pristine copy of the original ichat.dll, used by test_install.sh --restore.
    cp -p "$dst/bin/ichat.dll" "$dst/state/ichat.dll.orig"

    # Per-instance player name so the two copies are distinguishable in the lobby.
    pname="TEST$(echo "$n" | tr a-z A-Z)"
    pname=${pname:0:15}
    if [ -n "$src_player" ] && [ -f "$dst/bin/Players/$src_player.txt" ]; then
        cp "$dst/bin/Players/$src_player.txt" "$dst/bin/Players/$pname.txt"
    elif [ -f "$dst/bin/Players/standart player info.txt" ]; then
        cp "$dst/bin/Players/standart player info.txt" "$dst/bin/Players/$pname.txt"
    fi
    printf '%s\r\n' "$pname" > "$dst/bin/players.txt"
    echo "[$n] ready: $dst/bin (player $pname)"
done
