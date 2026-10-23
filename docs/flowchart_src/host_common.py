# Created by Rick KD0OSS
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# host_common.py - lays out the flow chart of a protocol host from a description.
from chartlib import Chart


def box(c, name, col, row, spec, default_kind="proc"):
    """spec is a string, or a dict with text, kind, w."""
    if isinstance(spec, str):
        spec = {"text": spec}
    kind = spec.get("kind", default_kind)
    w = spec.get("w", 122 if kind == "dec" else 150)
    return c.node(name, col, row, spec["text"], kind, w=w)


def lane_path(c, prefix, items, row, first, last, firstlbl, lastlbl, up_row, down_row, first_to_last=None):
    """Places first (an outside node) in column 0, items in columns 1 to 5 and
    last (an outside node) in column 6, linking them in a row.  Items may carry
    up and down side boxes, drawn above and below them."""
    box(c, prefix + "F", 0, row, first, "ext")
    box(c, prefix + "L", 6, row, last, "ext")
    prev = prefix + "F"
    lbl = firstlbl
    for i, it in enumerate(items):
        if isinstance(it, str):
            it = {"text": it}
        name = "%s%d" % (prefix, i + 1)
        box(c, name, i + 1, row, it)
        c.link(prev, name, lbl)
        lbl = it.get("out")
        prev = name
        for side, r, s, e in (("up", up_row, "t", "b"), ("down", down_row, "b", "t")):
            if side in it:
                label, text = it[side][0], it[side][1]
                sname = name + side
                box(c, sname, i + 1, r, {"text": text, "w": 164}, "proc")
                c.link(name, sname, label, s=s, e=e)
                if len(it[side]) > 2 and it[side][2]:
                    # the side box leads to the far outside node
                    c.link(sname, prefix + "L", None, s="r", e=("t" if side == "up" else "b"))
    c.link(prev, prefix + "L", lbl)


def render(path, spec):
    c = Chart(path, 1224, 792, spec["title"], spec["subtitle"], cellw=172, cellh=66, ox=100, oy=105)

    # lane 1: start-up
    c.lane(0, -0.6, 7, 1.2, "main()  -  start-up")
    xs = spec["start"]
    for i, t in enumerate(xs):
        c.node("s%d" % i, i + (7 - len(xs)) // 2 * 0, 0.0, t, "term" if i == 0 or i == len(xs) - 1 else "proc",
               w=160) if False else None
    n = len(xs)
    cols = [0, 1.15, 2.3, 3.45, 4.6, 5.75, 6.9][:n] if n > 5 else [0, 1.5, 3.0, 4.5, 6.0][:n]
    for i, t in enumerate(xs):
        kind = "term" if i == 0 else "proc"
        c.node("s%d" % i, cols[i], 0.0, t, kind, w=165 if n <= 5 else 150)
    for i in range(n - 1):
        c.link("s%d" % i, "s%d" % (i + 1))

    # lane 2: modem -> network
    c.lane(0, 0.7, 7, 3.0, spec["rx_title"])
    mh = {"text": "Modem Host\nTCP :18000+N-1", "w": 126}
    gw = {"text": spec["gw_name"], "w": 130} if "gw_name" in spec else None
    lane_path(c, "r", spec["rx"], 2.2, spec.get("rx_first", mh), spec.get("rx_last", gw),
              spec.get("rx_l1", "packets"), spec.get("rx_l2", "frames"), 1.2, 3.2)
    # lane 3: network -> modem
    c.lane(0, 3.85, 7, 3.0, spec["tx_title"])
    lane_path(c, "t", spec["tx"], 5.35, spec.get("tx_first", gw), spec.get("tx_last", mh),
              spec.get("tx_l1", "packets"), spec.get("tx_l2", "to modem"), 4.35, 6.35)

    # lane 4: background
    c.lane(0, 7.0, 7, 1.6, spec.get("bg_title", "Timers, dashboard commands and the database"))
    bg = spec["bg"]
    xs4 = [0.5, 2.5, 4.4][:len(bg)]
    for i, t in enumerate(bg):
        c.node("b%d" % i, xs4[i], 7.8, t, "proc", w=330 if i == 1 else 285)
    c.node("db", 5.9, 7.8, "MariaDB\nconfig, calls, history, dashboard commands, errors", "ext", w=175)
    for i in range(len(bg)):
        pass
    c.link("b%d" % (len(bg) - 1), "db", None, both=True)

    c.legend(0, 9.05)
    c.footer(spec["footer"])
    c.save()
