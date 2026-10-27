#!/bin/sh
# Build our ichat.dll (src/hook) into build/ichat.dll with mingw-w64 (32-bit).
# Then check the export table (the 12 IChat names, ordinals 1..12) and the imported DLLs.
set -eu
cd "$(dirname "$0")/.."
CC=${CC:-i686-w64-mingw32-gcc}
OBJDUMP=${OBJDUMP:-i686-w64-mingw32-objdump}
mkdir -p build

# -mcrtdll=msvcrt-os: link msvcrt.dll, not the UCRT (api-ms-win-crt-*) this toolchain defaults to
CFLAGS="-std=c11 -mcrtdll=msvcrt-os -O2 -Wall -Wextra -Werror=implicit-function-declaration \
  -D__USE_MINGW_ANSI_STDIO=1 -D_WIN32_WINNT=0x0501 -DWIN32_LEAN_AND_MEAN \
  -fno-strict-aliasing -fno-asynchronous-unwind-tables"
LDFLAGS="-shared -mcrtdll=msvcrt-os -static-libgcc -Wl,--enable-auto-image-base -Wl,--nxcompat -s"

$CC $CFLAGS -o build/ichat.dll src/hook/*.c src/hook/ichat.def $LDFLAGS -lws2_32 -luser32 -ladvapi32

# --- checks ---
fail=0
dump=$($OBJDUMP -p build/ichat.dll)
nexp=$(printf '%s\n' "$dump" | awk '/\[Ordinal\/Name Pointer\] Table/{f=1;next} f&&/^\s*\[/{n++} f&&/^$/{f=0} END{print n+0}')
if [ "$nexp" != 12 ]; then echo "build.sh: expected 12 exports, got $nexp" >&2; fail=1; fi
want='?ChatProcess@@YAXXZ ?GetGameID@@YAXPAD@Z ?GoHomeAnyway@@YAXXZ ?LeaveGSCRoom@@YAXXZ
?Process_GSC_ChatWindow@@YAH_NPAURoomInfo@@@Z ?ReportAliveState@@YAXHPAH@Z
?ReportGSCGame@@YAXHHPAUOnePlayerReport@@@Z ?SendPrivateMessage@@YAXPAD0@Z ?SendVictoryState@@YAXHE@Z
?SetChatWState@@YAXHH@Z ?SetGameID@@YAXPAD@Z ?StartGSCGame@@YAXPAD0HPAHPAPAD11@Z'
# "[ i] +base[ ordinal] hint name", in ordinal order
got=$(printf '%s\n' "$dump" | sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p' |
      sed -n 's/.*+base\[ *\([0-9]*\)\] *[0-9a-f]* \(.*\)$/\1 \2/p' | sort -n | awk '{print $2}' | tr '\n' ' ')
if [ "$(echo $got)" != "$(echo $want)" ]; then
  echo "build.sh: export table differs" >&2; echo " want: $(echo $want)" >&2; echo " got:  $got" >&2; fail=1
fi
if printf '%s\n' "$dump" | grep -q "Forwarder RVA"; then echo "build.sh: unexpected forwarder" >&2; fail=1; fi
bad=$(printf '%s\n' "$dump" | sed -n 's/.*DLL Name: //p' | grep -viE '^(kernel32|user32|advapi32|ws2_32|msvcrt)\.dll$' || true)
if [ -n "$bad" ]; then echo "build.sh: unexpected imports: $bad" >&2; fail=1; fi
[ $fail = 0 ] || exit 1
echo "built build/ichat.dll ($(stat -c %s build/ichat.dll) bytes): 12 IChat exports, imports $(printf '%s\n' "$dump" | sed -n 's/.*DLL Name: //p' | tr '\n' ' ')"
