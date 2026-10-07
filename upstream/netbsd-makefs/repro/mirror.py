#!/usr/bin/env python3
"""The metadata mirror of a UDF 2.50 image (patches/04-metadata-mirror-readonly.patch):

    mirror.py check IMAGE             the mirror file has its own copy of the metadata, byte for
                                      byte, and the partition map says so (METADATA_DUPLICATED)
    mirror.py only-mirror IMAGE OUT   OUT: IMAGE with the main metadata zeroed and the main
                                      metadata file entry pointed at the mirror's extent, so a
                                      reader that never looks at the mirror reads the mirror anyway
    mirror.py damaged IMAGE OUT       OUT: IMAGE with the main metadata file entry and the main
                                      metadata zeroed: a reader that uses the mirror (the Linux
                                      kernel: mount -o loop,ro -t udf OUT DIR) still reads it

Readers here (7-Zip) don't fall back to the mirror on their own; only-mirror lets them show that
the mirror alone is a complete metadata partition. Standard library only.
"""

import struct
import sys

S = 2048


def sector(img, n):
    return img[n * S:(n + 1) * S]


def layout(img):
    """(partition start, the metadata partition map's offset in the image) from the main VDS"""
    length, loc = struct.unpack_from("<II", sector(img, 256), 16)
    start = meta_map = None
    for n in range(loc, loc + length // S):
        d = sector(img, n)
        tag = struct.unpack_from("<H", d, 0)[0]
        if tag == 5:                                    # partition descriptor
            start = struct.unpack_from("<I", d, 188)[0]
        elif tag == 6:                                  # logical volume descriptor
            count, pos = struct.unpack_from("<I", d, 268)[0], 440
            for _ in range(count):
                kind, size = d[pos], d[pos + 1]
                if kind == 2 and d[pos + 5:pos + 28] == b"*UDF Metadata Partition":
                    meta_map = n * S + pos
                pos += size
        elif tag == 8:                                  # terminator
            break
    if start is None or meta_map is None:
        sys.exit("not a UDF image with a metadata partition")
    return start, meta_map


def extents(img, start, lbn):
    """the short_ad extents (partition block, bytes) of the file entry at partition block lbn"""
    fe = sector(img, start + lbn)
    l_ea, l_ad = struct.unpack_from("<II", fe, 208)
    out = []
    for off in range(216 + l_ea, 216 + l_ea + l_ad, 8):
        length, pos = struct.unpack_from("<II", fe, off)
        out.append((pos, length & 0x3FFFFFFF))
    return out, 216 + l_ea


def content(img, start, ext):
    return b"".join(img[(start + p) * S:(start + p) * S + n] for p, n in ext)


def crc_itu(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def retag(buf, off):
    """recompute the descriptor tag at buf[off] (CRC, then checksum)"""
    crc_len = struct.unpack_from("<H", buf, off + 10)[0]
    struct.pack_into("<H", buf, off + 8, crc_itu(bytes(buf[off + 16:off + 16 + crc_len])))
    buf[off + 4] = 0
    buf[off + 4] = sum(buf[off:off + 16]) & 0xFF


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("check", "only-mirror", "damaged"):
        sys.exit(__doc__)
    img = open(sys.argv[2], "rb").read()
    start, m = layout(img)
    main_fe, mirror_fe, flags = struct.unpack_from("<I", img, m + 40)[0], struct.unpack_from("<I", img, m + 44)[0], img[m + 58]
    main_ext, _ = extents(img, start, main_fe)
    mirror_ext, _ = extents(img, start, mirror_fe)
    if sys.argv[1] == "check":
        blocks = lambda ext: {p + i for p, n in ext for i in range(n // S)}
        same = content(img, start, main_ext) == content(img, start, mirror_ext)
        apart = not blocks(main_ext) & blocks(mirror_ext)
        print("metadata %s, mirror %s, METADATA_DUPLICATED %s: %s" % (
            main_ext, mirror_ext, "set" if flags & 1 else "clear",
            "a real copy" if flags & 1 and same and apart else
            "the same extent" if not apart else "DIFFERENT CONTENT" if not same else "flag clear"))
        sys.exit(0 if flags & 1 and same and apart else 1)
    out = bytearray(img)
    for p, n in main_ext:                               # the main copy is gone ...
        out[(start + p) * S:(start + p) * S + n] = bytes(n)
    fe_off = (start + main_fe) * S
    if sys.argv[1] == "damaged":                        # ... and so is its file entry
        out[fe_off:fe_off + S] = bytes(S)
        open(sys.argv[3], "wb").write(out)
        print("wrote %s: main metadata file entry and %s zeroed" % (sys.argv[3], main_ext))
        return
    # ... and its entry now reads the mirror's
    _, ad = extents(img, start, main_fe)
    if len(main_ext) != len(mirror_ext):
        sys.exit("the two files have different numbers of extents")
    for i, (p, n) in enumerate(mirror_ext):
        length = struct.unpack_from("<I", out, fe_off + ad + 8 * i)[0]
        struct.pack_into("<II", out, fe_off + ad + 8 * i, (length & 0xC0000000) | n, p)
    retag(out, fe_off)
    open(sys.argv[3], "wb").write(out)
    print("wrote %s: main metadata zeroed, its file entry reads %s" % (sys.argv[3], mirror_ext))


if __name__ == "__main__":
    main()
