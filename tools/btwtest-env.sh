# Shared settings for the one-PC test harness (sourced by the tools/*.sh test scripts).
# Every value can be overridden from the environment.

BTW_GAME_DIR=${BTW_GAME_DIR:-"$HOME/.local/share/Steam/steamapps/common/Cossacks Back to War"}
BTW_TEST_ROOT=${BTW_TEST_ROOT:-"$HOME/btw-test"}
BTW_WINE=${BTW_WINE:-/usr/bin/wine}              # or "<Proton dir>/files/bin/wine"
BTW_WINESERVER=${BTW_WINESERVER:-"$(dirname "$BTW_WINE")/wineserver"}
BTW_RES=${BTW_RES:-1024x768}                      # headless output size (game uses mode.dat, 1024x768)
BTW_TIMEOUT=${BTW_TIMEOUT:-600}                   # seconds before run_instance.sh kills the game
BTW_SHOT_EVERY=${BTW_SHOT_EVERY:-10}              # periodic screenshot interval, 0 = off
BTW_AUDIO=${BTW_AUDIO:-off}                       # off = no wine audio driver (nothing on the speakers)
BTW_WINEDEBUG=${BTW_WINEDEBUG:--all}
BTW_EXE=${BTW_EXE:-dmcr.exe}
BTW_LAUNCH=${BTW_LAUNCH:-btwlaunch}               # btwlaunch | direct (see run_instance.sh)

_btw_die() { echo "error: $*" >&2; exit 1; }

# btw_inst <name>: sets INST, D (instance dir), BIN, PFX (wine prefix), STATE, MOCK_ID.
btw_inst() {
    INST=$1
    [[ $INST =~ ^[a-z0-9_-]+$ ]] || _btw_die "bad instance name '$INST'"
    D="$BTW_TEST_ROOT/$INST"
    BIN="$D/bin"
    PFX="$BTW_TEST_ROOT/prefix-$INST"
    STATE="$D/state"
    if [ -n "${BTWSTEAM_MOCK_ID:-}" ]; then
        MOCK_ID=$BTWSTEAM_MOCK_ID
    elif [[ $INST =~ ^[a-z]$ ]]; then
        MOCK_ID=$(( $(printf '%d' "'$INST") - 96 ))   # a=1, b=2, c=3 ...
    else
        MOCK_ID=$(( $(cksum <<<"$INST" | cut -d' ' -f1) % 200 + 20 ))
    fi
}

# Wine environment for this instance. Never points at the user's display:
# DISPLAY / WAYLAND_DISPLAY are removed here and set only by our headless gamescope.
btw_wine_env() {
    unset WAYLAND_DISPLAY DISPLAY XAUTHORITY
    export WINEPREFIX="$PFX"
    export WINEDEBUG="$BTW_WINEDEBUG"
    # mscoree/mshtml: no Mono/Gecko install prompts; winemenubuilder: no .desktop files or
    # file associations written into the user's ~/.local/share/applications.
    local ov="mscoree,mshtml=;winemenubuilder.exe=d"
    [ "$BTW_AUDIO" = off ] && ov="$ov;winepulse.drv,winealsa.drv,wineoss.drv=d"
    [ -n "${BTW_DLLOVERRIDES:-}" ] && ov="$ov;$BTW_DLLOVERRIDES"
    export WINEDLLOVERRIDES="$ov"
    export BTWSTEAM_MOCK_ID="$MOCK_ID"
}

# Read the headless display of a running instance (written by run_instance.sh).
btw_load_display() {
    [ -f "$STATE/env" ] || _btw_die "instance $INST is not running (no $STATE/env)"
    # shellcheck disable=SC1090
    . "$STATE/env"
    [ -n "${GS_DISPLAY:-}" ] || _btw_die "no display recorded in $STATE/env"
}

btw_pid_is() { # <pid> <comm-prefix>
    [ -n "$1" ] && [ -r "/proc/$1/comm" ] && [[ $(cat "/proc/$1/comm") == "$2"* ]]
}
