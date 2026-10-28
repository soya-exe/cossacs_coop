#!/usr/bin/env bash
# Tests for the player installers (installer/install.sh, build/btwsteam-setup.exe under wine)
# against a fake Steam tree. Never touches the real install: dmcr.exe and the original
# ichat.dll are only read, from $BTW_TEST_ROOT/a (bin/dmcr.exe, state/ichat.dll.orig) or
# else from $BTW_GAME_DIR/bin.
#
# usage: tools/test_installers.sh [WORKDIR]      (default /tmp/btwsteam-installer-test)
#
# WORKDIR is wiped, then holds:
#   fakesteam/Steam                       Steam root; steamapps/libraryfolders.vdf lists Library2
#   fakesteam/Library2/steamapps/appmanifest_4850.acf   installdir "Cossacks BTW Test"
#   fakesteam/Library2/steamapps/common/Cossacks BTW Test/bin/{dmcr.exe,ichat.dll}
#   pkg/          what the release zip holds (install.sh, ichat.dll, steam_api.dll, setup.exe)
#   prefix/       WINEPREFIX for btwsteam-setup.exe; home*/ fake $HOME folders
# Builds build/ichat.dll, build/btwsteam-setup.exe and build/mock/steam_api.dll if missing.
# Env: SKIP_WINE=1 runs only the install.sh part. Exit code: number of failed checks (max 100).
set -uo pipefail
TOOLS=$(cd "$(dirname "$0")" && pwd)
PROJ=$(dirname "$TOOLS")
. "$TOOLS/btwtest-env.sh"
W=${1:-/tmp/btwsteam-installer-test}
case $W in /|"$HOME"|"$HOME/") _btw_die "refusing WORKDIR $W" ;; esac
case $(realpath -m "$W") in "$(realpath -m "$BTW_GAME_DIR")"*) _btw_die "WORKDIR inside the real install" ;; esac

VERSION=$(sed -n 's/^#define BTWSTEAM_VERSION *"\([^"]*\)".*/\1/p' "$PROJ/src/hook/btwsteam.h")
[ -f "$PROJ/build/ichat.dll" ] || "$PROJ/tools/build.sh" >/dev/null || exit 100
[ -f "$PROJ/build/btwsteam-setup.exe" ] || "$PROJ/tools/build_installer.sh" >/dev/null || exit 100
[ -f "$PROJ/build/mock/steam_api.dll" ] || "$PROJ/tools/build_mock.sh" >/dev/null || exit 100

# ---- ingredients (read-only copies) -------------------------------------------------------
rm -rf "$W"; mkdir -p "$W/src" "$W/pkg"
if [ -f "$BTW_TEST_ROOT/a/state/ichat.dll.orig" ] && [ -f "$BTW_TEST_ROOT/a/bin/dmcr.exe" ]; then
    cp "$BTW_TEST_ROOT/a/bin/dmcr.exe" "$W/src/dmcr.exe"
    cp "$BTW_TEST_ROOT/a/state/ichat.dll.orig" "$W/src/ichat.dll"
else
    cp "$BTW_GAME_DIR/bin/dmcr.exe" "$BTW_GAME_DIR/bin/ichat.dll" "$W/src/"
fi
chmod u+w "$W/src/"*
ORIG=$W/src/ichat.dll
[ "$(sha256sum < "$W/src/dmcr.exe" | cut -d' ' -f1)" = 3923b0ff3ae8da0584f29ae2bbf2e2deb030e57df11d37fb6fb55fa5ceb4acd6 ] \
    || _btw_die "the source dmcr.exe is not v1.35"
grep -aq btwsteam "$ORIG" && _btw_die "the source ichat.dll is not the original"

cp "$PROJ/installer/install.sh" "$PROJ/build/ichat.dll" "$PROJ/third_party/steam_api.dll" \
   "$PROJ/build/btwsteam-setup.exe" "$W/pkg/"
