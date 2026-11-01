#!/bin/bash
# Created by Rick KD0OSS
#
# run_error_test.sh - builds and runs error_test.c against the real reportError()
# in tools.c.  Usage: SHIM=<dir with mariadb/mysql.h> run_error_test.sh <root>

root="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
here="$(cd "$(dirname "$0")" && pwd)"
shim="${SHIM:?set SHIM to a directory that holds mariadb/mysql.h}"
out="${BUILD:-/tmp/error_test_build}"
mkdir -p "$out"

# The test defines its own logError(); the linker is told to take the first
# definition (the test's) over the one in tools.c, and the database calls that
# tools.c makes are never reached, so they are left unresolved.
gcc -O1 -w -fno-inline -I"$shim" -I"$root/tools" -I"$root/modem" -c -o "$out/tools.o" "$root/tools/tools.c" 2>"$out/build.log" || { head -20 "$out/build.log"; exit 2; }
gcc -O1 -w -I"$shim" -I"$root/tools" -I"$root/modem" -o "$out/error_test" "$here/error_test.c" "$out/tools.o" "$root/tools/string_builder.c" \
    -Wl,--allow-multiple-definition -Wl,--unresolved-symbols=ignore-all -lpthread -lm 2>>"$out/build.log" || { head -20 "$out/build.log"; exit 2; }
"$out/error_test"
