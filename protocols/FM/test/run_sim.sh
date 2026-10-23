#!/bin/bash
# Created by Rick KD0OSS
#
# run_sim.sh
#
# Builds fm_host and fm_gateway against the stubbed database (with the real
# timers and packet queueing of tools.c) and runs them against fm_el_sim.py,
# which plays the modem host, the EchoLink directory and an EchoLink station.
#
# Environment: BUILD=<dir>   where to build (default ./build_sim)
#              CC=<compiler> (default gcc)
#              SHIM=<dir>    extra include directory, if the MariaDB headers are not installed
#              GSMINC=<dir>  directory with gsm.h, if libgsm-dev is not installed
#              GSMLIB=<file> the libgsm library to link (default -lgsm)

set -e
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../../.."
build="${BUILD:-$here/build_sim}"
cc="${CC:-gcc}"
inc="-I$root/tools -I$root/modem -I$here/.. ${SHIM:+-I$SHIM}"
gsminc="${GSMINC:+-I$GSMINC}"
gsmlib="${GSMLIB:--lgsm}"

mkdir -p "$build"
"$root/tools/test/mk_tools_real.sh" "$root/tools/tools.c" "$build/tools_real.c"

$cc -O1 -w -DSTUB_UC=FM -DSTUB_LC=fm -DSTUB_REAL_TOOLS $inc -o "$build/fm_host" \
    "$here/../fm_host.c" "$here/../fm_dsp.c" "$root/tools/string_builder.c" \
    "$root/tools/test/tools_stub.c" "$build/tools_real.c" -lm -lpthread
$cc -O1 -w -DSTUB_UC=FM -DSTUB_LC=fm -DSTUB_REAL_TOOLS $inc $gsminc -I"$here/../gateway" -o "$build/fm_gateway" \
    "$here/../gateway/fm_gateway.c" "$here/../gateway/elink.c" "$root/tools/string_builder.c" \
    "$root/tools/test/tools_stub.c" "$build/tools_real.c" $gsmlib -lm -lpthread

$cc -O1 -w -I"$here/.." -o "$build/fm_dsp_test" "$here/fm_dsp_test.c" "$here/../fm_dsp.c" -lm
echo "=== DTMF decoder and converters"
"$build/fm_dsp_test"

rm -f /tmp/fm_stub_* /tmp/fm_elink_*.list
export FM_STUB_MODEM=openmt FM_STUB_accessMode=carrier FM_STUB_hangTime=0.4 FM_STUB_modeHold=0 FM_STUB_idEnable=0
export FM_STUB_callsign=N0LINK-L FM_STUB_password=secret FM_STUB_servers=127.0.0.1:7200
export FM_STUB_audioPort=7198 FM_STUB_bindAddress=127.0.0.1 FM_STUB_location=Test
export FM_SIM_LOGDIR="$build"

# the simulator listens for the host first
timeout 140 python3 "$here/fm_el_sim.py" > "$build/sim.log" 2>&1 &
simpid=$!
sleep 1
(cd "$build" && timeout 150 "$build/fm_host" -m 1 > "$build/host.log" 2>&1 &)
sleep 0.5
(cd "$build" && timeout 150 "$build/fm_gateway" -m 1 > "$build/gateway.log" 2>&1 &)
status=0
wait $simpid || status=$?
cat "$build/sim.log"
pkill -f "$build/fm_host" 2>/dev/null || true
pkill -f "$build/fm_gateway" 2>/dev/null || true
exit $status
