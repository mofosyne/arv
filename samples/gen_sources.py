#!/usr/bin/env python3
"""Generate the sample source folders (deterministic, standard library only).

    gen_sources.py DEST

Makes small but real files: PNG photos and scans, PDF documents, a C project
with a KiCad board and a git repository, text notes. File dates are set so the
coverage of each folder is meaningful.
"""

import datetime
import os
import random
import struct
import subprocess
import sys
import zlib


def png(path, width, height, pixel, when):
    """Write an RGB PNG; pixel(x, y) -> (r, g, b)."""
    raw = b"".join(b"\0" + bytes(c for x in range(width) for c in pixel(x, y)) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    put(path, data, when)


def pdf(path, lines, when):
    """Write a one-page PDF showing ``lines`` of text."""
    text = "BT /F1 12 Tf 72 720 Td 16 TL " + " ".join(
        "(%s) Tj T*" % l.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)") for l in lines) + " ET"
    objs = [b"<< /Type /Catalog /Pages 2 0 R >>",
            b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
            b"/Resources << /Font << /F1 5 0 R >> >> >>",
            b"<< /Length %d >>\nstream\n%s\nendstream" % (len(text), text.encode("latin-1")),
            b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"]
    out, offsets = bytearray(b"%PDF-1.4\n"), []
    for n, body in enumerate(objs, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n%s\nendobj\n" % (n, body)
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    out += b"".join(b"%010d 00000 n \n" % o for o in offsets)
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    put(path, bytes(out), when)


def put(path, data, when):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w", **({} if isinstance(data, bytes) else {"encoding": "utf-8"})) as f:
        f.write(data)
    ts = datetime.datetime(*when).timestamp()
    os.utime(path, (ts, ts))


def photo(rng, kind):
    """A small synthetic 'photo': sky and ground, a sun or a temple-ish shape."""
    sky = [rng.randint(90, 160), rng.randint(140, 200), rng.randint(200, 255)]
    ground = [rng.randint(40, 120), rng.randint(90, 160), rng.randint(30, 90)]
    horizon, sx, sy = rng.randint(55, 85), rng.randint(20, 140), rng.randint(10, 40)

    def pixel(x, y):
        if kind == "temple" and 50 < x < 110 and horizon - 40 < y < horizon:
            return (170, 40, 30) if (y - horizon + 40) % 12 < 3 or x in (58, 102) else (120, 30, 25)
        if (x - sx) ** 2 + (y - sy) ** 2 < 90:
            return (255, 230, 120)
        base = sky if y < horizon else ground
        shade = (y * 37 + x * 11) % 9
        return tuple(max(0, min(255, c + shade - 4)) for c in base)
    return pixel


def main(dest):
    rng = random.Random(20260930)

    # 1. Trip to Kyoto, July 2019
    trip = os.path.join(dest, "2019-07_Kyoto_trip")
    for day, places in ((1, ["Fushimi Inari", "Gion"]), (2, ["Kinkaku-ji", "Arashiyama"]), (3, ["Kiyomizu-dera"])):
        for n, place in enumerate(places):
            for k in range(3):
                png(os.path.join(trip, "day%d %s" % (day, place), "IMG_%04d.png" % (day * 100 + n * 10 + k)),
                    160, 120, photo(rng, "temple" if "ji" in place or "dera" in place or "Inari" in place else "view"),
                    (2019, 7, 13 + day, 9 + k, n * 7))
    put(os.path.join(trip, "notes.txt"),
        "Kyoto, 14-16 July 2019\n\nDay 1: Fushimi Inari early, then Gion in the evening.\n"
        "Day 2: Kinkaku-ji, bamboo grove at Arashiyama.\nDay 3: Kiyomizu-dera.\n", (2019, 7, 17, 20, 0))

    # 2. Weather station project, 2020-2023 (code, electronics, a git repository)
    proj = os.path.join(dest, "Projects_weather_station")
    put(os.path.join(proj, "README.md"), "# Weather station\n\nESP32 board with a BME280 sensor, logging to SD.\n"
        "Firmware in `firmware/`, KiCad board in `hardware/`.\n", (2020, 3, 2, 10, 0))
    put(os.path.join(proj, "firmware", "main.c"), '#include "sensor.h"\n\nint main(void)\n{\n\tsensor_init();\n'
        '\tfor (;;)\n\t\tlog_reading(sensor_read());\n}\n', (2021, 5, 1, 12, 0))
    put(os.path.join(proj, "firmware", "sensor.h"), "struct reading { float t, p, h; };\nvoid sensor_init(void);\n"
        "struct reading sensor_read(void);\nvoid log_reading(struct reading);\n", (2021, 5, 1, 12, 0))
    put(os.path.join(proj, "firmware", "Makefile"), "CFLAGS = -O2 -Wall\nfirmware: main.o\n\t$(CC) -o $@ $^\n",
        (2021, 5, 1, 12, 0))
    put(os.path.join(proj, "hardware", "station.kicad_pcb"), '(kicad_pcb (version 20221018) (generator pcbnew)\n'
        '  (general (thickness 1.6))\n  (layers (0 "F.Cu" signal) (31 "B.Cu" signal))\n)\n', (2022, 8, 20, 18, 0))
    put(os.path.join(proj, "hardware", "station.kicad_sch"), '(kicad_sch (version 20230121) (generator eeschema)\n'
        '  (paper "A4")\n)\n', (2022, 8, 20, 18, 0))
    for layer in ("F_Cu", "B_Cu", "F_Mask", "Edge_Cuts"):
        put(os.path.join(proj, "hardware", "gerbers", "station-%s.gbr" % layer),
            "%%FSLAX46Y46*%%\n%%MOMM*%%\n%%TF.FileFunction,%s*%%\nM02*\n" % layer, (2023, 1, 9, 9, 0))
    png(os.path.join(proj, "photos", "assembled.png"), 160, 120, photo(rng, "view"), (2023, 2, 11, 15, 0))
    # what git clones have: an executable script and symbolic links (docs/spec/smart-archive-format.md, "Links")
    put(os.path.join(proj, "firmware", "flash.sh"), "#!/bin/sh\n# flash the firmware over USB\n"
        "esptool.py write_flash 0x10000 firmware.bin\n", (2021, 5, 1, 12, 0))
    os.chmod(os.path.join(proj, "firmware", "flash.sh"), 0o755)
    ts = datetime.datetime(2023, 1, 9, 9, 0).timestamp()
    for target, link in (("../README.md", "hardware/README.md"),   # a file link: copied
                         ("hardware/gerbers", "gerbers")):          # a folder link: noted in the listing
        os.symlink(target, os.path.join(proj, link))
        os.utime(os.path.join(proj, link), (ts, ts), follow_symlinks=False)
    if subprocess.run(["git", "--version"], capture_output=True).returncode == 0:
        env = dict(os.environ, GIT_AUTHOR_NAME="Sample", GIT_AUTHOR_EMAIL="sample@example.invalid",
                   GIT_COMMITTER_NAME="Sample", GIT_COMMITTER_EMAIL="sample@example.invalid",
                   GIT_AUTHOR_DATE="2023-02-11T15:00:00Z", GIT_COMMITTER_DATE="2023-02-11T15:00:00Z")
        for cmd in (["init", "-q", "-b", "main"], ["add", "-A"], ["commit", "-q", "-m", "Weather station, board rev B"]):
            subprocess.run(["git", "-C", proj] + cmd, env=env, check=True, capture_output=True)
        ts = datetime.datetime(2023, 2, 11, 15, 0).timestamp()   # the repository is as old as its commit
        for root, _, names in os.walk(os.path.join(proj, ".git")):
            for name in names:
                os.utime(os.path.join(root, name), (ts, ts))

    # 3. Taxes 2019 (PDFs)
    taxes = os.path.join(dest, "Taxes_2019")
    pdf(os.path.join(taxes, "tax-return-2019.pdf"), ["Tax return 2019 (sample document)",
        "Income: 0.00", "This is generated sample data."], (2020, 4, 30, 10, 0))
    for m in range(1, 13, 3):
        pdf(os.path.join(taxes, "receipts", "receipts-2019-%02d.pdf" % m),
            ["Receipts, quarter starting %02d/2019 (sample)" % m], (2019, m + 2, 28, 12, 0))

    # 4. Scanned letters, 1995-2008: big enough to need several sample discs (about four, since
    # tools/ takes most of each tiny disc)
    scans = os.path.join(dest, "Scans_letters")
    for year in range(1995, 2009):
        for n in range(4):
            noise = random.Random(year * 10 + n)
            png(os.path.join(scans, str(year), "letter-%d-%d.png" % (year, n + 1)), 180, 240,
                lambda x, y, r=noise: (lambda g: (g, g, g - 10 if g > 10 else 0))(
                    235 - (60 if (y % 11 < 2 and 18 < x < 162 and y > 24) else 0) - r.randint(0, 25)),
                (year, 6, 1, 12, 0))

    # 5. Family photos 2020-2021
    family = os.path.join(dest, "Family_photos_2020-2021")
    for year, events in ((2020, ["Birthday", "Garden"]), (2021, ["Beach", "Christmas"])):
        for n, event in enumerate(events):
            for k in range(4):
                png(os.path.join(family, "%d %s" % (year, event), "PXL_%d%02d%02d.png" % (year, n + 3, k + 1)),
                    160, 120, photo(rng, "view"), (year, 3 + n * 6, 10 + k, 14, 0))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "sample-sources")
