#!/bin/sh
# Builds and runs the service manager test (needs a mysql.h shim path in SHIM, see the other run_sim.sh scripts).
cd "$(dirname "$0")" || exit 1
gcc -Wall -DSM_TEST -I"${SHIM:?set SHIM to the directory that holds mariadb/mysql.h}" -I../../ -o /tmp/sm_test sm_test.c && /tmp/sm_test
