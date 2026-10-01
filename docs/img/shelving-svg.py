#!/usr/bin/env python3
"""Draws shelving.svg (the recommended shelf layout in docs/shelving.md). Run: python3 docs/img/shelving-svg.py

Physical order: access level, then the year the disc was made, then the order it was made.
The ids are real (computed with the tool's id rules); the years made are illustrative.
"""
import os
from xml.sax.saxutils import escape as e

W, H = 1240, 820
FONT = "font-family='DejaVu Sans, Helvetica, Arial, sans-serif'"
MONO = "font-family='DejaVu Sans Mono, Menlo, Consolas, monospace'"
COL = {"public": ("#dcefdc", "#2f6b34"), "private": ("#dbe8f8", "#2f5d8a"), "sealed": ("#e3e3e3", "#444444")}
out = []
add = out.append


def text(x, y, s, size=13, weight="normal", fill="#1f2328", anchor="start", mono=False):
    add("<text x='%s' y='%s' font-size='%s' font-weight='%s' fill='%s' text-anchor='%s' %s>%s</text>"
        % (x, y, size, weight, fill, anchor, MONO if mono else FONT, e(s)))


def spine(x, y, h, disc_id, title, access, copy=False):
    fill, ink = COL[access]
    add("<rect x='%d' y='%d' width='34' height='%d' rx='3' fill='%s' stroke='%s' stroke-width='1.2'/>" % (x, y, h, fill, ink))
    add("<circle cx='%d' cy='%d' r='5' fill='none' stroke='%s' stroke-width='1'/>" % (x + 17, y + h - 12, ink))
    cx, cy = x + 13, y + h - 24
    add("<text transform='translate(%d,%d) rotate(-90)' font-size='10.5' font-weight='bold' fill='%s' %s>%s</text>"
        % (cx, cy, ink, MONO, e(disc_id)))
    add("<text transform='translate(%d,%d) rotate(-90)' font-size='9' fill='%s' %s>%s</text>"
        % (cx + 12, cy, ink, FONT, e(title + (" (copy 2)" if copy else ""))))


def box(x, y, h, code, access, discs, copy=False):
    """A box of discs in the order made; returns its width."""
    fill, ink = COL[access]
    w = max(14 + 38 * len(discs) + 10, 14 + int(7.4 * len(code)) + 14)
    add("<rect x='%d' y='%d' width='%d' height='%d' rx='6' fill='#fbfbf8' stroke='%s' stroke-width='2'/>" % (x, y, w, h, ink))
    add("<rect x='%d' y='%d' width='%d' height='24' rx='6' fill='%s'/>" % (x, y - 12, int(7.4 * len(code)) + 20, ink))
    text(x + 10, y + 5, code, 12, "bold", "#ffffff", mono=True)
    for i, d in enumerate(discs):
        spine(x + 14 + 38 * i, y + 22, h - 34, d[0], d[1], access, copy)
    return w


def next_box(x, y, h, w, ink, label):
    add("<rect x='%d' y='%d' width='%d' height='%d' rx='6' fill='none' stroke='%s' stroke-width='1.5' stroke-dasharray='6 4'/>"
        % (x, y, w, h, ink))
    for i, line in enumerate(label):
        text(x + w / 2, y + h / 2 - 6 + 14 * i, line, 10.5, fill=ink, anchor="middle")


def row(x, y, h, boxes, gap=14):
    """Lays boxes left to right; returns the x after the last one."""
    for code, access, discs in boxes:
        starts.append(x)
        x += box(x, y, h, code, access, discs) + gap
    return x


starts = []


def badge(x, y, n):
    add("<circle cx='%d' cy='%d' r='11' fill='#c2410c'/>" % (x, y))
    text(x, y + 4.5, str(n), 12, "bold", "#ffffff", anchor="middle")


add("<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' viewBox='0 0 %d %d'>" % (W, H, W, H))
add("<rect width='100%' height='100%' fill='#ffffff'/>")
text(30, 38, "Recommended shelving: by access level, then the year the disc was made", 20, "bold")
text(30, 60, "New discs only ever go at the end. Kind (PHOTO, TRIP, SCAN ...) is virtual: the catalogue on every disc and search.html.",
     13, fill="#57606a")

# bookcase at HOME / Study
add("<rect x='24' y='84' width='870' height='560' rx='8' fill='none' stroke='#8c6d46' stroke-width='3'/>")
text(36, 104, "HOME  /  HOME-STUDY  (bookcase in the study)", 13, "bold", "#8c6d46")
for yy in (370, 640):
    add("<rect x='24' y='%d' width='870' height='8' fill='#b08d5f'/>" % yy)
text(880, 128, "PRIVATE", 12, "bold", COL["private"][1], anchor="end")
text(880, 398, "PUBLIC", 12, "bold", COL["public"][1], anchor="end")

