#!/bin/bash
# run_ysf_interop.sh <build dir of openmt_radio (with ysf_interop_tool)>
#
# The YSF mode against the repo's real ysf_host (built against the stubbed database):
#   1. receive: ysf_host transmits a call (ysf_sim sends it from the network), the radio receives the samples
#   2. transmit: the radio transmits a call, ysf_host receives the samples and passes the frames to the gateway
# Environment: SHIM=<dir with mariadb/mysql.h> when the MariaDB headers are not installed.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../../.."
tool="$(cd "$1" && pwd)/ysf_interop_tool"
work="${WORK:-/tmp/ysf_interop}"
mkdir -p "$work"
cc="${CC:-gcc}"
inc="-I$root/tools -I$root/tools/test -I$root/modem -I$root/protocols/YSF ${SHIM:+-I$SHIM}"
ysfc="$root/protocols/YSF/ysf_func.c $root/protocols/YSF/ysf_dsp.c $root/tools/dsp_tools.c $root/tools/fec_tools.c $root/tools/ADF7021.c"

"$root/tools/test/mk_tools_real.sh" "$root/tools/tools.c" "$work/tools_real.c"
$cc -O1 -w -DSTUB_UC=YSF -DSTUB_LC=ysf -DSTUB_REAL_TOOLS -DSTUB_DB $inc -o "$work/ysf_host" "$root/protocols/YSF/ysf_host.c" \
    $ysfc "$root/tools/string_builder.c" "$root/tools/RingBuffer.c" "$root/tools/test/tools_stub.c" "$work/tools_real.c" -lm -lpthread
$cc -O1 -w $inc -o "$work/ysf_sim" "$root/protocols/YSF/test/ysf_sim.c" $ysfc "$root/tools/test/sim_common.c" -lm
$cc -O1 -w $inc -o "$work/ysf_radio_sim" "$here/ysf_radio_sim.c" "$root/protocols/YSF/ysf_func.c" "$root/tools/fec_tools.c" \
    "$root/tools/test/sim_common.c" -lm

status_all=0
run_pair()     # <sim program> <sim args...>  with the host started after it
{
    rm -f /tmp/ysf_stub_*
    (timeout 120 "$@" > "$work/sim.log" 2>&1 &)
    sleep 1.5
    (YSF_STUB_MODEM=openmt YSF_STUB_rxLevel=255 timeout 120 "$work/ysf_host" -m 1 > "$work/host.log" 2>&1 &)
    for i in $(seq 1 110); do
        sleep 1
        if grep -q "^PASS\|^FAIL\|did not connect\|could not connect\|no registration" "$work/sim.log"; then break; fi
    done
    pkill -x ysf_host || true
    pkill -x ysf_sim || true
    pkill -x ysf_radio_sim || true
}

echo "=== 1. ysf_host transmits, the radio receives"
run_pair env YSF_SIM_DUMP="$work/host_tx" "$work/ysf_sim" -m 1
tail -3 "$work/sim.log"
"$tool" replay "$work/host_tx.openmt" || status_all=1

echo
echo "=== 2. the radio transmits, ysf_host receives"
"$tool" capture "$work/radio_tx.raw"
run_pair "$work/ysf_radio_sim" "$work/radio_tx.raw"
grep -v "^  ok" "$work/sim.log" || true
grep -q "^PASS" "$work/sim.log" || status_all=1
exit $status_all
