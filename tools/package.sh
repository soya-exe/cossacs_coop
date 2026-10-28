#!/bin/sh
# Build the player release: dist/cossacks-steam-coop-<version>/ and
# dist/cossacks-steam-coop-<version>.zip, with btwsteam-setup.exe (Windows installer),
# ichat.dll, steam_api.dll, install.sh (Linux installer), PLAYERS.md and notes.
# The version comes from BTWSTEAM_VERSION in src/hook/btwsteam.h.
set -eu
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^#define BTWSTEAM_VERSION *"\([^"]*\)".*/\1/p' src/hook/btwsteam.h)
[ -n "$VERSION" ] || { echo "package.sh: no BTWSTEAM_VERSION in src/hook/btwsteam.h" >&2; exit 1; }
sh_ver=$(sed -n 's/^VERSION=//p' installer/install.sh)
[ "$sh_ver" = "$VERSION" ] || { echo "package.sh: installer/install.sh says VERSION=$sh_ver, btwsteam.h says $VERSION" >&2; exit 1; }

tools/build.sh
tools/build_installer.sh

NAME=cossacks-steam-coop-$VERSION
D=dist/$NAME
rm -rf "$D" "$D.zip"
mkdir -p "$D"
cp build/btwsteam-setup.exe build/ichat.dll third_party/steam_api.dll installer/install.sh docs/PLAYERS.md "$D/"
chmod 755 "$D/install.sh"
chmod 644 "$D/ichat.dll" "$D/steam_api.dll" "$D/PLAYERS.md"

cat > "$D/README.txt" <<EOF
Cossacks: Back to War - multiplayer over Steam (btwsteam $VERSION)
Сетевая игра через Steam для Cossacks: Back to War (btwsteam $VERSION)

Windows: double-click btwsteam-setup.exe / дважды щёлкните btwsteam-setup.exe
Linux (Steam + Proton): bash install.sh

How to play, uninstall, known limitations / Как играть, удаление, ограничения: PLAYERS.md
Source / исходный код: https://github.com/soya-exe/cossacs_coop

Files:
  btwsteam-setup.exe  Windows installer (ichat.dll and steam_api.dll are also built into it)
  install.sh          Linux installer (uses ichat.dll and steam_api.dll from this folder)
  ichat.dll           the mod; replaces the game's bin/ichat.dll (kept as ichat_orig.dll)
  steam_api.dll       Valve's Steamworks API library, see below
  SHA256SUMS.txt      checksums

steam_api.dll is Valve's redistributable Steamworks API library (32-bit), obtained via
Steamworks.NET (Standalone 2025.164.1, https://github.com/rlabrecque/Steamworks.NET).
It is Valve's file, not part of this mod's source code.
steam_api.dll - распространяемая библиотека Steamworks API от Valve (32-бит), взята из
Steamworks.NET. Это файл Valve, а не часть мода.
EOF

(cd "$D" && sha256sum btwsteam-setup.exe ichat.dll steam_api.dll install.sh PLAYERS.md README.txt > SHA256SUMS.txt)

if command -v zip >/dev/null; then
    (cd dist && zip -qr -X "$NAME.zip" "$NAME")
elif command -v bsdtar >/dev/null; then
    (cd dist && bsdtar -a -cf "$NAME.zip" "$NAME")
else
    python3 - "$NAME" <<'EOF'
import os, sys, zipfile
name = sys.argv[1]
os.chdir("dist")
with zipfile.ZipFile(name + ".zip", "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(name):
        for f in sorted(files):
            p = os.path.join(root, f)
            z.write(p, p)
EOF
fi
echo "packaged $D/ and $D.zip ($(stat -c %s "$D.zip") bytes):"
ls -l "$D"
