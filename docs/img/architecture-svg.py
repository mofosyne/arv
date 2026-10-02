#!/usr/bin/env python3
"""Draws architecture.svg (the four layers in docs/architecture.md). Run: python3 docs/img/architecture-svg.py

Centre: what is on every disc, layer by layer. Left: making a disc, top to bottom.
Right: reading and repairing one, bottom to top. The heavy line is the boundary: nothing
above it depends on anything below it; the Binding record is the only bridge.
"""
import os
from xml.sax.saxutils import escape as e

W, H = 1240, 1030
FONT = "font-family='DejaVu Sans, Helvetica, Arial, sans-serif'"
MONO = "font-family='DejaVu Sans Mono, Menlo, Consolas, monospace'"
INK = "#1f2328"
MUTED = "#57606a"
# (fill, ink) per layer: the archive above the boundary in cool colours, the medium below in warm
COL = {
    "content": ("#e3f1e3", "#2f6b34"),
    "description": ("#dfeaf8", "#2f5d8a"),
    "container": ("#f8ead8", "#8a5a1f"),
    "protection": ("#f8e0dc", "#8a3a2f"),
    "medium": ("#ececec", "#444444"),
}
X0, X1 = 284, 956            # the layer bands
LX, LW = 20, 236             # left column: making
RX, RW = 984, 236            # right column: reading
out = []
add = out.append


def text(x, y, s, size=12, weight="normal", fill=INK, anchor="start", mono=False, italic=False):
    add("<text x='%s' y='%s' font-size='%s' font-weight='%s' fill='%s' text-anchor='%s'%s %s>%s</text>"
        % (x, y, size, weight, fill, anchor, " font-style='italic'" if italic else "", MONO if mono else FONT, e(s)))


def rect(x, y, w, h, fill, stroke, rx=6, width=1.5, dash=None):
    add("<rect x='%s' y='%s' width='%s' height='%s' rx='%s' fill='%s' stroke='%s' stroke-width='%s'%s/>"
        % (x, y, w, h, rx, fill, stroke, width, " stroke-dasharray='%s'" % dash if dash else ""))


def band(y, h, key, number, name, rows, note):
    fill, ink = COL[key]
    rect(X0, y, X1 - X0, h, fill, ink)
    text(X0 + 14, y + 22, "%s  %s" % (number, name), 14, "bold", ink)
    text(X1 - 12, y + 22, note, 11, fill=ink, anchor="end", italic=True)
    for i, (left, right) in enumerate(rows):
        text(X0 + 14, y + 46 + 18 * i, left, 11.5, "bold", INK, mono=True)
        text(X0 + 238, y + 46 + 18 * i, right, 11.5, fill=INK)


def chips(x, y, label, items, ink):
    text(x, y + 14, label, 11, fill=ink, italic=True)
    x += 7 * len(label) + 10
    for item in items:
        w = 7.2 * len(item) + 16
        rect(x, y, w, 20, "#ffffff", ink, rx=10, width=1, dash="4 3")
        text(x + w / 2, y + 14, item, 11, fill=ink, anchor="middle")
        x += w + 8


def step(x, y, w, key, lines):
    fill, ink = COL[key]
    h = 12 + 16 * len(lines)
    rect(x, y, w, h, "#ffffff", ink, width=1.2)
    for i, line in enumerate(lines):
        text(x + 10, y + 19 + 16 * i, line, 11, "bold" if i == 0 else "normal", ink if i == 0 else INK,
             mono=i > 0 and line.startswith("$"))
    return y + h


def arrow(x, y0, y1, colour=MUTED):
    """A vertical arrow from y0 to y1 (pointing towards y1)."""
    add("<line x1='%s' y1='%s' x2='%s' y2='%s' stroke='%s' stroke-width='3'/>" % (x, y0, x, y1, colour))
    d = 9 if y1 > y0 else -9
    add("<path d='M%s,%s L%s,%s L%s,%s Z' fill='%s'/>" % (x - 7, y1 - d, x + 7, y1 - d, x, y1 + d * 0.2, colour))


