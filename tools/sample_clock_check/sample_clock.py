#!/usr/bin/env python3
# Rick KD0OSS
#
# Measures how far the sample clock of the OpenMT sample firmware is from 24000 Hz, from its debug reports in the
# journal of Modem_Host (the "Modem rec data:: <ms>" line followed by the dump of the "FW rx=<frames> ..." report).
# Every frame carries 96 samples, so 250 frames a second is exactly 24000 samples a second.
#
#   sudo journalctl -u modem-host --since "15 min ago" --no-pager | python3 sample_clock.py
#
# It prints the clock error in ppm and the SAMPLE_TRIM_PPM to build with.  Give the trim the firmware that
# produced the log was built with as the first argument (198 if it was built with the defaults).

import re
import sys

trim = int(sys.argv[1]) if len(sys.argv) > 1 else 198
points = []
last_ms = None
for line in sys.stdin:
    m = re.search(r'Modem rec data::\s+(\d+)\s+\(ms\)', line)
    if m:
        last_ms = int(m.group(1))
        continue
    # the ascii column of the dump:  *a..FW rx=38742 t*
    m = re.search(r'\*a\.\.FW rx=(\d+)', line)
    if m and last_ms is not None:
        points.append((last_ms, int(m.group(1))))
        last_ms = None

if len(points) < 3:
    sys.exit("Found %d reports; need at least 3, and 10 minutes or more apart for a good figure." % len(points))

import time


def fit(seg):
    """Least squares line through (seconds, frames); returns (frames per second, span in seconds)."""
    t0 = seg[0][0]
    xs = [(p[0] - t0) / 1000.0 for p in seg]
    ys = [p[1] for p in seg]
    n = len(seg)
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return 0.0, 0.0
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx, xs[-1] - xs[0]


# The counter only goes back to 0 when the firmware starts, so a drop means the modem restarted (or was flashed).
segments = [[points[0]]]
resets = []
for prev, cur in zip(points, points[1:]):
    if cur[1] < prev[1]:
        resets.append(cur[0])
        segments.append([cur])
    else:
        segments[-1].append(cur)

print("reports: %d over %.0f s" % (len(points), (points[-1][0] - points[0][0]) / 1000.0))
if resets:
    print("The modem RESTARTED %d time(s) during the log, at:" % len(resets))
    for r in resets[:20]:
        print("   " + time.strftime("%H:%M:%S", time.localtime(r / 1000.0)))
    if len(resets) > 20:
        print("   ...")

total_w = 0.0
total_ppm = 0.0
for seg in segments:
    if len(seg) < 3:
        continue
    rate, span = fit(seg)
    if span < 30:
        continue
    ppm = (rate / 250.0 - 1.0) * 1e6
    total_w += span
    total_ppm += ppm * span
    print("   stretch of %.0f s: %.3f frames/s = %+.0f ppm" % (span, rate, ppm))

if total_w < 30:
    sys.exit("No stretch of at least 30 s without a restart: the clock cannot be measured until the restarts are fixed.")
ppm = total_ppm / total_w
print("sample clock is %+.0f ppm from 24000 Hz (positive = fast), from %.0f s of data" % (ppm, total_w))
if total_w < 300:
    print("The data is short (<5 min): the figure is only good to about +-%d ppm." % round(1e6 / (250.0 * total_w)))
print("Built with SAMPLE_TRIM_PPM=%d, so use SAMPLE_TRIM_PPM=%d." % (trim, trim + round(ppm)))
if abs(ppm) > 3000:
    print("That is a large error: check EXT_CLK_HZ (0 if the board has no external clock) before trimming.")