OURS=$W/pkg/ichat.dll; STEAMDLL=$W/pkg/steam_api.dll; MOCK=$PROJ/build/mock/steam_api.dll

ROOT=$W/fakesteam/Steam; LIB=$W/fakesteam/Library2; GAME="$LIB/steamapps/common/Cossacks BTW Test"
GBIN=$GAME/bin
mkdir -p "$ROOT/steamapps" "$LIB/steamapps/common"
cat > "$ROOT/steamapps/libraryfolders.vdf" <<EOF
"libraryfolders"
{
	"0"
	{
		"path"		"$ROOT"
		"label"		""
		"apps"
		{
			"228980"		"123"
		}
	}
	"1"
	{
		"path"		"$LIB"
		"apps"
		{
			"4850"		"227000000"
		}
	}
}
EOF
cat > "$LIB/steamapps/appmanifest_4850.acf" <<EOF
"AppState"
{
	"appid"		"4850"
	"name"		"Cossacks: Back to War"
	"buildid"		"2684"
	"installdir"		"Cossacks BTW Test"
}
EOF

reset_game() { # reset_game [dir]: a fresh game folder with the original files
    local g=${1:-$GAME}
    rm -rf "$g"; mkdir -p "$g/bin"
    cp "$W/src/dmcr.exe" "$ORIG" "$g/bin/"
}

# ---- checks -------------------------------------------------------------------------------
pass=0; fail=0
ok() { echo "PASS  $*"; pass=$((pass + 1)); }
bad() { echo "FAIL  $*"; fail=$((fail + 1)); [ -n "${OUT:-}" ] && printf '%s\n' "$OUT" | sed 's/^/      | /'; }
expect() { # expect <name> <condition...>
    local n=$1; shift
    if "$@"; then ok "$n"; else bad "$n"; fi
}
has() { printf '%s' "$OUT" | grep -qF -- "$1"; }
s() { sha256sum < "$1" | cut -d' ' -f1; }

installed_ok() { # installed_ok <bin> <ichat src> <steam src>
    local b=$1
    cmp -s "$b/ichat_orig.dll" "$ORIG" && cmp -s "$b/ichat.dll" "$2" && cmp -s "$b/steam_api.dll" "$3" &&
        [ -f "$b/btwsteam-installed.txt" ] &&
        grep -q "^version=$VERSION" "$b/btwsteam-installed.txt" &&
        grep -q "^ichat.dll=$(s "$2")" "$b/btwsteam-installed.txt" &&
        grep -q "^ichat_orig.dll=$(s "$ORIG")" "$b/btwsteam-installed.txt" &&
        grep -q "^steam_api.dll=$(s "$3")" "$b/btwsteam-installed.txt" &&
        grep -q "^dmcr.exe=$(s "$b/dmcr.exe")" "$b/btwsteam-installed.txt" &&
        grep -q "^installed=20" "$b/btwsteam-installed.txt"
}
pristine() { # pristine <bin>: exactly the original dmcr.exe + ichat.dll, nothing else
    cmp -s "$1/ichat.dll" "$ORIG" && [ "$(ls -A "$1" | sort | tr '\n' ' ')" = "dmcr.exe ichat.dll " ]
}

# ================================ install.sh ================================================
echo "== install.sh"
IS() { OUT=$(HOME=$W/home BTWSTEAM_LANG=en "$@" 2>&1 </dev/null); RC=$?; }
mkdir -p "$W/home"
reset_game

IS env STEAM_ROOT="$ROOT" bash "$W/pkg/install.sh"
expect "sh: install via STEAM_ROOT + libraryfolders.vdf + appmanifest (rc 0)" [ $RC = 0 ]
expect "sh: found the game in Library2" has "Game found in the Steam library: $(realpath "$GBIN")"
expect "sh: installed files + record" installed_ok "$GBIN" "$OURS" "$STEAMDLL"
echo 4850 > "$GBIN/steam_appid.txt"   # what the DLL writes at its first start

