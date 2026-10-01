#!/usr/bin/env python3
"""Draws shelving.svg (the recommended shelf layout in docs/shelving.md). Run: python3 docs/img/shelving-svg.py"""
from xml.sax.saxutils import escape as e

W, H = 1120, 800
FONT = "font-family='DejaVu Sans, Helvetica, Arial, sans-serif'"
MONO = "font-family='DejaVu Sans Mono, Menlo, Consolas, monospace'"
COL = {"MEMORIES": ("#dbe8f8", "#2f5d8a"), "RECORDS": ("#f8ead4", "#8a5a1f"),
       "PROJECTS": ("#dcefdc", "#2f6b34"), "SEALED": ("#e3e3e3", "#444444")}
out = []
add = out.append

def text(x, y, s, size=13, weight="normal", fill="#1f2328", anchor="start", mono=False, extra=""):
    add("<text x='%s' y='%s' font-size='%s' font-weight='%s' fill='%s' text-anchor='%s' %s %s>%s</text>"
        % (x, y, size, weight, fill, anchor, MONO if mono else FONT, extra, e(s)))

def spine(x, y, h, disc_id, title, group, copy=False):
    fill, ink = COL[group]
    add("<rect x='%d' y='%d' width='34' height='%d' rx='3' fill='%s' stroke='%s' stroke-width='1.2'/>" % (x, y, h, fill, ink))
    add("<circle cx='%d' cy='%d' r='5' fill='none' stroke='%s' stroke-width='1'/>" % (x + 17, y + h - 12, ink))
    cx, cy = x + 13, y + h - 24
    add("<text transform='translate(%d,%d) rotate(-90)' font-size='10.5' font-weight='bold' fill='%s' %s>%s</text>"
        % (cx, cy, ink, MONO, e(disc_id)))
    add("<text transform='translate(%d,%d) rotate(-90)' font-size='9' fill='%s' %s>%s</text>"
        % (cx + 12, cy, ink, FONT, e(title + (" (copy 2)" if copy else ""))))

def box(x, y, w, h, code, name, group, discs, gap=0, note=None):
    fill, ink = COL[group]
    add("<rect x='%d' y='%d' width='%d' height='%d' rx='6' fill='#fbfbf8' stroke='%s' stroke-width='2'/>" % (x, y, w, h, ink))
    add("<rect x='%d' y='%d' width='%d' height='24' rx='6' fill='%s'/>" % (x, y - 12, min(w, 12 + 7.4 * len(code + name) + 20), ink))
    text(x + 10, y + 5, code, 12, "bold", "#ffffff", mono=True)
    text(x + 10 + 7.4 * len(code) + 12, y + 5, name, 12, "normal", "#ffffff")
    sx = x + 14
    for d in discs:
        if d is None:
            add("<rect x='%d' y='%d' width='%d' height='%d' rx='3' fill='none' stroke='%s' stroke-width='1' stroke-dasharray='4 3'/>"
                % (sx, y + 22, gap, h - 34, ink))
            text(sx + gap / 2, y + 22 + (h - 34) / 2, "room", 10, fill=ink, anchor="middle")
            text(sx + gap / 2, y + 22 + (h - 34) / 2 + 13, "to grow", 10, fill=ink, anchor="middle")
            sx += gap + 6
            continue
        spine(sx, y + 22, h - 34, d[0], d[1], d[2] if len(d) > 2 else group, d[3] if len(d) > 3 else False)
        sx += 38
    if note:
        text(x + 14, y + h + 18, note, 11, fill=ink)

def badge(x, y, n):
    add("<circle cx='%d' cy='%d' r='11' fill='#c2410c'/>" % (x, y))
    text(x, y + 4.5, str(n), 12, "bold", "#ffffff", anchor="middle")

add("<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' viewBox='0 0 %d %d'>" % (W, H, W, H))
add("<rect width='100%' height='100%' fill='#ffffff'/>")
text(30, 38, "Recommended shelving: shelf order is id order", 20, "bold")
text(30, 60, "Locations are recorded down to the box; inside a box, a disc's place follows from its id.", 13, fill="#57606a")

# bookcase at HOME / Study
add("<rect x='24' y='84' width='760' height='560' rx='8' fill='none' stroke='#8c6d46' stroke-width='3'/>")
text(36, 104, "HOME  /  HOME-STUDY  (bookcase in the study)", 13, "bold", "#8c6d46")
for yy in (370, 640):
    add("<rect x='24' y='%d' width='760' height='8' fill='#b08d5f'/>" % yy)

box(44, 140, 430, 228, "HOME-B01", "MEMORIES", "MEMORIES", [
    ("FAMILY-01_2020-2021_K", "Family 2020-21"), ("FAMILY-02_2022_K", "Family 2022"),
    ("PHOTO-01_2015-2016_R", "Photos 2015-16"), ("PHOTO-02_2017_0", "Photos 2017"),
    ("PHOTO-03_2018-2019_B", "Photos 2018-19"), ("TRIP-01_2019_4", "Kyoto July 2019"),
    ("TRIP-02_202304_5", "Lisbon April 2023"), None, ("VIDEO-01_2010-2020_1", "Home video")], gap=44)
