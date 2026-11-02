#!/bin/bash
# run_m17_interop.sh <build dir of openmt_radio (with m17_interop_tool)>
#
# The M17 mode against the repo's real m17_host (built against the stubbed database):
#   1. receive: m17_host transmits a call (m17_sim sends it from the network), the radio receives the samples
#   2. transmit: the radio transmits a call, m17_host receives the samples and passes the frames to the gateway
# Environment: SHIM=<dir with mariadb/mysql.h> when the MariaDB headers are not installed.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../../.."
tool="$(cd "$1" && pwd)/m17_interop_tool"
work="${WORK:-/tmp/m17_interop}"
mkdir -p "$work"
msrc="$root/protocols/m17"
inc="-I$root/tools -I$root/tools/test -I$root/modem -I$msrc ${SHIM:+-I$SHIM}"
lib="$msrc/M17/M17Callsign.cpp $msrc/M17/M17DSP.cpp $msrc/M17/M17FrameDecoder.cpp $msrc/M17/M17FrameEncoder.cpp $msrc/M17/M17LinkSetupFrame.cpp $msrc/M17/M17Golay.cpp"

"$root/tools/test/mk_tools_real.sh" "$root/tools/tools.c" "$work/tools_real.c"
gcc -O1 -w -c -DSTUB_UC=M17 -DSTUB_LC=m17 -DSTUB_REAL_TOOLS $inc -o "$work/stub.o" "$root/tools/test/tools_stub.c"
gcc -O1 -w -c $inc -o "$work/real.o" "$work/tools_real.c"
gcc -O1 -w -c $inc -o "$work/sb.o" "$root/tools/string_builder.c"
gcc -O1 -w -c $inc -o "$work/rb.o" "$root/tools/RingBuffer.c"
gcc -O1 -w -c $inc -o "$work/adf.o" "$root/tools/ADF7021.c"
gcc -O1 -w -c $inc -o "$work/simc.o" "$root/tools/test/sim_common.c"
g++ -std=c++17 -O1 -w $inc -o "$work/m17_host" "$msrc/m17_host.cpp" $lib "$work/stub.o" "$work/real.o" "$work/sb.o" "$work/rb.o" "$work/adf.o" -lm -lpthread
g++ -std=c++17 -O1 -w $inc -o "$work/m17_sim" "$msrc/test/m17_sim.cpp" $lib "$work/simc.o" -lm
g++ -std=c++17 -O1 -w $inc -o "$work/m17_radio_sim" "$here/m17_radio_sim.cpp" $lib "$work/simc.o" -lm

status_all=0
run_pair()     # <sim program> <sim args...>  with the host started after it
{
    rm -f /tmp/m17_stub_*
    (timeout 120 "$@" > "$work/sim.log" 2>&1 &)
    sleep 1.5
    (M17_STUB_MODEM=openmt M17_STUB_rxLevel=255 timeout 120 "$work/m17_host" -m 1 > "$work/host.log" 2>&1 &)
    for i in $(seq 1 110); do
        sleep 1
        if grep -q "^PASS\|^FAIL\|did not connect\|could not connect\|no registration" "$work/sim.log"; then break; fi
    done
    pkill -x m17_host || true
    pkill -x m17_sim || true
    pkill -x m17_radio_sim || true
    sleep 1
}

echo "=== 1. m17_host transmits, the radio receives"
run_pair env M17_SIM_DUMP="$work/host_tx" "$work/m17_sim" -m 1 --modem openmt
tail -3 "$work/sim.log"
"$tool" replay "$work/host_tx.openmt" || status_all=1

echo
echo "=== 2. the radio transmits, m17_host receives"
"$tool" capture "$work/radio_tx.raw"
run_pair "$work/m17_radio_sim" "$work/radio_tx.raw"
grep -v "^  ok" "$work/sim.log" || true
grep -q "^PASS" "$work/sim.log" || status_all=1

echo
echo "=== 3. the radio sends a text message in packet mode, m17_host receives"
"$tool" capture-msg "$work/radio_msg.raw"
run_pair "$work/m17_radio_sim" "$work/radio_msg.raw" --packet
grep -v "^  ok" "$work/sim.log" || true
grep -q "^PASS" "$work/sim.log" || status_all=1
grep -a "Packet data detected" "$work/host.log" > /dev/null && echo "m17_host: Packet data detected (it took the message for an SMS)" || echo "m17_host: no 'Packet data detected' in its log"
exit $status_all
