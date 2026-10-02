#!/usr/bin/env python3
"""Make test files, or scan an image for non-zero bytes after each file's end.

  padding.py make DIR          40 files, sizes just over sector boundaries, unique content
  padding.py scan DIR IMAGE    for each file: find its (single, sector-aligned) copy in
                               IMAGE and count non-zero bytes up to the next 2048 boundary
Exit status of scan: 0 when every file's padding is zero, 1 otherwise.
"""
import hashlib
import os
import sys

SECTOR = 2048


def make(folder):
    os.makedirs(folder, exist_ok=True)
    for i in range(40):
        size = SECTOR * (1 + i % 5) + 1 + 37 * i          # > 1 sector, not a multiple of 2048
        stream = b"".join(hashlib.sha256(b"%d-%d" % (i, k)).digest() for k in range(size // 32 + 1))
        with open(os.path.join(folder, "f%02d.bin" % i), "wb") as f:
            f.write(stream[:size])


def scan(folder, image):
    with open(image, "rb") as f:
        d = f.read()
    bad = 0
    names = sorted(os.listdir(folder))
    for n in names:
        with open(os.path.join(folder, n), "rb") as f:
            data = f.read()
        pos = d.find(data)
        if pos < 0 or pos % SECTOR or d.find(data, pos + 1) >= 0:
            sys.exit("%s: expected exactly one sector-aligned copy in the image" % n)
        end = pos + len(data)
        pad = d[end:-(-end // SECTOR) * SECTOR]
        nonzero = sum(1 for b in pad if b)
        if nonzero:
            bad += 1
            print("  %s: %d bytes, %d of %d padding bytes non-zero, first: %s"
                  % (n, len(data), nonzero, len(pad), pad[:16].hex(" ")))
    print("%d of %d files have non-zero padding" % (bad, len(names)))
    return 1 if bad else 0


if __name__ == "__main__":
    if sys.argv[1:2] == ["make"]:
        make(sys.argv[2])
    elif sys.argv[1:2] == ["scan"]:
        sys.exit(scan(sys.argv[2], sys.argv[3]))
    else:
        sys.exit(__doc__)
