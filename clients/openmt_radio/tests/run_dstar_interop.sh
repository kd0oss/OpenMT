#!/bin/bash
# run_dstar_interop.sh <build dir of openmt_radio (with dstar_interop_tool)>
#
# The D-STAR mode against the repo's real dstar_host (built against the stubbed database):
#   1. receive: dstar_host transmits a call (dstar_sim sends it from the network), the radio receives the samples
#   2. transmit: the radio transmits a call, dstar_host receives the samples and passes the frames to the gateway
# Environment: SHIM=<dir with mariadb/mysql.h> when the MariaDB headers are not installed.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../../.."
tool="$(cd "$1" && pwd)/dstar_interop_tool"
work="${WORK:-/tmp/dstar_interop}"
mkdir -p "$work"
cc="${CC:-gcc}"
inc="-I$root/tools -I$root/tools/test -I$root/modem -I$root/protocols/DSTAR ${SHIM:+-I$SHIM}"
dsrc="$root/protocols/DSTAR"

"$root/tools/test/mk_tools_real.sh" "$root/tools/tools.c" "$work/tools_real.c"
$cc -O1 -w -DSTUB_UC=DSTAR -DSTUB_LC=dstar -DSTUB_REAL_TOOLS -DSTUB_DB $inc -o "$work/dstar_host" "$dsrc/dstar_host.c" "$dsrc/dstar_gps.c" \
    "$dsrc/dstar_slowdata.c" "$root/tools/CCITTChecksumReverse.c" "$root/tools/string_builder.c" "$root/tools/RingBuffer.c" \
    "$root/tools/fec_tools.c" "$root/tools/ADF7021.c" "$root/tools/test/tools_stub.c" "$work/tools_real.c" -lm -lpthread
$cc -O1 -w $inc -o "$work/dstar_sim" "$dsrc/test/dstar_sim.c" "$root/tools/CCITTChecksumReverse.c" "$root/tools/fec_tools.c" \
    "$root/tools/test/sim_common.c" -lm
$cc -O1 -w $inc -o "$work/dstar_radio_sim" "$here/dstar_radio_sim.c" "$root/tools/fec_tools.c" "$root/tools/test/sim_common.c" -lm

status_all=0
run_pair()     # <sim program> <sim args...>  with the host started after it
{
    rm -f /tmp/dstar_stub_*
    (timeout 120 "$@" > "$work/sim.log" 2>&1 &)
    sleep 1.5
    (DSTAR_STUB_MODEM=openmt DSTAR_STUB_rxLevel=255 DSTAR_STUB_rpt1Call='N0CALL B' DSTAR_STUB_rpt2Call='N0CALL G' timeout 120 "$work/dstar_host" -m 1 > "$work/host.log" 2>&1 &)
    for i in $(seq 1 110); do
        sleep 1
        if grep -q "^PASS\|^FAIL\|did not connect\|could not connect\|no registration" "$work/sim.log"; then break; fi
    done
    pkill -x dstar_host || true
    pkill -x dstar_sim || true
    pkill -x dstar_radio_sim || true
}

echo "=== 1. dstar_host transmits, the radio receives"
run_pair env DSTAR_SIM_DUMP="$work/host_tx" "$work/dstar_sim" -m 1 --modem openmt
tail -3 "$work/sim.log"
"$tool" replay "$work/host_tx.openmt" || status_all=1

echo
echo "=== 2. the radio transmits, dstar_host receives"
"$tool" capture "$work/radio_tx.raw"
run_pair "$work/dstar_radio_sim" "$work/radio_tx.raw"
grep -v "^  ok" "$work/sim.log" || true
grep -q "^PASS" "$work/sim.log" || status_all=1
exit $status_all
