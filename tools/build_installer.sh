#!/bin/sh
# Build build/btwsteam-setup.exe (installer/setup.c) with build/ichat.dll and
# third_party/steam_api.dll embedded as resources. Builds the hook first if needed.
# The version comes from BTWSTEAM_VERSION in src/hook/btwsteam.h.
set -eu
cd "$(dirname "$0")/.."
CC=${CC:-i686-w64-mingw32-gcc}
WINDRES=${WINDRES:-i686-w64-mingw32-windres}
OBJDUMP=${OBJDUMP:-i686-w64-mingw32-objdump}

VERSION=$(sed -n 's/^#define BTWSTEAM_VERSION *"\([^"]*\)".*/\1/p' src/hook/btwsteam.h)
[ -n "$VERSION" ] || { echo "build_installer.sh: no BTWSTEAM_VERSION in src/hook/btwsteam.h" >&2; exit 1; }
if ! grep -aq "btwsteam-version:$VERSION" build/ichat.dll 2>/dev/null; then tools/build.sh; fi
grep -aq "btwsteam-version:$VERSION" build/ichat.dll || { echo "build_installer.sh: build/ichat.dll is not $VERSION" >&2; exit 1; }
[ -f third_party/steam_api.dll ] || { echo "build_installer.sh: third_party/steam_api.dll missing" >&2; exit 1; }

mkdir -p build/installer
vcomma=$(echo "$VERSION" | awk -F'[.+-]' '{printf "%d,%d,%d,0", $1, $2, $3}')
sed -e "s|@VERSION@|$VERSION|g" -e "s|@VERSION_COMMA@|$vcomma|g" \
    -e "s|@MANIFEST@|$PWD/installer/setup.manifest|" -e "s|@ICHAT@|$PWD/build/ichat.dll|" \
    -e "s|@STEAMAPI@|$PWD/third_party/steam_api.dll|" installer/setup.rc > build/installer/setup.rc
$WINDRES -O coff -i build/installer/setup.rc -o build/installer/setup.res.o

$CC -std=c11 -mcrtdll=msvcrt-os -municode -O2 -Wall -Wextra -Werror=implicit-function-declaration \
    -D__USE_MINGW_ANSI_STDIO=1 -D_WIN32_WINNT=0x0601 -DWIN32_LEAN_AND_MEAN -DUNICODE -D_UNICODE \
    "-DBTWSTEAM_VERSION=\"$VERSION\"" -fno-asynchronous-unwind-tables \
    -o build/btwsteam-setup.exe installer/setup.c build/installer/setup.res.o \
    -mconsole -static-libgcc -Wl,--nxcompat -Wl,--dynamicbase -s -ladvapi32

bad=$($OBJDUMP -p build/btwsteam-setup.exe | sed -n 's/.*DLL Name: //p' | grep -viE '^(kernel32|advapi32|msvcrt)\.dll$' || true)
if [ -n "$bad" ]; then echo "build_installer.sh: unexpected imports: $bad" >&2; exit 1; fi
echo "built build/btwsteam-setup.exe ($(stat -c %s build/btwsteam-setup.exe) bytes), btwsteam $VERSION"
