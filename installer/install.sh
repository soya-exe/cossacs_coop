#!/usr/bin/env bash
# btwsteam installer for Linux (Steam + Proton), no Wine needed: Steam multiplayer for
# Cossacks: Back to War (Steam app 4850, dmcr.exe v1.35). Same logic and the same record file
# (bin/btwsteam-installed.txt) as btwsteam-setup.exe, so either one can undo the other.
#
# usage: ./install.sh [GAME_DIR] [--install | --uninstall | --status] [--force]
#
#   GAME_DIR     the game folder, its bin/ or bin/dmcr.exe. Default: search the Steam
#                libraries ($STEAM_ROOT if set, else ~/.local/share/Steam, ~/.steam/steam,
#                ~/.steam/root, Flatpak and Snap Steam): steamapps/libraryfolders.vdf ->
#                steamapps/appmanifest_4850.acf "installdir" -> steamapps/common/<dir>/bin/dmcr.exe
#   --install    install or update (default): bin/ichat.dll -> bin/ichat_orig.dll (once), then
#                our ichat.dll and steam_api.dll from this script's folder into bin/
#   --uninstall  restore the original files (logs are kept)
#   --status     show what is installed
#   --force      install even if dmcr.exe is not the supported v1.35
#
# Env: STEAM_ROOT=<dir> searches only that Steam folder. BTWSTEAM_ICHAT / BTWSTEAM_STEAM_API
# use other DLL files (tests). BTWSTEAM_LANG=ru|en (default from $LANG).
# Exit codes: 0 ok, 1 failed, 2 bad arguments, 3 game not found, 4 wrong dmcr.exe, 5 game running.
set -euo pipefail

VERSION=0.1.2
APPID=4850
DMCR_SHA256=3923b0ff3ae8da0584f29ae2bbf2e2deb030e57df11d37fb6fb55fa5ceb4acd6
ICHAT_GSC_SHA256=8a0d5edcb6a207d25c14c3c2af5e8fbabbc2d08f9ed8f751e7bc9b242e14986a
RECORD=btwsteam-installed.txt
HERE=$(cd "$(dirname "$0")" && pwd)
# Payload: next to this script (release zip); in a source checkout (installer/install.sh)
# fall back to the built ../build/ichat.dll and ../third_party/steam_api.dll.
SRC_ICHAT=$HERE/ichat.dll SRC_STEAM=$HERE/steam_api.dll
if [ ! -f "$SRC_ICHAT" ] && [ -f "$HERE/../build/ichat.dll" ] && [ -f "$HERE/../third_party/steam_api.dll" ]; then
    SRC_ICHAT=$(cd "$HERE/.." && pwd)/build/ichat.dll SRC_STEAM=$(cd "$HERE/.." && pwd)/third_party/steam_api.dll
fi

lang=${BTWSTEAM_LANG:-${LC_ALL:-${LC_MESSAGES:-${LANG:-en}}}}
case $lang in ru*) RU=1 ;; *) RU=0 ;; esac
# say "english" "русский" [printf args...]
say() { local f; if [ $RU = 1 ]; then f=$2; else f=$1; fi; shift 2; printf -- "$f\n" "$@"; }

sha() {
    if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}
# btwsteam version inside a DLL, empty if it is not ours (see src/hook/log.c)
btw_ver() {
    [ -f "$1" ] || return 0
    local v
    v=$(grep -aoE 'btwsteam-version:[0-9A-Za-z.+-]+' "$1" 2>/dev/null | head -n1 | cut -d: -f2) || true
    if [ -z "$v" ]; then
        v=$(grep -aoE 'btwsteam [0-9A-Za-z.+-]+ \(ichat\.dll proxy\) started' "$1" 2>/dev/null | head -n1 | cut -d' ' -f2) || true
    fi
    printf '%s' "$v"
}
fsha() { [ -f "$1" ] && sha "$1" || true; }

# ---- arguments ---------------------------------------------------------------------------------
cmd=install; force=0; game_arg=""
while [ $# -gt 0 ]; do
    case $1 in
        --install) cmd=install ;;
        --uninstall|--remove) cmd=uninstall ;;
        --status) cmd=status ;;
        --force) force=1 ;;
        -h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) say "Unknown option: %s" "Неизвестный параметр: %s" "$1"; exit 2 ;;
        *) [ -z "$game_arg" ] || { say "Only one game folder, please." "Укажите только одну папку игры."; exit 2; }
           game_arg=$1 ;;
    esac
    shift
done

