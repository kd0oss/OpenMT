#!/bin/bash
# Created by Rick KD0OSS
#
# run_sim.sh [modem types]
#
# Builds m17_host (against the stubbed database, with the real timers and
# packet queueing of tools.c) and m17_sim, then runs the pair once for each
# modem type: mmdvm (frames), openmt (samples) and openmths (bits).
#
# Environment: BUILD=<dir>   where to build (default ./build_sim)
#              SHIM=<dir>    extra include directory, if the MariaDB headers are not installed
#              SIMARGS=...   extra options for m17_sim, e.g. "--gap 100"

set -e
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../../.."
build="${BUILD:-$here/build_sim}"
inc="-I$root/tools -I$root/modem -I$here/.. ${SHIM:+-I$SHIM}"
types="${*:-mmdvm openmt openmths}"

mkdir -p "$build"
"$root/tools/test/mk_tools_real.sh" "$root/tools/tools.c" "$build/tools_real.c"

m17src="$here/../M17/M17Callsign.cpp $here/../M17/M17DSP.cpp $here/../M17/M17FrameDecoder.cpp $here/../M17/M17FrameEncoder.cpp $here/../M17/M17LinkSetupFrame.cpp $here/../M17/M17Golay.cpp"

gcc -O1 -w -c -DSTUB_UC=M17 -DSTUB_LC=m17 -DSTUB_REAL_TOOLS $inc -o "$build/stub.o" "$root/tools/test/tools_stub.c"
gcc -O1 -w -c $inc -o "$build/real.o" "$build/tools_real.c"
gcc -O1 -w -c $inc -o "$build/sb.o" "$root/tools/string_builder.c"
gcc -O1 -w -c $inc -o "$build/rb.o" "$root/tools/RingBuffer.c"
gcc -O1 -w -c $inc -o "$build/adf.o" "$root/tools/ADF7021.c"
gcc -O1 -w -c $inc -I"$root/tools/test" -o "$build/simc.o" "$root/tools/test/sim_common.c"
g++ -std=c++17 -O1 -w $inc -o "$build/m17_host" "$here/../m17_host.cpp" $m17src \
    "$build/stub.o" "$build/real.o" "$build/sb.o" "$build/rb.o" "$build/adf.o" -lm -lpthread
g++ -std=c++17 -O1 -w $inc -I"$root/tools/test" -o "$build/m17_sim" "$here/m17_sim.cpp" $m17src "$build/simc.o" -lm

status=0
for t in $types; do
    echo "=== M17 host with a $t modem"
    rm -f /tmp/m17_stub_*
    (timeout 150 "$build/m17_sim" -m 1 --modem "$t" $SIMARGS > "$build/sim_$t.log" 2>&1 &)
    sleep 1.5
    (M17_STUB_MODEM=$t M17_STUB_rxLevel=255 timeout 150 "$build/m17_host" -m 1 > "$build/host_$t.log" 2>&1 &)
    for i in $(seq 1 140); do
        sleep 1
        if grep -q "^PASS\|^FAIL\|did not connect\|cannot listen\|could not connect\|no registration" "$build/sim_$t.log"; then break; fi
    done
    grep -v "^  ok" "$build/sim_$t.log" || true
    grep -q "^PASS" "$build/sim_$t.log" || status=1
    pkill -x m17_host || true
    pkill -x m17_sim || true
    sleep 1.5
done
exit $status
