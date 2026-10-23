#!/bin/bash
# Created by Rick KD0OSS
#
# run_sim.sh [modem types]
#
# Builds dmr_host (against the stubbed database, with the real timers and
# packet queueing of tools.c) and dmr_sim, then runs the pair once for each
# modem type: mmdvm (frames), openmt (samples) and openmths (bits).
#
# Environment: BUILD=<dir>   where to build (default ./build_sim)
#              CC=<compiler> (default gcc)
#              SIMARGS=--duplex   run the duplex rebuild test (use with the mmdvm type only)
#              SHIM=<dir>    extra include directory, if the MariaDB headers are not installed

set -e
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../../.."
build="${BUILD:-$here/build_sim}"
cc="${CC:-gcc}"
inc="-I$root/tools -I$root/modem -I$here/.. ${SHIM:+-I$SHIM}"
types="${*:-mmdvm openmt openmths}"

mkdir -p "$build"
"$root/tools/test/mk_tools_real.sh" "$root/tools/tools.c" "$build/tools_real.c"

$cc -O1 -w -DSTUB_UC=DMR -DSTUB_LC=dmr -DSTUB_REAL_TOOLS $inc -o "$build/dmr_host" \
    "$here/../dmr_host.c" "$here/../dmr_func.c" "$root/tools/string_builder.c" "$root/tools/RingBuffer.c" \
    "$root/tools/fec_tools.c" "$root/tools/ADF7021.c" "$root/tools/test/tools_stub.c" "$build/tools_real.c" -lm -lpthread
$cc -O1 -w $inc -I"$root/tools/test" -o "$build/dmr_sim" "$here/dmr_sim.c" "$here/../dmr_func.c" \
    "$root/tools/fec_tools.c" "$root/tools/test/sim_common.c" -lm

status=0
for t in $types; do
    echo "=== DMR host with a $t modem"
    rm -f /tmp/dmr_stub_*
    (DMR_SIM_DEBUG=${DMR_SIM_DEBUG:-} DMR_SIM_DUMP=${DMR_SIM_DUMP:+$DMR_SIM_DUMP.$t} timeout 120 "$build/dmr_sim" -m 1 --modem "$t" $SIMARGS > "$build/sim_$t.log" 2>&1 &)
    sleep 1.5
    (DMR_STUB_MODEM=$t DMR_STUB_rxLevel=255 timeout 120 "$build/dmr_host" -m 1 $HOSTARGS > "$build/host_$t.log" 2>&1 &)
    for i in $(seq 1 110); do
        sleep 1
        if grep -q "^PASS\|^FAIL\|did not connect\|cannot listen\|could not connect\|no registration" "$build/sim_$t.log"; then break; fi
    done
    grep -v "^  ok" "$build/sim_$t.log" || true
    grep -q "^PASS" "$build/sim_$t.log" || status=1
    pkill -x dmr_host || true
    pkill -x dmr_sim || true
    sleep 1.5
done
exit $status
