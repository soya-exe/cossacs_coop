#!/bin/bash
# Prove the real game install was not changed by the tests: compare the hashes recorded by
# mkcopies.sh (dmcr.exe, ichat.dll, csemu.dll, the symlinked ALL.GSC / camp.gs1, the user's
# players.txt / Players/*.txt / *.opt / mode.dat) and list any file in the install newer than
# that record.
# --rebaseline: re-record the same files as they are now (after the user knowingly changed the
# install, e.g. installed the mod or played there).
set -u
TOOLS=$(cd "$(dirname "$0")" && pwd)
. "$TOOLS/btwtest-env.sh"
rec="$BTW_TEST_ROOT/install.sha256"
[ -f "$rec" ] || _btw_die "no $rec (run tools/mkcopies.sh first)"
if [ "${1:-}" = --rebaseline ]; then
    files=$(awk '{ sub(/^[0-9a-f]+ [ *]/, ""); print }' "$rec")
    (cd "$BTW_GAME_DIR/bin" && while IFS= read -r f; do [ -e "$f" ] && sha256sum "$f"; done <<<"$files") > "$rec.new" &&
        mv "$rec.new" "$rec" && echo "re-recorded $(wc -l < "$rec") file hashes in $rec"
    exit
fi
rc=0
(cd "$BTW_GAME_DIR/bin" && sha256sum --quiet -c "$rec") || rc=1
newer=$(find "$BTW_GAME_DIR" -newer "$rec" -print 2>/dev/null)
if [ -n "$newer" ]; then echo "files in the install changed after $rec was written:"; echo "$newer"; rc=1; fi
[ $rc -eq 0 ] && echo "install unchanged" || echo "INSTALL CHANGED" >&2
exit $rc
