#!/usr/bin/env python3
"""RS03 augmenting, written from docs/rs03-format.md alone, to show the spec is complete.

    python3 spec-check.py IMAGE MEDIUM-SECTORS OUT

reads IMAGE, writes the augmented image to OUT. Slow and simple on purpose: one codeword column
at a time would be clearer still, but this runs the shift register over whole layers (bytes
objects) so the checks finish in seconds. Section numbers refer to the spec.
"""
import hashlib
import sys
import zlib

SECTOR = 2048


# 4. Arithmetic: GF(2^8) with x^8 + x^7 + x^2 + x + 1 (0x187), alpha = x
EXP, LOG = [0] * 512, [0] * 256
v = 1
for i in range(255):
    EXP[i] = EXP[i + 255] = v
    LOG[v] = i
    v <<= 1
    if v & 0x100:
        v ^= 0x187


def mul(a, b):
    return 0 if a == 0 or b == 0 else EXP[LOG[a] + LOG[b]]


def crc(data):
    """4. CRC-32, reflected 0xEDB88320, start 0xFFFFFFFF, no final inversion"""
    return zlib.crc32(data) ^ 0xFFFFFFFF


def le(n, size):
    return n.to_bytes(size, "little")


def generator(nroots):
    """5. g(x) = product of (x - alpha^(11 * (112 + i))), i = 0 .. nroots-1; g[k] is x^k's coefficient"""
    g = [1]
    for i in range(nroots):
        r = EXP[(11 * (112 + i)) % 255]
        g = [(g[k - 1] if k > 0 else 0) ^ (mul(g[k], r) if k < len(g) else 0) for k in range(len(g) + 1)]
    return g


def augment(image, medium):
    data_sectors = -(-len(image) // SECTOR)
    in_last = len(image) - (data_sectors - 1) * SECTOR
    image = image + bytes(data_sectors * SECTOR - len(image))

    # 2. Layout
    spl = medium // 255
    ndata = max(84, -(-(data_sectors + 2) // spl)) + 1
    nroots = 255 - ndata
    first_crc = (ndata - 1) * spl
    total = 255 * spl
    fp = hashlib.md5(image[16 * SECTOR:17 * SECTOR]).digest()

    # 3.1 the ecc header, two sectors after the data
    h = bytearray(4096)
    h[0:16] = b"*dvdisaster*RS03"
    h[19] = 1
    h[20:36] = fp
    h[68:76] = le(data_sectors, 8)
    h[76:80] = le(ndata, 4)
    h[80:84] = le(nroots, 4)
    h[84:88] = le(7910, 4)
    h[88:92] = le(7900, 4)
    h[92:96] = le(16, 4)
    h[116:120] = le(in_last, 4)
    h[120:128] = le(spl, 8)
    h[96:100] = le(0x4C5047, 4)
    h[96:100] = le(crc(bytes(h)), 4)

    # 3.3 padding sectors up to the CRC layer
    out = bytearray(image) + h
    for s in range(data_sectors + 2, first_crc):
        p = bytearray(SECTOR)
        text = (b"dvdisaster padding sector       "
                b"This is a padding sector needed for augmenting the image with error correction data.")
        p[0:len(text)] = text
        end = b"dvdisaster padding sector end marker"
        p[2047 - len(end):2047] = end
        for at, field in ((0x100, b"Padding sector marker version"), (0x120, b"1.00"),
                          (0x140, b"Padding sector number"), (0x160, str(s).encode()),
                          (0x180, b"Medium fingerprint"), (0x1C0, b"Medium fingerprint sector"),
                          (0x1E0, b"16")):
            p[at:at + len(field)] = field
        p[0x1A0:0x1B0] = fp
        out += p

    def sector(layer, n):
        at = (layer * spl + n) * SECTOR
        return bytes(out[at:at + SECTOR])

    # 3.2 the CRC layer: CRC sector n holds the CRCs of position n+1 (the last one: position 0)
    for n in range(spl):
        b = bytearray(SECTOR)
        for layer in range(ndata - 1):
            b[4 * layer:4 * layer + 4] = le(crc(sector(layer, (n + 1) % spl)), 4)
        b[1024:1040] = b"*dvdisaster*RS03"
        b[1043] = 1
        b[1044:1048] = le(7910, 4)
        b[1048:1052] = le(7900, 4)
        b[1052:1056] = le(16, 4)
        b[1056:1072] = fp
        b[1088:1096] = le(data_sectors, 8)
        b[1096:1100] = le(in_last, 4)
        b[1100:1104] = le(ndata, 4)
        b[1104:1108] = le(nroots, 4)
        b[1112:1120] = le(spl, 8)
        b[1120:1124] = le(0x4C5047, 4)
        b[1120:1124] = le(crc(bytes(b)), 4)
        out += b

    # 5. the parity: per byte position, layers 0 .. ndata-1 are the message (layer 0 the highest
    # power); the parity is message * x^nroots mod g(x), layer ndata + k holding x^(nroots-1-k)'s
    # coefficient. A shift register over whole layers at once: reg[k] is a bytes object.
    g = generator(nroots)
    times = [bytes(mul(c, x) for x in range(256)) for c in g]
    width = spl * SECTOR
    reg = [bytes(width) for _ in range(nroots)]

    def xor(a, b):
        return (int.from_bytes(a, "little") ^ int.from_bytes(b, "little")).to_bytes(width, "little")

    for layer in range(ndata):
        fb = xor(bytes(out[layer * width:(layer + 1) * width]), reg[0])
        reg = [xor(reg[k + 1], fb.translate(times[nroots - 1 - k])) for k in range(nroots - 1)] \
            + [fb.translate(times[0])]
    for k in range(nroots):
        out += reg[k]
    assert len(out) == total * SECTOR
    return bytes(out)


if __name__ == "__main__":
    image, medium, target = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    with open(image, "rb") as f:
        data = f.read()
    with open(target, "wb") as f:
        f.write(augment(data, medium))
