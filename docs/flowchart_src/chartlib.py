# Created by Rick KD0OSS
#
# chartlib.py - a small flow chart drawing library for the OpenMT documents.
#
# It draws boxes, decisions, data stores and arrows on a PDF page (reportlab),
# on a grid: a node is placed by column and row, and a connector is routed with
# straight and right-angle segments between the sides of two nodes.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.

from reportlab.pdfgen import canvas
from reportlab.pdfbase.pdfmetrics import stringWidth
from reportlab.lib.colors import HexColor, black, white

FONT = "Helvetica"
FONTB = "Helvetica-Bold"

# colours: fill, border
KINDS = {
    "proc":  ("#FFFFFF", "#33475B"),   # a step
    "dec":   ("#FFF4C2", "#8A6D00"),   # a decision
    "term":  ("#DCE9F5", "#1F4E79"),   # start / end
    "ext":   ("#F8D7E0", "#A03050"),   # outside the program: network, DB, radio
    "hw":    ("#D5EBDD", "#2E6B45"),   # hardware / modem
    "buf":   ("#E8E8F4", "#4A4A8A"),   # a queue or buffer
    "thr":   ("#C9D9EA", "#1F4E79"),   # a thread's own title
    "note":  ("#FFFFFF", "#999999"),
    "host":  ("#C9D9EA", "#1F4E79"),
}


def wrap(text, width, size, font=FONT):
    """Breaks text into lines that fit a width.  A \\n starts a new line."""
    lines = []
    for para in text.split("\n"):
        words = para.split(" ")
        cur = ""
        for w in words:
            t = (cur + " " + w).strip()
            if stringWidth(t, font, size) <= width or not cur:
                cur = t
            else:
                lines.append(cur)
                cur = w
        lines.append(cur)
    return lines


class Node:
    def __init__(self, name, x, y, w, h, kind, lines, size, bold=False):
        self.name, self.x, self.y, self.w, self.h = name, x, y, w, h
        self.kind, self.lines, self.size, self.bold = kind, lines, size, bold

    def side(self, s):
        if s == "l":
            return (self.x - self.w / 2, self.y)
        if s == "r":
            return (self.x + self.w / 2, self.y)
        if s == "t":
            return (self.x, self.y + self.h / 2)
        return (self.x, self.y - self.h / 2)