IS env STEAM_ROOT="$ROOT" bash "$W/pkg/install.sh" --install
expect "sh: second install (rc 0)" [ $RC = 0 ]
expect "sh: second install keeps the backup" has "already present (kept)"
expect "sh: second install: backup still the original" installed_ok "$GBIN" "$OURS" "$STEAMDLL"

IS env STEAM_ROOT="$ROOT" bash "$W/pkg/install.sh" --status
expect "sh: --status (rc 0, INSTALLED)" eval '[ $RC = 0 ] && has "State: INSTALLED (btwsteam $VERSION)"'

IS env STEAM_ROOT="$ROOT" bash "$W/pkg/install.sh" --uninstall
expect "sh: --uninstall (rc 0)" [ $RC = 0 ]
expect "sh: --uninstall restores ichat.dll byte-identical, removes the rest" pristine "$GBIN"
IS env STEAM_ROOT="$ROOT" bash "$W/pkg/install.sh" --uninstall
expect "sh: second --uninstall: nothing to do (rc 0)" eval '[ $RC = 0 ] && has "nothing changed" && pristine "$GBIN"'

for rel in .local/share/Steam .steam/steam .var/app/com.valvesoftware.Steam/.local/share/Steam; do
    h=$W/home-$(echo "$rel" | tr '/.' '__'); mkdir -p "$h/$(dirname "$rel")"; ln -s "$ROOT" "$h/$rel"
    OUT=$(HOME=$h BTWSTEAM_LANG=en bash "$W/pkg/install.sh" --status 2>&1 </dev/null); RC=$?
    expect "sh: finds Steam in ~/$rel" eval '[ $RC = 0 ] && has "Game found in the Steam library: $(realpath "$GBIN")"'
done
IS bash "$W/pkg/install.sh" --status
expect "sh: no Steam, no argument: rc 3" [ $RC = 3 ]
IS bash "$W/pkg/install.sh" "$GAME" --status
expect "sh: game folder as argument" eval '[ $RC = 0 ] && has "Game folder:     $GBIN"'
IS bash "$W/pkg/install.sh" "$GBIN/dmcr.exe" --status
expect "sh: dmcr.exe as argument" eval '[ $RC = 0 ] && has "State: not installed."'
IS bash "$W/pkg/install.sh" "$W/nowhere" --status
expect "sh: wrong folder: rc 3" [ $RC = 3 ]
IS bash "$W/pkg/install.sh" --bogus
expect "sh: unknown option: rc 2" [ $RC = 2 ]

BAD=$W/badgame; reset_game "$BAD"; printf 'x' >> "$BAD/bin/dmcr.exe"
IS bash "$W/pkg/install.sh" "$BAD"
expect "sh: wrong dmcr.exe hash refused (rc 4)" eval '[ $RC = 4 ] && has "not the supported version"'
expect "sh: refused install changed nothing" eval 'cmp -s "$BAD/bin/ichat.dll" "$ORIG" && [ $(ls -A "$BAD/bin" | wc -l) = 2 ]'
IS bash "$W/pkg/install.sh" "$BAD" --force
expect "sh: --force installs anyway (rc 0)" eval '[ $RC = 0 ] && installed_ok "$BAD/bin" "$OURS" "$STEAMDLL"'
IS bash "$W/pkg/install.sh" "$BAD" --uninstall
expect "sh: uninstall after --force" eval '[ $RC = 0 ] && cmp -s "$BAD/bin/ichat.dll" "$ORIG"'

