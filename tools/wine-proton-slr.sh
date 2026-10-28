#!/bin/bash
# BTW_WINE for the harness: Proton's wine inside the Steam Linux Runtime, as Steam starts it.
#
#   BTW_WINE=tools/wine-proton-slr.sh \
#   BTW_WINESERVER="<Steam>/steamapps/common/Proton - Experimental/files/bin/wineserver" \
#   tools/run_instance.sh d
#
# Proton's wine started directly on the host does not work for this game: wined3d needs
# lib/vkd3d on WINEDLLPATH, and the 32-bit winex11 needs 32-bit libXrandr/libXi/libXrender/
# libXcomposite/libXcursor (missing on this host), so the game stalls on a white screen. Inside
# the runtime it reaches the main menu. pressure-vessel drops PULSE_SERVER/PIPEWIRE_REMOTE, so the
# harness's dead values are put back inside; Audio="" and the dll overrides still apply. With no
# audio device, csemu.dll's CD-music thread (only started when bin/Tracks exists) overflows its
# stack under Proton (0xc00000fd), so move the copy's Tracks symlink away for Proton runs.
# Env: BTW_PROTON (default "Proton - Experimental"), BTW_SLR (default SteamLinuxRuntime_4).
STEAMAPPS="$HOME/.local/share/Steam/steamapps/common"
P="$STEAMAPPS/${BTW_PROTON:-Proton - Experimental}/files"
SLR="$STEAMAPPS/${BTW_SLR:-SteamLinuxRuntime_4}"
[ -x "$P/bin/wine" ] && [ -x "$SLR/run" ] || { echo "wine-proton-slr: $P/bin/wine or $SLR/run missing" >&2; exit 1; }
export WINEDLLPATH="$P/lib/vkd3d:$P/lib/wine"
export BTW_PROTON_LIBS="$P/lib/x86_64-linux-gnu:$P/lib/i386-linux-gnu" BTW_PROTON_WINE="$P/bin/wine"
export BTW_PULSE="${PULSE_SERVER:-}" BTW_PIPEWIRE="${PIPEWIRE_REMOTE:-}"
exec "$SLR/run" -- /bin/sh -c '
  export LD_LIBRARY_PATH="$BTW_PROTON_LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  [ -n "$BTW_PULSE" ] && export PULSE_SERVER="$BTW_PULSE"
  [ -n "$BTW_PIPEWIRE" ] && export PIPEWIRE_REMOTE="$BTW_PIPEWIRE"
  exec "$BTW_PROTON_WINE" "$@"' sh "$@"
