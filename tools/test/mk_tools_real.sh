#!/bin/bash
# Created by Rick KD0OSS
#
# mk_tools_real.sh <tools.c> <output.c>
#
# Makes a small C file from the parts of tools.c that need no database: the
# bit counting tables, the delay and clock helpers, the socket and log
# helpers, the software timers, hexDump(), modemWriteBits() and
# modemQueueSamples().  A simulator build links it with tools_stub.c
# (compiled with -DSTUB_REAL_TOOLS) so the host under test runs the real
# timers and the real packet queueing code instead of stubs.

set -e
src="$1"
out="$2"

{
    echo "/* Made from $(basename "$src") by mk_tools_real.sh -- do not edit */"
    sed -n '/^#include "tools.h"/,/^#define MAX_LINE_LENGTH/p' "$src"
    awk '/^const uint8_t BITS_TABLE/ {p=1} /^ \* readConfig\(\)/ {if (p) {exit}} p {print}' "$src" | sed '$d'
    awk '/^TIMERS +timer\[TIMER_COUNT\];/ {p=1} p {print}' "$src"
} > "$out"