add("<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' viewBox='0 0 %d %d'>" % (W, H, W, H))
add("<rect width='100%' height='100%' fill='#ffffff'/>")
text(W / 2, 36, "Four layers, one boundary", 22, "bold", anchor="middle")
text(W / 2, 60, "What an archive disc is made of, how it is made, and how it is read back without this tool",
     13, fill=MUTED, anchor="middle")
text(LX, 100, "Making a disc (arv make)", 13, "bold")
text((X0 + X1) / 2, 100, "On every disc", 13, "bold", anchor="middle")
text(RX + RW, 100, "Reading and repairing it", 13, "bold", anchor="end")

# ---------------------------------------------------------------- layers
Y = {"content": (114, 110), "description": (234, 176), "container": (520, 104),
     "protection": (634, 132), "medium": (776, 144)}

band(*Y["content"], "content", "1", "CONTENT", [
    ("data/", "your files, untouched: never moved, renamed or packed"),
    ("manifest-sha256.txt", "a hash per file (sha256sum -c works); sha512 too"),
    ("bagit.txt, bag-info.txt", "BagIt (RFC 8493), a package archives already take"),
], "independent of the medium")

band(*Y["description"], "description", "2", "DESCRIPTION", [
    ("catalog.rec", "entry point: this Disc, its Binding, its Events"),
    ("catalog/archive.rec", "the whole archive at burn time, by access level"),
    ("catalog/volumes/<id>/", "every disc's listing, manifest, tags, formats"),
    ("README.txt, index.html", "how to check, repair and search; a viewer"),
    ("tools/", "the source of the tools that made this disc"),
    ("tagmanifest-sha256.txt", "hashes of everything above except data/"),
], "independent of the medium")
text(X0 + 14, Y["description"][0] + Y["description"][1] - 10,
     "Plain text: recfiles and TSV. At home the same catalog/ lives in a .arv folder beside your files.",
     11, fill=COL["description"][1], italic=True)

# the boundary, with the Binding record as the only bridge
by = 470
add("<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='%s' stroke-width='4'/>" % (X0, by, X1, by, INK))
text(X0 + 4, by - 26, "Above: the archive. It never depends", 12, "bold")
text(X0 + 4, by - 10, "on anything below.", 12, "bold")
text(X0 + 4, by + 22, "Below: how one volume is stored. New medium,", 12, "bold")
text(X0 + 4, by + 38, "new binding; nothing above changes.", 12, "bold")
bx, bw = 640, 316
rect(bx, by - 40, bw, 80, "#fff8e1", INK, width=2)
text(bx + bw / 2, by - 20, "Binding record (in the catalogue)", 13, "bold", anchor="middle")
text(bx + bw / 2, by + 0, "Volume  Container  Protection", 11.5, anchor="middle", mono=True)
text(bx + bw / 2, by + 16, "Media  Filesystem  Ecc  MediumSectors", 11.5, anchor="middle", mono=True)
text(bx + bw / 2, by + 33, "the only place the medium is named", 11, fill=MUTED, anchor="middle", italic=True)

band(*Y["container"], "container", "3", "CONTAINER", [
    ("hybrid (default)", "ISO 9660 + Rock Ridge + Joliet, UDF 1.02 bridge"),
    ("udf250", "UDF 2.50, Blu-ray layout (src/udfmake)"),
], "depends on the medium")
chips(X0 + 14, Y["container"][0] + 76, "other bindings later:", ["LTFS", "exFAT", "tar", "Piql AFS"],
      COL["container"][1])

y0, h0 = Y["protection"]
band(y0, h0, "protection", "4", "PROTECTION", [], "depends on the medium")
text(X0 + 14, y0 + 44, "RS03 augmented image (dvdisaster Light): Reed-Solomon over the whole image,", 11.5)
text(X0 + 14, y0 + 60, "filesystem included, sized to fill the medium (at least 20% redundancy).", 11.5)
segments = [("filesystem: files + catalogue", 0.60, "#ffffff"), ("", 0.04, "#f3f3f3"), ("H", 0.025, "#f2c9c2"),
            ("CRC", 0.055, "#eab3aa"), ("RS03 error correction", 0.28, "#dd9184")]
