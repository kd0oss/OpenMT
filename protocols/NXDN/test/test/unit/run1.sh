#!/bin/bash
# usage: run1.sh <modem> <extra sim args> ; env passes through
rm -f /tmp/nxdn_stub_*
(timeout 100 ./nxdn_sim -m 1 --hang 3 "$@" > sim.log 2>&1 &)
sleep 0.5
(NXDN_STUB_modeHang=3 timeout 100 ./nxdn_host -m 1 > host.log 2>&1 &)
for i in $(seq 1 90); do sleep 1; if grep -q "^PASS\|^FAIL" sim.log; then break; fi; done
cat sim.log | grep -v "^  ok"
pkill nxdn_host; sleep 0.3