x = row(44, 140, 228, [
    ("HOME-PRV-2020", "private", [("CODE-01_2012-2020_T", "Software 2012-20"), ("PHOTO-01_2015-2016_R", "Photos 2015-16"),
                                  ("PHOTO-02_2017_0", "Photos 2017"), ("VIDEO-01_2010-2020_1", "Home video")]),
    ("HOME-PRV-2021", "private", [("PHOTO-03_2018-2019_B", "Photos 2018-19"), ("FAMILY-01_2020-2021_K", "Family 2020-21")]),
    ("HOME-PRV-2023", "private", [("ELEC-01_2018-2022_K", "Electronics"), ("FAMILY-02_2022_K", "Family 2022"),
                                  ("PROJ-01_2020-2023_L", "Weather station")]),
    ("HOME-PRV-2026", "private", [("LETTERS-01_1990-1999_R", "Letters 1990s"), ("SCAN-01_1995-2008_D", "Letters scanned 1/3"),
                                  ("SCAN-02_1995-2008_B", "Letters scanned 2/3"), ("SCAN-03_1995-2008_9", "Letters scanned 3/3")]),
])
next_box(x, 140, 228, 70, COL["private"][1], ["next", "year's", "box"])
bx = starts[-1] + 14 + 38
add("<path d='M %d 376 v 8 h %d v -8' fill='none' stroke='%s' stroke-width='1.5'/>" % (bx, 38 * 3 - 4, COL["private"][1]))
text(bx + 55, 397, "made in 2026, covering 1995-2008", 10.5, fill=COL["private"][1], anchor="middle")

x = row(44, 410, 228, [
    ("HOME-PUB-2020", "public", [("TRIP-01_2019_4", "Kyoto July 2019")]),
    ("HOME-PUB-2023", "public", [("TRIP-02_202304_5", "Lisbon April 2023"), ("PROJ-02_2024_L", "Projects 2024")]),
])
next_box(x, 410, 228, 70, COL["public"][1], ["next", "year's", "box"])

# safe
add("<rect x='430' y='412' width='230' height='222' rx='10' fill='#f1f1f1' stroke='#444' stroke-width='3'/>")
add("<circle cx='642' cy='523' r='9' fill='none' stroke='#444' stroke-width='2'/>")
text(444, 432, "HOME-SAFE  (fire safe)", 12, "bold", "#444")
text(444, 448, "SEALED, by year made:", 11, fill="#444")
for i, (code, d) in enumerate([("2021", ("TAXES-01_2019-2020_I", "Tax 2019")), ("2024", ("LEGAL-01_2001-2024_K", "ID documents"))]):
    bx = 444 + 82 * i
    add("<rect x='%d' y='456' width='74' height='170' rx='4' fill='none' stroke='#444' stroke-dasharray='3 3'/>" % bx)
    text(bx + 6, 474, code, 10.5, "bold", "#444", mono=True)
    spine(bx + 36, 462, 158, d[0], d[1], "sealed")

# labels legend inside the bookcase
add("<rect x='690' y='412' width='190' height='222' rx='6' fill='#fffdf5' stroke='#d0c4a8'/>")
text(702, 434, "Labels", 13, "bold")
rows = [("Disc hub", ["the full id,", "solvent-free marker"]), ("Case spine", ["volume label: id + title"]),
        ("Box", ["its code, large; contents", "list in the lid"])]
yy = 458
for head, lines in rows:
    text(702, yy, head, 11.5, "bold")
    for line in lines:
        yy += 15
        text(702, yy, line, 11, fill="#57606a")
    yy += 22
text(702, yy - 4, "archive list --at BOX", 10, fill="#57606a", mono=True)

# off-site
add("<rect x='918' y='84' width='300' height='560' rx='8' fill='none' stroke='#6b7280' stroke-width='3' stroke-dasharray='8 5'/>")
text(930, 104, "PARENTS  (off-site)", 13, "bold", "#4b5563")
box(934, 140, 228, "PARENTS-PRV-2020", "private", [("CODE-01_2012-2020_T", "Software 2012-20"),
                                                   ("PHOTO-01_2015-2016_R", "Photos 2015-16"),
                                                   ("PHOTO-02_2017_0", "Photos 2017"),
                                                   ("VIDEO-01_2010-2020_1", "Home video")], copy=True)
box(934, 410, 228, "PARENTS-PRV-2026", "private", [("LETTERS-01_1990-1999_R", "Letters 1990s"),
                                                   ("SCAN-01_1995-2008_D", "Letters scanned 1/3"),
                                                   ("SCAN-02_1995-2008_B", "Letters scanned 2/3"),
                                                   ("SCAN-03_1995-2008_9", "Letters scanned 3/3")], copy=True)
text(1068, 386, "...  same split, burned from the same .iso", 10.5, fill="#4b5563", anchor="middle")

# callouts
for n, (bx, by) in enumerate([(870, 150), (130, 128), (300, 158), (524, 392), (420, 404), (928, 128)], 1):
    badge(bx, by, n)
notes = [
    "One section per access level: private and public on the shelf, sealed in the safe.",
    "Within a section, one box per year the disc was made (its Date field, not its coverage).",
    "Within a box, in the order made: a new disc always goes at the end of the newest box.",
    "Old material goes in the year it was archived; the id still shows what it covers (SCAN-..._1995-2008).",
    "Sealed discs (taxes, identity documents) go in the safe, also by year made.",
    "Second copies, burned from the same image, at another site, in the same split.",
]
for i, s in enumerate(notes):
    badge(42, 672 + 22 * i, i + 1)
    text(60, 676 + 22 * i, s, 12.5)
add("</svg>")
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "shelving.svg"), "w", encoding="utf-8") as f:
    f.write("\n".join(out) + "\n")
