#!/bin/bash
# Created by Rick KD0OSS
#
# run_e2e.sh
#
# Runs the real Modem_Host and the real mmdvm_bridge against a fake MMDVM modem, with fake protocol services
# (e2e_test.py):
#
#   e2e_test.py (services) <-> Modem_Host <-> /dev/mmbridge <-> mmdvm_bridge <-> /dev/ttyMMsim <-> e2e_test.py (modem)
#
# Both programs use the stubbed database of tools/test/tools_stub.c.  Run it as root (it makes /dev links), with g++
# not needed.  Environment: BUILD=<dir> (default ./build_e2e), SHIM=<dir> for the MariaDB headers.

set -e
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../../.."
build="${BUILD:-$here/build_e2e}"
inc="-I$root/tools -I$root/modem -I$root/modem/mmdvm-bridge ${SHIM:+-I$SHIM}"
hs="$root/modem/OPENMT_HS/test"
export BUILD="$build"
mkdir -p "$build"

"$root/tools/test/mk_tools_real.sh" "$root/tools/tools.c" "$build/tools_real.c"
flags="-std=gnu11 -O1 -w $inc -I$root/tools/test -DSTUB_UC=MH -DSTUB_LC=mh -DSTUB_REAL_TOOLS"
gcc $flags -o "$build/Modem_Host" "$root/modem/Modem_Host.c" "$hs/mh_stub.c" "$root/tools/RingBuffer.c" \
    "$root/tools/test/tools_stub.c" "$build/tools_real.c" "$root/tools/bridge_common.c" -lm -lpthread -lutil
gcc $flags -DTX_TIMEOUT_MS=1500U -o "$build/mmdvm_bridge" "$root/modem/mmdvm-bridge/mmdvm_bridge.c" \
    "$root/modem/mmdvm-bridge/mmdvm_frames.c" "$root/modem/mmdvm-bridge/mode_space.c" "$root/tools/bridge_common.c" \
    "$root/tools/test/tools_stub.c" "$build/tools_real.c" -lm -lpthread -lutil

rm -f /tmp/mh_stub_* /dev/mmbridge /dev/ttyMMsim
: > "$build/host.log"
python3 "$here/e2e_test.py"
