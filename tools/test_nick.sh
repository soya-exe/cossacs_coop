#!/bin/sh
# Host-side unit test of the nick sanitizer (src/hook/nick.c) with the system C compiler.
# Prints PASS or FAIL as the last line; exit code 0 / 1.
set -eu
cd "$(dirname "$0")/.."
HOSTCC=${HOSTCC:-gcc}
mkdir -p build/test
$HOSTCC -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o build/test/test_nick src/hook/nick.c src/test/test_nick.c
exec build/test/test_nick