class Chart:
    def __init__(self, path, w, h, title, subtitle="", cellw=150, cellh=64, ox=40, oy=80):
        self.c = canvas.Canvas(path, pagesize=(w, h))
        self.W, self.H = w, h
        self.cw, self.ch, self.ox, self.oy = cellw, cellh, ox, oy
        self.nodes = {}
        self.title = title
        self.pages = 1
        self.c.setTitle(title)
        self.c.setAuthor("Rick KD0OSS")
        self.header(title, subtitle)

    # ---- coordinates: column/row (row grows downwards) -> page points
    def px(self, col):
        return self.ox + col * self.cw

    def py(self, row):
        return self.H - (self.oy + row * self.ch)

    def header(self, title, subtitle):
        c = self.c
        c.setFont(FONTB, 20)
        c.setFillColor(black)
        c.drawString(40, self.H - 40, title)
        if subtitle:
            c.setFont(FONT, 10)
            c.setFillColor(HexColor("#555555"))
            c.drawString(40, self.H - 58, subtitle)
        c.setFont(FONT, 7)
        c.setFillColor(HexColor("#777777"))
        c.drawRightString(self.W - 40, self.H - 36, "OpenMT  -  Rick KD0OSS")
        c.setFillColor(black)

    # ---- lanes and notes
    def lane(self, col, row, cols, rows, title, fill="#F3F6FA", border="#B8C4D2"):
        c = self.c
        x = self.px(col) - self.cw / 2
        y = self.py(row + rows)
        w, h = cols * self.cw, rows * self.ch
        c.setFillColor(HexColor(fill))
        c.setStrokeColor(HexColor(border))
        c.setLineWidth(0.8)
        c.setDash(3, 2)
        c.roundRect(x, y, w, h, 6, stroke=1, fill=1)
        c.setDash()
        c.setFillColor(HexColor("#3A5068"))
        c.setFont(FONTB, 9)
        c.drawString(x + 8, y + h - 14, title)
        c.setFillColor(black)

    def text(self, col, row, txt, size=8, bold=False, color="#333333", width=None, align="l"):
        c = self.c
        x, y = self.px(col), self.py(row)
        font = FONTB if bold else FONT
        lines = wrap(txt, width, size, font) if width else txt.split("\n")
        c.setFont(font, size)
        c.setFillColor(HexColor(color))
        for i, ln in enumerate(lines):
            if align == "c":
                c.drawCentredString(x, y - i * (size + 2), ln)
            elif align == "r":
                c.drawRightString(x, y - i * (size + 2), ln)
            else:
                c.drawString(x, y - i * (size + 2), ln)
        c.setFillColor(black)

    # ---- nodes
    def node(self, name, col, row, text, kind="proc", w=None, h=None, size=8, bold=False, dx=0, dy=0):
        w = w or self.cw - 24
        font = FONTB if bold else FONT
        inner = w - (36 if kind == "dec" else 12)
        if kind == "dec":
            inner = w * 0.62
        lines = wrap(text, inner, size, font)
        need = len(lines) * (size + 2) + 12
        if kind == "dec":
            need = max(need * 1.5, 40)
        h = h or max(need, 30)
        x, y = self.px(col) + dx, self.py(row) + dy
        n = Node(name, x, y, w, h, kind, lines, size, bold)
        self.nodes[name] = n
        self.drawnode(n)
        return n

    def drawnode(self, n):
        c = self.c
        fill, border = KINDS.get(n.kind, KINDS["proc"])
        c.setFillColor(HexColor(fill))
        c.setStrokeColor(HexColor(border))
        c.setLineWidth(1.1)
        x0, y0 = n.x - n.w / 2, n.y - n.h / 2
        if n.kind == "dec":
            p = c.beginPath()
            p.moveTo(n.x, n.y + n.h / 2)
            p.lineTo(n.x + n.w / 2, n.y)
            p.lineTo(n.x, n.y - n.h / 2)
            p.lineTo(n.x - n.w / 2, n.y)
            p.close()
            c.drawPath(p, stroke=1, fill=1)
        elif n.kind in ("term", "host", "thr"):
            c.roundRect(x0, y0, n.w, n.h, min(n.h / 2, 14), stroke=1, fill=1)
        elif n.kind == "ext":
            c.roundRect(x0, y0, n.w, n.h, 4, stroke=1, fill=1)
            c.setDash(2, 2)
            c.roundRect(x0 + 2, y0 + 2, n.w - 4, n.h - 4, 3, stroke=1, fill=0)
            c.setDash()
        elif n.kind == "buf":
            c.rect(x0, y0, n.w, n.h, stroke=1, fill=1)
            c.line(x0 + 7, y0, x0 + 7, y0 + n.h)
            c.line(x0 + n.w - 7, y0, x0 + n.w - 7, y0 + n.h)
        elif n.kind == "note":
            c.setDash(2, 2)
            c.rect(x0, y0, n.w, n.h, stroke=1, fill=1)
            c.setDash()
        else:
            c.rect(x0, y0, n.w, n.h, stroke=1, fill=1)
        c.setFillColor(black)
        c.setFont(FONTB if n.bold or n.kind in ("thr", "host") else FONT, n.size)
        total = len(n.lines) * (n.size + 2)
        ty = n.y + total / 2 - n.size
        for i, ln in enumerate(n.lines):
            c.drawCentredString(n.x, ty - i * (n.size + 2) + 1.5, ln)

    # ---- connectors
    def arrow(self, pts, label=None, dashed=False, color="#33475B", both=False, lpos=0.5, lside=0):
        c = self.c
        c.setStrokeColor(HexColor(color))
        c.setFillColor(HexColor(color))
        c.setLineWidth(1.0)
        if dashed:
            c.setDash(4, 3)
        p = c.beginPath()
        p.moveTo(*pts[0])
        for q in pts[1:]:
            p.lineTo(*q)
        c.drawPath(p, stroke=1, fill=0)
        c.setDash()
        self.head(pts[-1], pts[-2])
        if both:
            self.head(pts[0], pts[1])
        if label:
            self.label(pts, label, lpos, lside)
        c.setFillColor(black)

    def head(self, tip, frm, size=5.5):
        import math
        c = self.c
        a = math.atan2(tip[1] - frm[1], tip[0] - frm[0])
        p = c.beginPath()
        p.moveTo(*tip)
        p.lineTo(tip[0] - size * math.cos(a - 0.45), tip[1] - size * math.sin(a - 0.45))
        p.lineTo(tip[0] - size * math.cos(a + 0.45), tip[1] - size * math.sin(a + 0.45))
        p.close()
        c.drawPath(p, stroke=0, fill=1)

    def label(self, pts, text, lpos=0.5, lside=0):
        # the label goes on the longest segment
        best, bl = 0, 0
        for i in range(len(pts) - 1):
            L = abs(pts[i + 1][0] - pts[i][0]) + abs(pts[i + 1][1] - pts[i][1])
            if L > bl:
                best, bl = i, L
        a, b = pts[best], pts[best + 1]
        mx = a[0] + (b[0] - a[0]) * lpos
        my = a[1] + (b[1] - a[1]) * lpos
        c = self.c
        size = 7
        lines = text.split("\n")
        wd = max(stringWidth(l, FONT, size) for l in lines) + 4
        ht = len(lines) * (size + 1) + 2
        if lside:
            if abs(b[0] - a[0]) > abs(b[1] - a[1]):
                my += lside * (ht / 2 + 2)
            else:
                mx += lside * (wd / 2 + 2)
        c.setFillColor(white)
        c.setStrokeColor(white)
        c.rect(mx - wd / 2, my - ht / 2, wd, ht, stroke=0, fill=1)
        c.setFillColor(HexColor("#222222"))
        c.setFont(FONT, size)
        for i, l in enumerate(lines):
            c.drawCentredString(mx, my + ht / 2 - size - 0.5 - i * (size + 1), l)
        c.setFillColor(black)

    def link(self, a, b, label=None, s=None, e=None, via=None, dashed=False, both=False,
             color="#33475B", lpos=0.5, lside=0, mid=None):
        """Connects node a to node b.  s and e name the sides ('l','r','t','b');
        when omitted they are chosen from the relative positions.  via is a list
        of (col,row) waypoints (page points if given as ('p',x,y)); mid moves the
        middle segment of an elbow (a page offset)."""
        A, B = self.nodes[a], self.nodes[b]
        dx, dy = B.x - A.x, B.y - A.y
        if s is None or e is None:
            if abs(dx) < 3:
                s0, e0 = ("b", "t") if dy < 0 else ("t", "b")
            elif abs(dy) < 3:
                s0, e0 = ("r", "l") if dx > 0 else ("l", "r")
            elif abs(dy) >= abs(dx):
                s0, e0 = ("b", "t") if dy < 0 else ("t", "b")
            else:
                s0, e0 = ("r", "l") if dx > 0 else ("l", "r")
            s = s or s0
            e = e or e0
        p1, p2 = A.side(s), B.side(e)
        pts = [p1]
        if via:
            for v in via:
                pts.append((self.px(v[0]), self.py(v[1])))
        else:
            vert = lambda q: q in ("t", "b")
            if abs(p1[0] - p2[0]) < 1.5 and vert(s) and vert(e):
                pass
            elif abs(p1[1] - p2[1]) < 1.5 and not vert(s) and not vert(e):
                pass
            elif vert(s) and vert(e):
                my = (p1[1] + p2[1]) / 2 + (mid or 0)
                pts += [(p1[0], my), (p2[0], my)]
            elif not vert(s) and not vert(e):
                mx = (p1[0] + p2[0]) / 2 + (mid or 0)
                pts += [(mx, p1[1]), (mx, p2[1])]
            elif vert(s) and not vert(e):
                pts += [(p1[0], p2[1])]
            else:
                pts += [(p2[0], p1[1])]
        pts.append(p2)
        self.arrow(pts, label, dashed, color, both, lpos, lside)

    def legend(self, col, row):
        items = [("proc", "step"), ("dec", "decision"), ("term", "start / thread"), ("ext", "outside / database / network"),
                 ("hw", "modem hardware"), ("buf", "queue / buffer")]
        x = self.px(col)
        y = self.py(row)
        c = self.c
        c.setFont(FONTB, 7.5)
        c.drawString(x, y + 4, "Key")
        for i, (k, t) in enumerate(items):
            n = Node("k", x + 12 + i * 118, y - 14, 20, 11, k, [], 6)
            self.drawnode(n)
            c.setFont(FONT, 7)
            c.drawString(x + 26 + i * 118, y - 17, t)

    def footer(self, text):
        c = self.c
        c.setFont(FONT, 7)
        c.setFillColor(HexColor("#666666"))
        for i, l in enumerate(text.split("\n")):
            c.drawString(40, 34 - i * 9, l)
        c.setFillColor(black)

    def newpage(self, title, subtitle=""):
        self.c.showPage()
        self.nodes = {}
        self.pages += 1
        self.header(title, subtitle)

    def save(self):
        self.c.showPage()
        self.c.save()