box(494, 140, 270, 228, "HOME-B02", "RECORDS", "RECORDS", [
    ("LETTERS-01_1990-1999_R", "Letters 1990s"), ("SCAN-01_1995-2008_D", "Letters scanned 1/3"),
    ("SCAN-02_1995-2008_B", "Letters scanned 2/3"), ("SCAN-03_1995-2008_9", "Letters scanned 3/3"), None], gap=58)
add("<path d='M 552 382 v 8 h 112 v -8' fill='none' stroke='#8a5a1f' stroke-width='1.5'/>")
text(608, 404, "split set: Bag-Count 1-3 of 3, kept together", 10.5, fill="#8a5a1f", anchor="middle")

box(44, 410, 270, 228, "HOME-B03", "PROJECTS", "PROJECTS", [
    ("CODE-01_2012-2020_T", "Software 2012-20"), ("ELEC-01_2018-2022_K", "Electronics"),
    ("PROJ-01_2020-2023_L", "Weather station"), ("PROJ-02_2024_L", "Projects 2024"), None], gap=56)

# safe
add("<rect x='420' y='412' width='170' height='222' rx='10' fill='#f1f1f1' stroke='#444' stroke-width='3'/>")
add("<circle cx='566' cy='523' r='9' fill='none' stroke='#444' stroke-width='2'/>")
text(434, 432, "HOME-SAFE", 12, "bold", "#444", mono=True)
text(434, 448, "Fire safe", 11, fill="#444")
spine(438, 458, 168, "TAXES-01_2019-2020_I", "Tax 2019 (sealed)", "SEALED")
spine(478, 458, 168, "LEGAL-01_2001-2024_K", "ID documents", "SEALED")

# labels legend inside the bookcase
add("<rect x='610' y='412' width='160' height='222' rx='6' fill='#fffdf5' stroke='#d0c4a8'/>")
text(622, 434, "Labels", 13, "bold")
rows = [("Disc hub", ["the full id,", "solvent-free marker"]), ("Case spine", ["volume label: id + title"]),
        ("Box", ["its code, large; contents", "list in the lid"])]
yy = 458
for head, lines in rows:
    text(622, yy, head, 11.5, "bold")
    for line in lines:
        yy += 15
        text(622, yy, line, 11, fill="#57606a")
    yy += 22
text(622, yy - 4, "archive list --at BOX", 10, fill="#57606a", mono=True)

# off-site
add("<rect x='808' y='84' width='290' height='560' rx='8' fill='none' stroke='#6b7280' stroke-width='3' stroke-dasharray='8 5'/>")
text(820, 104, "PARENTS  (off-site)", 13, "bold", "#4b5563")
box(826, 140, 254, 476, "PARENTS-B01", "copies", "MEMORIES", [], note=None)
copies = [("FAMILY-01_2020-2021_K", "Family 2020-21"), ("PHOTO-01_2015-2016_R", "Photos 2015-16"),
          ("PHOTO-02_2017_0", "Photos 2017"), ("PHOTO-03_2018-2019_B", "Photos 2018-19"),
          ("TRIP-01_2019_4", "Kyoto July 2019"), ("SCAN-01_1995-2008_D", "Letters scanned 1/3")]
for i, c in enumerate(copies):
    grp = "RECORDS" if c[0].startswith("SCAN") else "MEMORIES"
    spine(840 + 38 * i, 162, 210, c[0], c[1], grp, True)
for i, c in enumerate([("SCAN-02_1995-2008_B", "Letters scanned 2/3"), ("SCAN-03_1995-2008_9", "Letters scanned 3/3"),
                       ("CODE-01_2012-2020_T", "Software 2012-20"), ("PROJ-01_2020-2023_L", "Weather station")]):
    grp = "RECORDS" if c[0].startswith("SCAN") else "PROJECTS"
    spine(840 + 38 * i, 392, 210, c[0], c[1], grp, True)
text(953, 638, "same ids as at home, burned from the same .iso", 10.5, fill="#4b5563", anchor="middle")

# callouts
for n, (x, y) in enumerate([(36, 128), (232, 128), (210, 158), (340, 158), (412, 404), (818, 128)], 1):
    badge(x, y, n)
notes = [
    "One box (or shelf section) per top-level group of the vocabulary, in its Order: MEMORIES, RECORDS, ... PROJECTS.",
    "Within a group, by set code (FAMILY, PHOTO, TRIP, VIDEO), alphabetically or in vocabulary order; pick one.",
    "Within a set, by sequence number. Each disc has one home: its Set (categories, tags, collections are for finding).",
    "Leave room at the end of each set: sequence numbers only grow.",
    "Sealed discs (taxes, identity documents) go in the safe.",
    "Second copies, burned from the same image, at another site. The order there matters less: locations are per box.",
]
for i, s in enumerate(notes):
    badge(42, 672 + 20 * i, i + 1)
    text(60, 676 + 20 * i, s, 12)
add("</svg>")
open(__import__("os").path.join(__import__("os").path.dirname(__import__("os").path.abspath(__file__)), "shelving.svg"), "w").write("\n".join(out))
