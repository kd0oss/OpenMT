#!/bin/bash
# run_dmr_interop.sh <build dir of openmt_radio (with dmr_interop_tool)>
#
# The DMR mode against the repo's real dmr_host (built against the stubbed database by protocols/DMR/test/run_sim.sh):
#   1. receive: dmr_host transmits a call (dmr_sim sends it from the network), the radio receives the samples
#   2. transmit: the radio transmits a call, dmr_host receives the samples and passes the call to the gateway
#   3. repeater, receive: dmr_host sends its two slot downlink (duplex) with a call in slot 1 and in slot 2, the radio listens
#      on each slot: it must decode the call of its own slot and hear nothing of the other
#   4. repeater, transmit timing: the radio keys up against that downlink (modelled modem, one clock for both directions)
#      and its bursts must land in the right place of the TDMA cycle, with and without the modem's TX timing report
#   5. repeater, both ends: the radio and the real dmr_host with an idle repeater: wake-up, downlink, timed call, the call
#      has to reach the gateway in the radio's time slot
# Environment: SHIM=<dir with mariadb/mysql.h> when the MariaDB headers are not installed.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../../.."
tool="$(cd "$1" && pwd)/dmr_interop_tool"
work="${WORK:-/tmp/dmr_interop}"
mkdir -p "$work"

echo "=== 1. dmr_host transmits, the radio receives"
BUILD="$work/sim" DMR_SIM_DUMP="$work/host_tx" bash "$root/protocols/DMR/test/run_sim.sh" openmt > "$work/stock.log" 2>&1 || { cat "$work/stock.log"; exit 1; }
tail -3 "$work/stock.log"
"$tool" replay "$work/host_tx.openmt"

echo
echo "=== 2. the radio transmits, dmr_host receives"
"$tool" capture "$work/radio_tx.raw" "$work/radio_bursts.txt"
inc="-I$root/tools -I$root/tools/test"
gcc -O1 -w $inc -o "$work/dmr_radio_sim" "$here/dmr_radio_sim.c" "$root/tools/test/sim_common.c" -lm
rm -f /tmp/dmr_stub_*
(timeout 120 "$work/dmr_radio_sim" "$work/radio_tx.raw" "$work/radio_bursts.txt" > "$work/radio_sim.log" 2>&1 &)
sleep 1.5
(DMR_STUB_MODEM=openmt DMR_STUB_rxLevel=255 timeout 120 "$work/sim/dmr_host" -m 1 > "$work/radio_host.log" 2>&1 &)
for i in $(seq 1 110); do
    sleep 1
    if grep -q "^PASS\|^FAIL\|did not connect\|could not connect\|no registration" "$work/radio_sim.log"; then break; fi
done
grep -v "^  ok" "$work/radio_sim.log" || true
status=0
grep -q "^PASS" "$work/radio_sim.log" || status=1
pkill -x dmr_host || true
pkill -x dmr_radio_sim || true

status_all=$status

echo
echo "=== 3. repeater: the radio listens to the two slot downlink of dmr_host"
for host in 1 2; do
    rm -f "$work"/dl_$host.openmt
    if [ $host = 2 ]; then slot2=1; else slot2=; fi
    ( [ -n "$slot2" ] && export DMR_SIM_SLOT2=1; BUILD="$work/sim" DMR_SIM_DUPLEX_SAMPLE=1 DMR_SIM_DUMP="$work/dl_$host" \
        bash "$root/protocols/DMR/test/run_sim.sh" openmt > "$work/dl_$host.log" 2>&1 ) || true
    for radio in 1 2; do
        out=$(DMR_REPLAY_REPEATER=$radio "$tool" replay "$work/dl_$host.openmt" 2>&1 || true)
        if [ $host = $radio ]; then
            if echo "$out" | grep -q "^PASS"; then echo "  call in slot $host, radio on slot $radio: decoded  (ok)"; else echo "  call in slot $host, radio on slot $radio: NOT decoded  FAIL"; status_all=1; fi
        else
            if echo "$out" | grep -q "0 speech blocks"; then echo "  call in slot $host, radio on slot $radio: ignored  (ok)"; else echo "  call in slot $host, radio on slot $radio: heard the other slot  FAIL"; status_all=1; fi
        fi
    done
done

echo
echo "=== 4. repeater: where the radio's bursts land in the cycle"
for slot in 1 2; do
    for report in report noreport; do
        out=$("$tool" repeater-tx "$work/dl_1.openmt" $slot $report 0 2>&1 || true)
        echo "$out" | grep "uplink sync" || echo "$out" | tail -3
        echo "$out" | grep -q "^PASS" || { echo "  slot $slot $report: FAIL"; status_all=1; }
    done
done

echo
echo "=== 5. repeater: the radio and the real dmr_host, idle repeater"
for slot in 1 2; do
    rm -f /tmp/dmr_stub_*
    (timeout 60 "$tool" repeater-loop $slot > "$work/loop_$slot.log" 2>&1 &)
    sleep 2
    (DMR_STUB_MODEM=openmt DMR_STUB_rxLevel=255 timeout 60 "$work/sim/dmr_host" -m 1 -d > "$work/loop_host_$slot.log" 2>&1 &)
    for i in $(seq 1 50); do
        sleep 1
        if grep -q "^PASS\|^FAIL\|did not\|could not\|no registration" "$work/loop_$slot.log"; then break; fi
    done
    pkill -x dmr_host || true
    echo "  time slot $slot: $(grep '^PASS\|^FAIL' "$work/loop_$slot.log" || echo 'no result')"
    grep "FAIL" "$work/loop_$slot.log" | grep -v "^FAIL$" || true
    grep -q "^PASS" "$work/loop_$slot.log" || status_all=1
done
exit $status_all