sx, sw = X0 + 14, X1 - X0 - 28
for label, frac, fill in segments:
    w = sw * frac
    rect(round(sx, 1), y0 + 72, round(w, 1), 26, fill, COL["protection"][1], rx=0, width=1)
    if label:
        text(sx + w / 2, y0 + 89, label, 10.5, anchor="middle", fill=COL["protection"][1])
    sx += w
text(X0 + 14, y0 + 118, "Padding, header (H), CRC layer and ECC follow the filesystem; "
     "a reader that knows nothing of RS03 just sees the filesystem.", 10.5, fill=MUTED, italic=True)

y0, h0 = Y["medium"]
band(y0, h0, "medium", "5", "MEDIUM", [], "today: Blu-ray")
for i, (cx, where) in enumerate(((X0 + 60, "copy 1: home"), (X0 + 250, "copy 2: off-site"))):
    cy = y0 + 68
    add("<circle cx='%d' cy='%d' r='30' fill='#f6f6f6' stroke='#444' stroke-width='1.5'/>" % (cx, cy))
    add("<circle cx='%d' cy='%d' r='6' fill='#ffffff' stroke='#444' stroke-width='1.2'/>" % (cx, cy))
    text(cx + 40, cy + 4, where, 11.5, "bold")
text(X0 + 440, y0 + 52, "M-DISC BD-R, 25 or 100 GB", 12, "bold")
text(X0 + 440, y0 + 70, "identical copies of one image;", 11.5)
text(X0 + 440, y0 + 86, "copies can rebuild each other", 11.5)
chips(X0 + 14, y0 + 112, "other media later:", ["LTO tape", "hard drives", "film (Piql)"], COL["medium"][1])

# ---------------------------------------------------------------- making (left, downwards)
arrow(LX + LW + 14, 114, Y["medium"][0] + Y["medium"][1] - 6)
step(LX, 116, LW, "content", ["1 scan and hash", "names checked for each image type"])
step(LX, 170, LW, "content", ["2 bag it (BagIt)", "the folder itself is never changed"])
step(LX, 238, LW, "description", ["3 describe", "set, title, tags, access level"])
step(LX, 292, LW, "description", ["4 snapshot the catalogue", "every disc so far, minus sealed",
                                  "detail; add tools/ and README"])
step(LX, 524, LW, "container", ["5 build the image", "genisoimage, or udfmake"])
step(LX, 638, LW, "protection", ["6 add RS03", "dvdisaster Light"])
step(LX, 692, LW, "protection", ["7 verify the image", "dvdisaster -t"])
step(LX, 780, LW, "medium", ["8 burn two or more copies", "keep them apart; arv burned"])

# ---------------------------------------------------------------- reading (right, upwards)
arrow(RX - 14, Y["medium"][0] + Y["medium"][1] - 6, 114)
step(RX, 780, RW, "medium", ["1 read the disc to an image", "dvdisaster -r; read copy B into",
                             "copy A's image to fill its gaps"])
step(RX, 638, RW, "protection", ["2 repair the image", "dvdisaster -f"])
step(RX, 524, RW, "container", ["3 open it", "mount, or 7-Zip: no tool of ours"])
step(RX, 238, RW, "description", ["4 understand and search", "README.txt, index.html;",
                                  "arv find, or grep catalog/"])
step(RX, 116, RW, "content", ["5 check every file", "sha256sum -c, or bagit.py"])

# ---------------------------------------------------------------- footer
text(W / 2, 960, "Every disc stands alone: any one disc, on any computer, holds its files, the whole catalogue,",
     13, anchor="middle")
text(W / 2, 980, "the source of its tools and the steps to repair it.", 13, anchor="middle")
text(W / 2, 1012, "Drawn by docs/img/architecture-svg.py", 10.5, fill=MUTED, anchor="middle")
add("</svg>")

with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "architecture.svg"), "w", encoding="utf-8") as f:
    f.write("\n".join(out) + "\n")