reset_game; printf 'MZ fake SteamAPI_Init steam_api from another mod' > "$GBIN/steam_api.dll"; cp "$GBIN/steam_api.dll" "$W/foreign_steam_api.dll"
IS bash "$W/pkg/install.sh" "$GAME"
expect "sh: existing steam_api.dll -> steam_api_orig.dll" eval '[ $RC = 0 ] && cmp -s "$GBIN/steam_api_orig.dll" "$W/foreign_steam_api.dll" && installed_ok "$GBIN" "$OURS" "$STEAMDLL"'
IS bash "$W/pkg/install.sh" "$GAME" --uninstall
expect "sh: uninstall puts the old steam_api.dll back" eval '[ $RC = 0 ] && cmp -s "$GBIN/steam_api.dll" "$W/foreign_steam_api.dll" && [ ! -e "$GBIN/steam_api_orig.dll" ] && cmp -s "$GBIN/ichat.dll" "$ORIG"'

reset_game
IS bash "$W/pkg/install.sh" "$GAME"
cp "$ORIG" "$GBIN/ichat.dll"     # Steam "Verify integrity of game files"
IS bash "$W/pkg/install.sh" "$GAME" --status
expect "sh: after Steam verify, --status says NOT active" has "State: NOT active"
IS bash "$W/pkg/install.sh" "$GAME"
expect "sh: reinstall after Steam verify" eval '[ $RC = 0 ] && installed_ok "$GBIN" "$OURS" "$STEAMDLL"'
IS bash "$W/pkg/install.sh" "$GAME" --uninstall
expect "sh: uninstall after reinstall" eval '[ $RC = 0 ] && pristine "$GBIN"'

reset_game
IS bash "$W/pkg/install.sh" "$GAME"
rm -f "$GBIN/ichat_orig.dll"     # backup lost while our DLL is in place
IS bash "$W/pkg/install.sh" "$GAME"
expect "sh: ours installed, backup gone: refuses (rc 1), never backs up our DLL" \
    eval '[ $RC = 1 ] && [ ! -e "$GBIN/ichat_orig.dll" ] && cmp -s "$GBIN/ichat.dll" "$OURS"'

reset_game; cp "$W/pkg/install.sh" "$W/pkg/ichat.dll" "$W/pkg/steam_api.dll" "$GAME/"
IS bash "$GAME/install.sh" --status
expect "sh: finds the game from its own folder" eval '[ $RC = 0 ] && has "Game found next to the installer: $GBIN"'
rm -f "$GAME/install.sh" "$GAME/ichat.dll" "$GAME/steam_api.dll"

mkdir -p "$W/lonely"; cp "$W/pkg/install.sh" "$W/lonely/"
IS bash "$W/lonely/install.sh" "$GAME"
expect "sh: no DLLs next to install.sh: rc 1, nothing changed" eval '[ $RC = 1 ] && pristine "$GBIN"'

mkdir -p "$W/run"; cp /usr/bin/sleep "$W/run/dmcr.exe"; "$W/run/dmcr.exe" 30 & spid=$!
sleep 0.3
IS bash "$W/pkg/install.sh" "$GAME"
expect "sh: refuses while dmcr.exe runs (rc 5)" eval '[ $RC = 5 ] && has "the game is running" && pristine "$GBIN"'
kill $spid 2>/dev/null; wait $spid 2>/dev/null

OUT=$(HOME=$W/home BTWSTEAM_LANG=ru bash "$W/pkg/install.sh" "$GAME" --status 2>&1); RC=$?
expect "sh: Russian messages" eval '[ $RC = 0 ] && has "Состояние: не установлен."'

