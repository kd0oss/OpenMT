#!/bin/bash
# run_p25_interop.sh <build dir of openmt_radio (with p25_interop_tool)>
#
# The P25 mode against the repo's real p25_host (built against the stubbed database by protocols/P25/test/run_sim.sh):
#   1. receive: p25_host transmits a call (p25_sim sends it from the network), the radio receives the samples
#   2. transmit: the radio transmits a call, p25_host receives the samples and passes the frames to the gateway
# Environment: SHIM=<dir with mariadb/mysql.h> when the MariaDB headers are not installed.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../../.."
tool="$(cd "$1" && pwd)/p25_interop_tool"
work="${WORK:-/tmp/p25_interop}"
mkdir -p "$work"

echo "=== 1. p25_host transmits, the radio receives"
BUILD="$work/sim" P25_SIM_DUMP="$work/host_tx" bash "$root/protocols/P25/test/run_sim.sh" openmt > "$work/stock.log" 2>&1 || { cat "$work/stock.log"; exit 1; }
tail -3 "$work/stock.log"
status_all=0
"$tool" replay "$work/host_tx.openmt" || status_all=1

echo
echo "=== 2. the radio transmits, p25_host receives"
"$tool" capture "$work/radio_tx.raw"
inc="-I$root/tools -I$root/tools/test -I$root/protocols/P25"
gcc -O1 -w $inc -o "$work/p25_radio_sim" "$here/p25_radio_sim.c" "$root/protocols/P25/p25_func.c" "$root/tools/fec_tools.c" \
    "$root/tools/test/sim_common.c" -lm
rm -f /tmp/p25_stub_*
(timeout 120 "$work/p25_radio_sim" "$work/radio_tx.raw" > "$work/radio_sim.log" 2>&1 &)
sleep 1.5
(P25_STUB_MODEM=openmt P25_STUB_rxLevel=255 P25_STUB_NAC=0x293 timeout 120 "$work/sim/p25_host" -m 1 > "$work/radio_host.log" 2>&1 &)
for i in $(seq 1 110); do
    sleep 1
    if grep -q "^PASS\|^FAIL\|did not connect\|could not connect\|no registration" "$work/radio_sim.log"; then break; fi
done
grep -v "^  ok" "$work/radio_sim.log" || true
grep -q "^PASS" "$work/radio_sim.log" || status_all=1
pkill -x p25_host || true
pkill -x p25_radio_sim || true
exit $status_all