say "btwsteam install.sh %s: Cossacks: Back to War multiplayer over Steam\n" "btwsteam install.sh %s: сетевая игра Cossacks: Back to War через Steam\n" "$VERSION"

# ---- finding the game --------------------------------------------------------------------------
# game_bin_from <dir|file>: prints the bin dir if dmcr.exe is there
game_bin_from() {
    local d=$1
    [ -f "$d" ] && d=$(dirname "$d")
    [ -d "$d" ] || return 1
    d=$(cd "$d" && pwd)
    if [ -f "$d/dmcr.exe" ]; then printf '%s\n' "$d"; return 0; fi
    if [ -f "$d/bin/dmcr.exe" ]; then printf '%s\n' "$d/bin"; return 0; fi
    return 1
}

# library paths of one Steam root: itself + "path" entries of libraryfolders.vdf
steam_libraries() {
    local root=$1 vdf
    printf '%s\n' "$root"
    for vdf in "$root/steamapps/libraryfolders.vdf" "$root/config/libraryfolders.vdf"; do
        [ -f "$vdf" ] || continue
        # new format: "path" "<dir>"; old: "1" "<dir>". Unescape \\ -> \.
        sed -n -e 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' \
               -e 's/^[[:space:]]*"[0-9][0-9]*"[[:space:]]*"\(\/.*\)"[[:space:]]*$/\1/p' "$vdf" |
            sed 's/\\\\/\\/g'
    done
}

find_steam_games() {
    local roots=() r lib acf dir bin seen=""
    if [ -n "${STEAM_ROOT:-}" ]; then
        roots=("$STEAM_ROOT")
    else
        roots=("$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root"
               "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
               "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"
               "$HOME/snap/steam/common/.local/share/Steam")
    fi
    for r in "${roots[@]}"; do
        [ -d "$r" ] || continue
        while IFS= read -r lib; do
            [ -d "$lib" ] || continue
            acf="$lib/steamapps/appmanifest_$APPID.acf"
            dir="Cossacks Back to War"
            if [ -f "$acf" ]; then
                local d
                d=$(sed -n 's/^[[:space:]]*"installdir"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$acf" | head -n1)
                [ -n "$d" ] && dir=$d
            fi
            bin=$(game_bin_from "$lib/steamapps/common/$dir" 2>/dev/null) || continue
            bin=$(realpath "$bin" 2>/dev/null || printf '%s' "$bin")
            case $'\n'"$seen" in *$'\n'"$bin"$'\n'*) continue ;; esac
            seen+="$bin"$'\n'
            printf '%s\n' "$bin"
        done < <(steam_libraries "$r")
    done
}

if [ -n "$game_arg" ]; then
    BIN=$(game_bin_from "$game_arg") || { say "Error: no bin/dmcr.exe in \"%s\"." "Ошибка: в \"%s\" нет bin/dmcr.exe." "$game_arg"; exit 3; }
elif BIN=$(game_bin_from "$HERE" 2>/dev/null); then
    say "Game found next to the installer: %s" "Игра найдена рядом с установщиком: %s" "$BIN"