# ================================ btwsteam-setup.exe (wine) =================================
if [ "${SKIP_WINE:-0}" != 1 ]; then
echo "== btwsteam-setup.exe (wine)"
# silent: no wine audio drivers, and no sound server reachable even if one loads
export WINEPREFIX=$W/prefix WINEDEBUG=-all
export WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d;winepulse.drv,winealsa.drv,wineoss.drv=d"
export PULSE_SERVER=unix:/nonexistent PIPEWIRE_REMOTE=/nonexistent SDL_AUDIODRIVER=dummy
unset PULSE_COOKIE
unset DISPLAY WAYLAND_DISPLAY XAUTHORITY
WINEHOME=$W/home-wine; mkdir -p "$WINEHOME"
# HOME is a fake one: under wine the installer also looks for Linux Steam in $HOME
HOME=$WINEHOME "$BTW_WINE" wineboot -i >/dev/null 2>&1
HOME=$WINEHOME "$BTW_WINESERVER" -w
WS() { OUT=$(HOME=$WINEHOME BTWSTEAM_LANG=en "$BTW_WINE" "$@" --no-pause 2>&1 </dev/null); RC=$?; OUT=${OUT//$'\r'/}; }
REG() { HOME=$WINEHOME "$BTW_WINE" reg "$@" >/dev/null 2>&1; }
winpath() { printf 'Z:%s' "$1" | tr '/' '\\'; }
SETUP=$W/pkg/btwsteam-setup.exe
mkdir -p "$W/solo"; cp "$SETUP" "$W/solo/"   # alone: only the embedded payload
reset_game

WS "$SETUP" "$GAME"
expect "exe: install with the folder as argument (rc 0)" [ $RC = 0 ]
expect "exe: prefers the DLLs next to the exe" has "Using $(winpath "$W/pkg/ichat.dll") (btwsteam $VERSION)"
expect "exe: installed files + record" installed_ok "$GBIN" "$OURS" "$STEAMDLL"
echo 4850 > "$GBIN/steam_appid.txt"
WS "$SETUP" "$GAME" --install
expect "exe: second install (rc 0, backup kept)" eval '[ $RC = 0 ] && has "already present (kept)" && installed_ok "$GBIN" "$OURS" "$STEAMDLL"'
WS "$SETUP" "$GAME" --status
expect "exe: --status (rc 0, INSTALLED)" eval '[ $RC = 0 ] && has "State: INSTALLED (btwsteam $VERSION)" && has "steam_appid.txt: 4850"'
WS "$SETUP" "$(winpath "$GAME")" --uninstall
expect "exe: --uninstall with a Z:\\ path (rc 0)" [ $RC = 0 ]
expect "exe: --uninstall restores ichat.dll byte-identical, removes the rest" pristine "$GBIN"
WS "$SETUP" "$GAME" --uninstall
expect "exe: second --uninstall: nothing to do (rc 0)" eval '[ $RC = 0 ] && has "nothing changed"'

WS "$W/solo/btwsteam-setup.exe" --status
expect "exe: no registry, not in the game folder: rc 3" eval '[ $RC = 3 ] && has "Could not find"'
REG add 'HKCU\Software\Valve\Steam' /v SteamPath /t REG_SZ /d "Z:$ROOT" /f
WS "$W/solo/btwsteam-setup.exe" --status
expect "exe: Steam via HKCU SteamPath (forward slashes) + vdf + acf" has "Game found in the Steam library: $(winpath "$GBIN")"
WS "$W/solo/btwsteam-setup.exe"
expect "exe: install with no argument, embedded payload only" eval '[ $RC = 0 ] && ! has "instead of the built-in copy" && installed_ok "$GBIN" "$OURS" "$STEAMDLL"'
WS "$W/solo/btwsteam-setup.exe" --uninstall
expect "exe: uninstall with no argument" eval '[ $RC = 0 ] && pristine "$GBIN"'
REG delete 'HKCU\Software\Valve\Steam' /f
REG add 'HKLM\SOFTWARE\WOW6432Node\Valve\Steam' /v InstallPath /t REG_SZ /d "$(winpath "$ROOT")" /f
WS "$W/solo/btwsteam-setup.exe" --status
expect "exe: Steam via HKLM WOW6432Node InstallPath" eval '[ $RC = 0 ] && has "Game found in the Steam library: $(winpath "$GBIN")"'
REG delete 'HKLM\SOFTWARE\WOW6432Node\Valve\Steam' /f

# Linux Steam through Wine's Z: drive, no registry: Wine hides $HOME from Windows programs and
# sets WINEHOMEDIR instead. ~/.steam/steam and ~/.steam/root are symlinks to the same Steam, so
# the game must be reported once.
mkdir -p "$WINEHOME/.local/share" "$WINEHOME/.steam"
ln -sfn "$ROOT" "$WINEHOME/.local/share/Steam"
ln -sfn "$ROOT" "$WINEHOME/.steam/steam"; ln -sfn "$ROOT" "$WINEHOME/.steam/root"
WS "$W/solo/btwsteam-setup.exe" --status
expect "exe: Linux Steam via WINEHOMEDIR, no registry" eval '[ $RC = 0 ] && has "Game found in the Steam library: "'
expect "exe: symlinked Steam roots give one game, not copies" eval '! has "another copy"'
rm -f "$WINEHOME/.local/share/Steam" "$WINEHOME/.steam/steam" "$WINEHOME/.steam/root"

cp "$SETUP" "$GAME/"
WS "$GAME/btwsteam-setup.exe" --status
expect "exe: finds the game from its own folder" eval '[ $RC = 0 ] && has "Game found next to the installer: $(winpath "$GBIN")"'
rm -f "$GAME/btwsteam-setup.exe"

mkdir -p "$W/pkg2"; cp "$SETUP" "$W/pkg2/"; cp "$OURS" "$W/pkg2/ichat.dll"; printf 'variant' >> "$W/pkg2/ichat.dll"
cp "$MOCK" "$W/pkg2/steam_api.dll"
WS "$W/pkg2/btwsteam-setup.exe" "$GAME"
expect "exe: DLLs next to the exe win over the embedded ones" eval '[ $RC = 0 ] && installed_ok "$GBIN" "$W/pkg2/ichat.dll" "$MOCK"'
WS "$W/solo/btwsteam-setup.exe" "$GAME"
expect "exe: update to another build keeps the backup" eval '[ $RC = 0 ] && installed_ok "$GBIN" "$OURS" "$STEAMDLL" && [ ! -e "$GBIN/steam_api_orig.dll" ]'
WS "$W/solo/btwsteam-setup.exe" "$GAME" --uninstall
expect "exe: uninstall after update" eval '[ $RC = 0 ] && pristine "$GBIN"'
WS "$W/solo/btwsteam-setup.exe" "$GAME" --steam-api "$MOCK"
expect "exe: hidden --steam-api FILE" eval '[ $RC = 0 ] && installed_ok "$GBIN" "$OURS" "$MOCK"'
WS "$W/solo/btwsteam-setup.exe" "$GAME" --uninstall
expect "exe: uninstall removes the --steam-api file" eval '[ $RC = 0 ] && pristine "$GBIN"'

reset_game "$BAD"; printf 'x' >> "$BAD/bin/dmcr.exe"
WS "$SETUP" "$BAD"
expect "exe: wrong dmcr.exe hash refused (rc 4)" eval '[ $RC = 4 ] && has "not the supported version" && [ $(ls -A "$BAD/bin" | wc -l) = 2 ] && cmp -s "$BAD/bin/ichat.dll" "$ORIG"'
WS "$SETUP" "$BAD" --force
expect "exe: --force installs anyway (rc 0)" eval '[ $RC = 0 ] && installed_ok "$BAD/bin" "$OURS" "$STEAMDLL"'

reset_game; cp "$W/foreign_steam_api.dll" "$GBIN/steam_api.dll"
WS "$SETUP" "$GAME"
expect "exe: existing steam_api.dll -> steam_api_orig.dll" eval '[ $RC = 0 ] && cmp -s "$GBIN/steam_api_orig.dll" "$W/foreign_steam_api.dll" && installed_ok "$GBIN" "$OURS" "$STEAMDLL"'
WS "$SETUP" "$GAME" --uninstall
expect "exe: uninstall puts the old steam_api.dll back" eval '[ $RC = 0 ] && cmp -s "$GBIN/steam_api.dll" "$W/foreign_steam_api.dll" && [ ! -e "$GBIN/steam_api_orig.dll" ] && cmp -s "$GBIN/ichat.dll" "$ORIG"'

reset_game
IS bash "$W/pkg/install.sh" "$GAME"
WS "$SETUP" "$GAME" --status
expect "mixed: install.sh install, setup.exe --status sees it" eval '[ $RC = 0 ] && has "State: INSTALLED" && has "by install.sh $VERSION"'
WS "$SETUP" "$GAME" --uninstall
expect "mixed: setup.exe uninstalls what install.sh installed" eval '[ $RC = 0 ] && pristine "$GBIN"'
WS "$SETUP" "$GAME"
IS bash "$W/pkg/install.sh" "$GAME" --uninstall
expect "mixed: install.sh uninstalls what setup.exe installed" eval '[ $RC = 0 ] && pristine "$GBIN"'

reset_game
WS "$SETUP" "$GAME"
rm -f "$GBIN/ichat_orig.dll"     # backup lost while our DLL is in place
WS "$SETUP" "$GAME"
expect "exe: ours installed, backup gone: refuses (rc 1), never backs up our DLL" \
    eval '[ $RC = 1 ] && [ ! -e "$GBIN/ichat_orig.dll" ] && cmp -s "$GBIN/ichat.dll" "$OURS"'
reset_game

WS "$SETUP" "$GAME" --bogus
expect "exe: unknown option: rc 2" [ $RC = 2 ]
WS "$SETUP" "$W/nowhere"
expect "exe: wrong folder: rc 3" [ $RC = 3 ]
OUT=$(HOME=$WINEHOME "$BTW_WINE" "$SETUP" "$GAME" --status --lang ru --no-pause 2>&1 </dev/null); RC=$?
expect "exe: Russian messages (--lang ru)" eval '[ $RC = 0 ] && has "Состояние: не установлен."'

# the game running: a stand-in dmcr.exe (sleeps 20 s) in its own game folder
RUNG=$W/rungame; mkdir -p "$RUNG/bin"; cp "$ORIG" "$RUNG/bin/"
printf '#include <windows.h>\nint main(void){Sleep(20000);return 0;}\n' > "$W/sleeper.c"
if i686-w64-mingw32-gcc -O2 -o "$RUNG/bin/dmcr.exe" "$W/sleeper.c" 2>/dev/null; then
    HOME=$WINEHOME "$BTW_WINE" "$RUNG/bin/dmcr.exe" </dev/null >/dev/null 2>&1 & wpid=$!
    for _ in $(seq 50); do pgrep -f 'rungame/bin/dmcr.exe' >/dev/null && break; sleep 0.1; done
    sleep 1
    WS "$SETUP" "$RUNG" --force
    expect "exe: refuses to install while dmcr.exe runs (rc 5)" eval '[ $RC = 5 ] && has "the game is running" && [ ! -e "$RUNG/bin/ichat_orig.dll" ]'
    WS "$SETUP" "$RUNG" --uninstall
    expect "exe: refuses to uninstall while dmcr.exe runs (rc 5)" [ $RC = 5 ]
    IS bash "$W/pkg/install.sh" "$RUNG" --force
    expect "sh: sees a dmcr.exe running under wine (rc 5)" [ $RC = 5 ]
    HOME=$WINEHOME "$BTW_WINESERVER" -k; wait $wpid 2>/dev/null
else
    bad "exe: running-game test (could not build the stand-in dmcr.exe)"
fi
HOME=$WINEHOME "$BTW_WINESERVER" -k 2>/dev/null
fi

echo "== $pass passed, $fail failed"
[ $fail -lt 100 ] && exit $fail || exit 100
