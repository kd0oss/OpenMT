#!/bin/bash
# Created by Rick KD0OSS
#
# make_flowcharts.sh <output directory>
#
# Draws the OpenMT flow chart PDFs.  Needs python3 and reportlab (pip install reportlab).
# Run it again after the code changes; the charts are described in the Python files here.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
out="${1:-$here/..}"
mkdir -p "$out"
cd "$here"
python3 modem_host.py "$out/OpenMT_Modem_Host_Flow.pdf"
python3 hosts_all.py "$out"
python3 overall.py "$out/OpenMT_System_Flow.pdf"
ls -l "$out"/OpenMT_*_Flow.pdf
