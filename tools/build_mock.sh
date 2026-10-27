#!/usr/bin/env bash
# Build the mock steam_api.dll and its test exe (32-bit, mingw) into build/mock/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src/mock"
OUT="$ROOT/build/mock"
CC="${CC:-i686-w64-mingw32-gcc}"
CFLAGS=(-std=gnu11 -O2 -g0 -Wall -Wextra -D_WIN32_WINNT=0x0601 -DWIN32_LEAN_AND_MEAN -I"$SRC")
LDFLAGS=(-static-libgcc -s)
mkdir -p "$OUT"
"$CC" "${CFLAGS[@]}" -shared -o "$OUT/steam_api.dll" "$SRC/steam_mock.c" "${LDFLAGS[@]}" -lws2_32
"$CC" "${CFLAGS[@]}" -o "$OUT/test_mock.exe" "$SRC/test_mock.c" "${LDFLAGS[@]}"
# sanity: exports and imports
OBJDUMP="${OBJDUMP:-i686-w64-mingw32-objdump}"
nexp=$("$OBJDUMP" -p "$OUT/steam_api.dll" | awk '/\[Ordinal\/Name Pointer\] Table/{f=1;next} f&&/^\s*\[/{n++} END{print n+0}')
if "$OBJDUMP" -p "$OUT/steam_api.dll" | grep -qi 'DLL Name: libwinpthread\|DLL Name: libgcc'; then
    echo "build_mock: unexpected runtime DLL import" >&2; exit 1
fi
echo "built $OUT/steam_api.dll ($nexp exports) and $OUT/test_mock.exe"
