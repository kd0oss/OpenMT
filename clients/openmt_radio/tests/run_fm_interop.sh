#!/bin/bash
# run_fm_interop.sh <build dir of openmt_radio (with fm_interop_tool)>
#
# The FM mode against the repo's real fm_host (built against the stubbed database), as a radio and a repeater:
#   1. the radio transmits a call (1 kHz speech with a CTCSS tone of 88.5 Hz): fm_host must open on the tone and repeat it
#   2. what fm_host transmits is received by the radio, between noise as a receiver gives without a carrier
# Environment: SHIM=<dir with mariadb/mysql.h> when the MariaDB headers are not installed.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../../.."
tool="$(cd "$1" && pwd)/fm_interop_tool"
work="${WORK:-/tmp/fm_interop}"
mkdir -p "$work"
cc="${CC:-gcc}"
fmsrc="$root/protocols/FM"
inc="-I$root/tools -I$root/tools/test -I$root/modem -I$fmsrc ${SHIM:+-I$SHIM}"

"$root/tools/test/mk_tools_real.sh" "$root/tools/tools.c" "$work/tools_real.c"
$cc -O1 -w -DSTUB_UC=FM -DSTUB_LC=fm -DSTUB_REAL_TOOLS $inc -o "$work/fm_host" "$fmsrc/fm_host.c" "$fmsrc/fm_dsp.c" \
    "$root/tools/string_builder.c" "$root/tools/test/tools_stub.c" "$work/tools_real.c" -lm -lpthread
$cc -O1 -w $inc -o "$work/fm_radio_sim" "$here/fm_radio_sim.c" "$root/tools/test/sim_common.c" -lm

echo "=== the radio transmits, fm_host repeats it, the radio receives it"
"$tool" capture "$work/radio_tx.raw"
rm -f /tmp/fm_stub_*
(timeout 120 "$work/fm_radio_sim" "$work/radio_tx.raw" "$work/host_tx.raw" > "$work/sim.log" 2>&1 &)
sleep 1.5
(FM_STUB_MODEM=openmt FM_STUB_accessMode=ctcss FM_STUB_ctcssRx=88.5 FM_STUB_ctcssTx=88.5 FM_STUB_hangTime=0.4 FM_STUB_modeHold=0 \
    FM_STUB_idEnable=0 timeout 120 "$work/fm_host" -m 1 > "$work/host.log" 2>&1 &)
for i in $(seq 1 110); do
    sleep 1
    if grep -q "^PASS\|^FAIL\|did not connect\|no registration\|cannot read" "$work/sim.log"; then break; fi
done
pkill -x fm_host || true
pkill -x fm_radio_sim || true
sleep 1
grep -v "^  ok" "$work/sim.log" || true
status=0
grep -q "^PASS" "$work/sim.log" || status=1

echo
echo "=== the radio receives what fm_host transmitted"
"$tool" replay "$work/host_tx.raw" || status=1
exit $status