else
    mapfile -t games < <(find_steam_games)
    if [ ${#games[@]} -eq 0 ]; then
        say "Could not find Cossacks: Back to War (Steam app %s). Give the game folder as an argument:\n  %s \"/path/to/Cossacks Back to War\"" \
            "Не удалось найти Cossacks: Back to War (Steam app %s). Укажите папку игры аргументом:\n  %s \"/путь/к/Cossacks Back to War\"" "$APPID" "$0"
        exit 3
    fi
    BIN=${games[0]}
    say "Game found in the Steam library: %s" "Игра найдена в библиотеке Steam: %s" "$BIN"
    for g in "${games[@]:1}"; do
        say "  (another copy: %s; give its folder as an argument to use it)" "  (ещё одна копия: %s; чтобы выбрать её, укажите папку аргументом)" "$g"
    done
fi

ICHAT="$BIN/ichat.dll"; ICHAT_ORIG="$BIN/ichat_orig.dll"
STEAM="$BIN/steam_api.dll"; STEAM_ORIG="$BIN/steam_api_orig.dll"
APPID_TXT="$BIN/steam_appid.txt"; REC="$BIN/$RECORD"; LOG="$BIN/btwsteam.log"

rec() { [ -f "$REC" ] && sed -n "s/^$1=//p" "$REC" | tr -d '\r' | head -n1 || true; }
appid_is_ours() { [ -f "$APPID_TXT" ] && [ "$(tr -d ' \t\r\n' < "$APPID_TXT")" = "$APPID" ]; }

# any process whose name or argv[0] is dmcr.exe (Wine/Proton show the Windows path there)
game_running() {
    local p a0 comm
    for p in /proc/[0-9]*; do
        [ "${p#/proc/}" = $$ ] && continue
        comm=$(cat "$p/comm" 2>/dev/null) || continue
        a0=""
        IFS= read -r -d '' a0 < "$p/cmdline" 2>/dev/null || true
        a0=${a0##*/}; a0=${a0##*\\}
        if [ "${comm,,}" = dmcr.exe ] || [ "${a0,,}" = dmcr.exe ]; then RUNNING_PID=${p#/proc/}; return 0; fi
    done
    return 1
}
refuse_if_running() {
    if game_running; then
        say "Error: the game is running (dmcr.exe, pid %s). Quit it, then run the installer again." \
            "Ошибка: игра запущена (dmcr.exe, pid %s). Закройте её и запустите установщик снова." "$RUNNING_PID"
        exit 5
    fi
}

ichat_desc() {
    local f=$1 v
    if [ ! -f "$f" ]; then say "absent" "нет"; return; fi
    v=$(btw_ver "$f"); local s; s=$(sha "$f")
    if [ -n "$v" ]; then echo "btwsteam $v (${s:0:12})"
    elif [ "$s" = "$ICHAT_GSC_SHA256" ]; then say "original GSC (%s)" "оригинал GSC (%s)" "${s:0:12}"
    else say "not btwsteam (%s)" "не btwsteam (%s)" "${s:0:12}"; fi
}

# ---- status ------------------------------------------------------------------------------------
cmd_status() {
    local ds; ds=$(fsha "$BIN/dmcr.exe")
    say "Game folder:     %s" "Папка игры:      %s" "$BIN"
    if [ "$ds" = "$DMCR_SHA256" ]; then say "dmcr.exe:        v1.35 (OK)" "dmcr.exe:        v1.35 (OK)"
    else say "dmcr.exe:        NOT the supported version" "dmcr.exe:        НЕ та версия"; fi
    say "ichat.dll:       %s" "ichat.dll:       %s" "$(ichat_desc "$ICHAT")"
    say "ichat_orig.dll:  %s" "ichat_orig.dll:  %s" "$(ichat_desc "$ICHAT_ORIG")"
    if [ -f "$STEAM" ]; then
        local s; s=$(sha "$STEAM")
        if [ "$s" = "$(rec steam_api.dll)" ]; then say "steam_api.dll:   %s (installed by btwsteam)" "steam_api.dll:   %s (установлен btwsteam)" "${s:0:12}"
        else echo "steam_api.dll:   ${s:0:12}"; fi
    else say "steam_api.dll:   absent" "steam_api.dll:   нет"; fi
    [ -f "$STEAM_ORIG" ] && echo "steam_api_orig.dll: $(sha "$STEAM_ORIG" | cut -c1-12)"
    if [ -f "$APPID_TXT" ]; then appid_is_ours && echo "steam_appid.txt: $APPID" || echo "steam_appid.txt: ?"
    else say "steam_appid.txt: absent" "steam_appid.txt: нет"; fi
    if [ -f "$REC" ]; then
        say "Record:          %s, version %s, installed %s by %s" "Запись:          %s, версия %s, установлено %s (%s)" \
            "$RECORD" "$(rec version)" "$(rec installed)" "$(rec installer)"
    else say "Record:          none" "Запись:          нет"; fi
    if [ -f "$LOG" ]; then
        say "Log:             %s\n                 %s" "Журнал:          %s\n                 %s" "$LOG" "$(head -n1 "$LOG" | tr -d '\r')"
    fi
    game_running && say "The game is running now." "Игра сейчас запущена."
    local vi vo; vi=$(btw_ver "$ICHAT"); vo=$(btw_ver "$ICHAT_ORIG")
    if [ -n "$vi" ] && [ -f "$ICHAT_ORIG" ] && [ -z "$vo" ]; then
        say "State: INSTALLED (btwsteam %s)." "Состояние: УСТАНОВЛЕН (btwsteam %s)." "$vi"
    elif [ -n "$vi" ]; then
        say "State: btwsteam %s is in place, but there is no backup ichat_orig.dll." \
            "Состояние: btwsteam %s на месте, но копии ichat_orig.dll нет." "$vi"
    elif [ -f "$ICHAT_ORIG" ] || [ -f "$REC" ]; then
        say "State: NOT active: ichat.dll is the game's own (Steam \"Verify integrity\" restores it). Run the installer again." \
            "Состояние: НЕ активен: ichat.dll снова оригинальный (его возвращает проверка целостности в Steam). Запустите установку ещё раз."
    else say "State: not installed." "Состояние: не установлен."; fi
}

# ---- install -----------------------------------------------------------------------------------
put() { # put <src> <dst>: copy via a temp file, then check the hash
    cp -f "$1" "$2.btwtmp" && mv -f "$2.btwtmp" "$2" || { rm -f "$2.btwtmp"; say "Error: cannot write %s" "Ошибка: не удалось записать %s" "$2"; return 1; }
    [ "$(sha "$1")" = "$(sha "$2")" ] || { say "Error: %s does not match what was written." "Ошибка: %s не совпадает с записанным." "$2"; return 1; }
}

cmd_install() {
    local ds; ds=$(fsha "$BIN/dmcr.exe")
    if [ "$ds" = "$DMCR_SHA256" ]; then
        say "dmcr.exe: v1.35 (Steam build 2684), supported." "dmcr.exe: v1.35 (сборка Steam 2684), поддерживается."
    else
        say "%s: dmcr.exe is not the supported version.\n  SHA-256 %s\n  expected %s (Steam v1.35, build 2684)\n  The mod patches code at fixed addresses and works only with that exact file.\n  Update the game in Steam (or \"Verify integrity of game files\") and try again." \
            "%s: dmcr.exe не той версии.\n  SHA-256 %s\n  нужен   %s (Steam v1.35, сборка 2684)\n  Мод правит код по фиксированным адресам и работает только с этим файлом.\n  Обновите игру в Steam (или «Проверить целостность файлов игры») и повторите." \
            "$([ $force = 1 ] && say Warning Предупреждение || say Error Ошибка)" "${ds:--}" "$DMCR_SHA256"
        if [ $force = 1 ]; then say "  --force given: installing anyway." "  Указан --force: устанавливаем всё равно."
        else say "  (--force installs anyway, at your own risk.)" "  (--force установит всё равно, на ваш риск.)"; exit 4; fi
    fi
    refuse_if_running

    local src_ichat=${BTWSTEAM_ICHAT:-$SRC_ICHAT} src_steam=${BTWSTEAM_STEAM_API:-$SRC_STEAM}
    local pv; pv=$(btw_ver "$src_ichat")
    if [ ! -f "$src_ichat" ] || [ -z "$pv" ]; then
        say "Error: %s is missing or is not a btwsteam build. Run install.sh from the unpacked release zip (or, in a source checkout, build first: tools/build.sh)." \
            "Ошибка: %s отсутствует или не из btwsteam. Запускайте install.sh из распакованного архива релиза (или в исходниках сначала соберите: tools/build.sh)." "$src_ichat"; exit 1
    fi
    if [ ! -f "$src_steam" ] || ! grep -aq 'SteamAPI_' "$src_steam"; then
        say "Error: %s is missing or is not a steam_api.dll." "Ошибка: %s отсутствует или не является steam_api.dll." "$src_steam"; exit 1
    fi
    if [ "$(cd "$(dirname "$src_ichat")" && pwd)" = "$BIN" ]; then
        say "Error: run install.sh from the unpacked release folder, not from the game's bin/." \
            "Ошибка: запускайте install.sh из распакованной папки релиза, а не из bin/ игры."; exit 1
    fi
    local steam_sha; steam_sha=$(sha "$src_steam")

    local vi vo appid_pre moved=0
    vi=$(btw_ver "$ICHAT"); vo=$(btw_ver "$ICHAT_ORIG")
    appid_pre=$(rec steam_appid_preexisting)
    if [ -z "$appid_pre" ]; then
        if [ -f "$APPID_TXT" ] && [ -z "$vi" ] && [ ! -f "$ICHAT_ORIG" ]; then appid_pre=1; else appid_pre=0; fi
    fi

    # 1. the game's ichat.dll -> ichat_orig.dll
    if [ -f "$ICHAT_ORIG" ] && [ -n "$vo" ]; then
        if [ ! -f "$ICHAT" ] || [ -n "$vi" ]; then
            say "Error: ichat_orig.dll is a btwsteam build, not the game's file, and ichat.dll is not the\n  original either. Use Steam's \"Verify integrity of game files\", then run the installer again." \
                "Ошибка: ichat_orig.dll — сборка btwsteam, а не файл игры, и ichat.dll тоже не оригинал.\n  Выполните в Steam «Проверить целостность файлов игры» и запустите установку снова."; exit 1
        fi
        mv -f "$ICHAT" "$ICHAT_ORIG"; moved=1
        say "ichat_orig.dll held a btwsteam build; replaced with the game's ichat.dll." "В ichat_orig.dll была сборка btwsteam; заменена оригинальным ichat.dll."
    elif [ ! -f "$ICHAT_ORIG" ]; then
        if [ ! -f "$ICHAT" ]; then
            say "Error: bin/ichat.dll is missing. Use Steam's \"Verify integrity of game files\" first." \
                "Ошибка: нет bin/ichat.dll. Сначала выполните в Steam «Проверить целостность файлов игры»."; exit 1
        fi
        if [ -n "$vi" ]; then
            say "Error: ichat.dll is already btwsteam %s, but the game's original (ichat_orig.dll) is gone.\n  Use Steam's \"Verify integrity of game files\" (it restores ichat.dll), then run the installer again." \
                "Ошибка: ichat.dll уже btwsteam %s, но оригинала (ichat_orig.dll) нет.\n  Выполните в Steam «Проверить целостность файлов игры» (вернёт ichat.dll) и запустите установку снова." "$vi"; exit 1
        fi
        mv "$ICHAT" "$ICHAT_ORIG"; moved=1
        say "Original ichat.dll kept as ichat_orig.dll." "Оригинальный ichat.dll сохранён как ichat_orig.dll."
    elif [ -f "$ICHAT" ] && [ -z "$vi" ] && [ "$(sha "$ICHAT")" != "$(sha "$ICHAT_ORIG")" ]; then
        mv -f "$ICHAT" "$ICHAT_ORIG"; moved=1
        say "ichat.dll was replaced by Steam since the last install; the new original is kept as ichat_orig.dll." \
            "ichat.dll был заменён Steam после прошлой установки; новый оригинал сохранён как ichat_orig.dll."
    else
        say "Backup ichat_orig.dll already present (kept)." "Копия ichat_orig.dll уже есть (оставлена)."
    fi

    # 2. our ichat.dll
    if ! put "$src_ichat" "$ICHAT"; then
        if [ $moved = 1 ] && [ ! -f "$ICHAT" ] && mv "$ICHAT_ORIG" "$ICHAT"; then
            say "Rolled back: the original ichat.dll is in place again." "Откат: оригинальный ichat.dll возвращён на место."
        fi
        exit 1
    fi
    say "Installed ichat.dll (btwsteam %s)." "Установлен ichat.dll (btwsteam %s)." "$pv"

    # 3. steam_api.dll; a foreign one is kept as steam_api_orig.dll
    if [ -f "$STEAM" ] && [ ! -f "$STEAM_ORIG" ]; then
        local cur; cur=$(sha "$STEAM")
        if [ "$cur" != "$steam_sha" ] && [ "$cur" != "$(rec steam_api.dll)" ]; then
            mv "$STEAM" "$STEAM_ORIG"
            say "The existing steam_api.dll is kept as steam_api_orig.dll." "Имевшийся steam_api.dll сохранён как steam_api_orig.dll."
        fi
    fi
    put "$src_steam" "$STEAM" || exit 1
    say "Installed steam_api.dll." "Установлен steam_api.dll."

    # 4. the record (same format as btwsteam-setup.exe)
    {
        echo "# btwsteam (Cossacks: Back to War multiplayer over Steam). Written by the installer,"
        echo "# read by --uninstall / --status (btwsteam-setup.exe and install.sh). Do not edit."
        echo "version=$pv"
        echo "installed=$(date '+%Y-%m-%d %H:%M:%S')"
        echo "installer=install.sh $VERSION"
        echo "game_bin=$BIN"
        echo "dmcr.exe=$(fsha "$BIN/dmcr.exe")"
        echo "ichat.dll=$(fsha "$ICHAT")"
        echo "ichat_orig.dll=$(fsha "$ICHAT_ORIG")"
        echo "steam_api.dll=$(fsha "$STEAM")"
        [ -f "$STEAM_ORIG" ] && echo "steam_api_orig.dll=$(fsha "$STEAM_ORIG")"
        echo "steam_appid_preexisting=$appid_pre"
    } > "$REC.btwtmp" && mv -f "$REC.btwtmp" "$REC" || say "Warning: cannot write %s" "Предупреждение: не удалось записать %s" "$REC"

    say "\nDone: btwsteam %s is installed in %s\nPlay: start the game from Steam -> Multiplayer -> Deathmatch -> \"GSC-Game.Net Internet Game\" -> Create.\nFriends first join your Steam lobby (your invite, or \"Join game\" in the friends list), then click the same Create.\nAfter Steam's \"Verify integrity of game files\" run this installer again. Log: bin/btwsteam.log" \
        "\nГотово: btwsteam %s установлен в %s\nИгра: запустите игру из Steam -> Multiplayer -> Deathmatch -> «GSC-Game.Net Internet Game» -> Create.\nДрузья сначала входят в ваше лобби Steam (приглашение или «Присоединиться к игре» в списке друзей), потом жмут ту же кнопку Create.\nПосле проверки целостности файлов в Steam запустите установщик снова. Журнал: bin/btwsteam.log" \
        "$pv" "$BIN"
}

# ---- uninstall ---------------------------------------------------------------------------------
cmd_uninstall() {
    refuse_if_running
    local rc=0 changed=0 vi vo
    vi=$(btw_ver "$ICHAT"); vo=$(btw_ver "$ICHAT_ORIG")
    if [ -f "$ICHAT_ORIG" ] && [ -z "$vo" ]; then
        if [ ! -f "$ICHAT" ] || [ -n "$vi" ]; then
            mv -f "$ICHAT_ORIG" "$ICHAT"; changed=1
            say "Restored the original ichat.dll." "Оригинальный ichat.dll восстановлен."
        elif [ "$(sha "$ICHAT")" = "$(sha "$ICHAT_ORIG")" ]; then
            rm -f "$ICHAT_ORIG"; changed=1
            say "ichat.dll is already the original; removed the extra ichat_orig.dll." "ichat.dll уже оригинальный; лишний ichat_orig.dll удалён."
        else
            say "ichat.dll is not btwsteam and differs from ichat_orig.dll; both left as they are." \
                "ichat.dll не из btwsteam и отличается от ichat_orig.dll; оба файла оставлены как есть."
        fi
    elif [ -n "$vi" ]; then
        say "Error: ichat.dll is btwsteam but there is no usable ichat_orig.dll to restore.\n  Use Steam's \"Verify integrity of game files\": it downloads the original ichat.dll." \
            "Ошибка: ichat.dll из btwsteam, а годного ichat_orig.dll для восстановления нет.\n  Выполните в Steam «Проверить целостность файлов игры»: Steam скачает оригинальный ichat.dll."
        rc=1
    else
        say "ichat.dll is the game's own; nothing to restore." "ichat.dll — файл игры; восстанавливать нечего."
    fi

    if [ -f "$STEAM_ORIG" ]; then
        mv -f "$STEAM_ORIG" "$STEAM"; changed=1
        say "Restored the previous steam_api.dll from steam_api_orig.dll." "Прежний steam_api.dll восстановлен из steam_api_orig.dll."
    elif [ -f "$STEAM" ]; then
        local cur ours=0; cur=$(sha "$STEAM")
        [ "$cur" = "$(rec steam_api.dll)" ] && ours=1
        [ -f "${BTWSTEAM_STEAM_API:-$SRC_STEAM}" ] && [ "$cur" = "$(sha "${BTWSTEAM_STEAM_API:-$SRC_STEAM}")" ] && ours=1
        if [ $ours = 1 ]; then
            rm -f "$STEAM"; changed=1; say "Removed steam_api.dll." "steam_api.dll удалён."
        else
            say "steam_api.dll was not installed by btwsteam; left in place." "steam_api.dll поставлен не btwsteam; оставлен."
        fi
    fi
    if appid_is_ours && [ "$(rec steam_appid_preexisting)" != 1 ]; then
        rm -f "$APPID_TXT"; changed=1; say "Removed steam_appid.txt." "steam_appid.txt удалён."
    fi
    if [ -f "$REC" ]; then rm -f "$REC" && changed=1 || rc=1; fi
    if [ $rc = 0 ]; then
        if [ $changed = 1 ]; then say "\nDone: btwsteam is removed. Logs (btwsteam.log) are kept." "\nГотово: btwsteam удалён. Журналы (btwsteam.log) оставлены."
        else say "\nbtwsteam was not installed; nothing changed." "\nbtwsteam не был установлен; ничего не изменено."; fi
    fi
    return $rc
}

case $cmd in
    status) cmd_status ;;
    uninstall) cmd_uninstall ;;
    install) cmd_install ;;
esac
