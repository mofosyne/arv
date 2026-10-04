# RS03 augmented images: the format

**What every arv disc uses for error correction, written down so that it can be read, tested
and repaired by a program written from this page alone.** The format is dvdisaster's RS03
(dvdisaster 0.79.x, dvdisaster Light, the speed47 fork), as an *augmented image*: the error
correction is added to the disc image itself and fills the medium. This page describes it
independently of any code. arv's implementation is [`src/rs03`](../../src/rs03/);
[`dev-tools/rs03-spec-check.py`](../../dev-tools/rs03-spec-check.py) is a second one written from this page
only, and both give the test vectors in section 7, as dvdisaster Light does.

Only what an augmented image needs is described: not RS03 error correction *files* (`.ecc`), nor
the older RS01 and RS02 methods. Numbers are little-endian; sectors are 2048 bytes; sector `s`
is bytes `2048·s` to `2048·s + 2047` of the image.

## 1. The idea

The image is cut into **255 layers** of equal size. For each sector position `n` in a layer
and each byte position `b` in a sector, the byte `b` of sector `n` of every layer, 255 bytes
in all, forms one **Reed-Solomon codeword**. The first layers hold the data (the original
image, then a header and padding), one layer holds checksums of the data sectors, and the rest
hold parity. Since the 255 bytes of a codeword lie one layer apart, a scratch or a run of
unreadable sectors costs each codeword at most a symbol or two, and any `nroots` damaged layers
at a position can be rebuilt as long as it is known which ones they are.

## 2. Layout

Given the original image of `D` sectors (the last one may be partly filled; see `inLast`) and a
medium of `M` sectors:

| Value | Definition |
|---|---|
| `spl` | sectors per layer: `floor(M / 255)` |
| total size | `255 · spl` sectors; the medium's last `M mod 255` sectors stay unused |
| `ndata` | `max(84, ceil((D + 2) / spl)) + 1`: the data layers, plus the CRC layer |
| `nroots` | `255 − ndata`: the parity layers, and the code's correction capacity (at most 170, since `ndata` is at least 85) |
| redundancy | `nroots / ndata` |

An image can be augmented only if `nroots ≥ 8` (dvdisaster's rule) and `D > 16` (sector 16 is
fingerprinted, section 3).

Layer `L` (0 to 254) is sectors `L·spl` to `L·spl + spl − 1`. Sector `n` of layer `L` is image
sector `L·spl + n`.

| Sectors | Contents |
|---|---|
| `0` to `D − 1` | the original image; a partly filled last sector is completed with zero bytes |
| `D`, `D + 1` | the **ecc header** (section 3.1), 4096 bytes |
| `D + 2` to `(ndata − 1)·spl − 1` | **padding sectors** (section 3.3) |
| `(ndata − 1)·spl` to `ndata·spl − 1` | the **CRC layer** (layer `ndata − 1`, section 3.2) |
| `ndata·spl` to `255·spl − 1` | the **parity layers** `ndata` to `254` (section 5) |

When no medium size is given, dvdisaster (and arv) choose the smallest of these, in sectors,
that leaves at least 8 roots:

| Medium | Sectors | Without BD-R defect management |
|---|---|---|
| CD-R | 359 424 | |
| DVD single layer | 2 295 104 | |
| DVD double layer | 4 171 712 | |
| BD-R 25 GB | 11 826 176 | 12 219 392 |
| BD-R 50 GB | 23 652 352 | 24 438 784 |
| BDXL 100 GB (3 layers) | 47 305 728 | 48 878 592 |
| BDXL 128 GB (4 layers) | 60 403 712 | 62 500 864 |

(The sizes without defect management are offered only when asked for; with it, the drive keeps
spare areas and the disc holds less.)

## 3. The sectors RS03 adds

The **medium fingerprint** `FP` is the MD5 of image sector 16 (16 bytes).

### 3.1 The ecc header (sectors `D`, `D + 1`)

4096 bytes; all bytes not listed are zero.

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 12 | cookie | `*dvdisaster*` |
| 12 | 4 | method | `RS03` |
| 16 | 4 | methodFlags | `0, 0, 0, 1` (byte 3: as dvdisaster's builds write it) |
| 20 | 16 | mediumFP | `FP` |
| 36 | 16 | mediumSum | zero (an MD5 of the whole medium, not written) |
| 52 | 16 | eccSum | zero (for `.ecc` files) |
| 68 | 8 | sectors | `D` |
| 76 | 4 | dataBytes | `ndata` |
| 80 | 4 | eccBytes | `nroots` |
| 84 | 4 | creatorVersion | `7910` (dvdisaster 0.79.10, and Light) |
| 88 | 4 | neededVersion | `7900` |
| 92 | 4 | fpSector | `16` |
| 96 | 4 | selfCRC | the CRC (section 4) of all 4096 bytes, computed with this field holding `0x004C5047` |
| 100 | 16 | crcSum | zero (for RS02) |
| 116 | 4 | inLast | bytes used in the original image's last sector, 1 to 2048 |
| 120 | 8 | sectorsPerLayer | `spl` |
| 128 | 8 | sectorsAddedByEcc | zero |

Note that the header sits at sector `D`, which it records: a reader can confirm a header found
by searching by checking that its `sectors` field equals its own position.

### 3.2 The CRC layer (layer `ndata − 1`)

Its sector `n` (for `n` from 0 to `spl − 1`) is a **CRC block**, 2048 bytes; all bytes not
listed are zero:

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 · 256 | crc | `crc[L]`, for `L` from 0 to `ndata − 2`: the CRC (section 4) of sector `m` of data layer `L`, where `m = n + 1`, or `m = 0` for the last block (`n = spl − 1`). Unused entries are zero |
| 1024 | 12 | cookie | `*dvdisaster*` |
| 1036 | 4 | method | `RS03` |
| 1040 | 4 | methodFlags | `0, 0, 0, 1` |
| 1044 | 4 | creatorVersion | `7910` |
| 1048 | 4 | neededVersion | `7900` |
| 1052 | 4 | fpSector | `16` |
| 1056 | 16 | mediumFP | `FP` |
| 1072 | 16 | mediumSum | zero |
| 1088 | 8 | dataSectors | `D` |
| 1096 | 4 | inLast | as in the header |
| 1100 | 4 | dataBytes | `ndata` |
| 1104 | 4 | eccBytes | `nroots` |
| 1112 | 8 | sectorsPerLayer | `spl` |
| 1120 | 4 | selfCRC | the CRC of all 2048 bytes, computed with this field holding `0x004C5047` |

So the CRCs of the data sectors at position `n` are in the CRC block at position `n − 1` (for
position 0: the last CRC block), and every CRC block also repeats the layout, so that the layout
can be found from any of them (section 6).

### 3.3 Padding sectors

Each padding sector `s` (its image sector number) is 2048 bytes, all zero except for these
ASCII strings (no terminating zero needed beyond the zero bytes around them):

| Offset | Text |
|---|---|
| 0 | `dvdisaster padding sector       This is a padding sector needed for augmenting the image with error correction data.` (seven spaces after `sector`) |
| 0x100 | `Padding sector marker version` |
| 0x120 | `1.00` |
| 0x140 | `Padding sector number` |
| 0x160 | `s` in decimal |
| 0x180 | `Medium fingerprint` |
| 0x1A0 | `FP` (16 bytes, binary) |
| 0x1C0 | `Medium fingerprint sector` |
| 0x1E0 | `16` |
| 2011 | `dvdisaster padding sector end marker` (36 bytes: bytes 2011 to 2046; byte 2047 is zero) |

## 4. Arithmetic

**CRC.** The CRC-32 polynomial in its reflected form `0xEDB88320`, register starting at
`0xFFFFFFFF`, bytes fed least significant bit first, and **no final inversion**: that is,
the usual CRC-32 (zlib, Ethernet) with its result XORed with `0xFFFFFFFF`.

**The field.** GF(2^8) built on `x^8 + x^7 + x^2 + x + 1` (`0x187`), with `α = x` (the byte
`0x02`) as its primitive element: `α^0 = 1`, and each next power is the previous one shifted
left by a bit, XORed with `0x187` whenever bit 8 comes out set.

## 5. The parity

For each sector position `n` (0 to `spl − 1`) and byte position `b` (0 to 2047), let
`c_L` be byte `b` of sector `n` of layer `L`. The message is the data and CRC layers,
`c_0 … c_(ndata−1)`, and the codeword is the polynomial

    C(x) = c_0·x^254 + c_1·x^253 + … + c_254·x^0

(layer 0 is the highest power). It is a systematic Reed-Solomon code with the generator

    g(x) = (x − α^(11·112)) · (x − α^(11·113)) · … · (x − α^(11·(112 + nroots − 1)))

(exponents modulo 255; the code's first root is 112 and its primitive element 11, as in
CCSDS). The parity bytes are the remainder

    c_ndata·x^(nroots−1) + … + c_254·x^0  =  (c_0·x^(ndata−1) + … + c_(ndata−1)) · x^nroots  mod g(x)

so that `C(x)` is a multiple of `g(x)`: layer `ndata + k` holds the coefficient of
`x^(nroots − 1 − k)`. The parity protects the CRC layer exactly as it is stored.

## 6. Testing and repairing (how readers use it)

These are not part of the format; they are how dvdisaster and arv use it.

- **Finding the layout.** For an image of `T` sectors with `T` a multiple of 255, `spl = T / 255`;
  for each `ndata` from 85 to 254, the sector `(ndata − 1)·spl` (or the next few, in case it is
  damaged) is a CRC block with a valid selfCRC, its own `dataBytes = ndata` and
  `sectorsPerLayer = spl`. If the image is cut short or those blocks are damaged, look for the
  ecc header: a sector starting with `*dvdisaster*RS03`, a valid selfCRC, and `sectors` equal
  to its own position. Then recompute the layout (section 2) from `D` and `255·spl`, and check
  it gives the recorded `ndata`.
- **Testing.** Every data sector's CRC against its CRC block; every CRC block's selfCRC and
  fields; every parity sector against the parity the data and CRC layers give.
- **Finding damage.** A data sector whose CRC differs, a CRC block failing its selfCRC, a sector
  the reader could not read (dvdisaster writes a *dead sector marker*: it starts with
  `dvdisaster dead sector marker` and a newline, and holds `dvdisaster dead sector end marker`
  and a newline in bytes 2012 to 2045; other readers such as ddrescue leave zeros, so an all-zero
  CRC or parity sector is suspect), and sectors missing from the end. These are *erasures*: at
  each position, up to `nroots` of them can be rebuilt.
- **Repairing.** Per codeword, the usual errors-and-erasures decoding (syndromes at the roots
  of `g`, Berlekamp-Massey started from the erasure locator, Chien search, Forney's formula).
  Unknown errors cost two of the `nroots` each; known erasures cost one. Positions where the
  data CRCs, the CRC block and the parity all agree need no decoding.

## 7. Test vectors

The input images are generated: byte `i` of the image is `(31·i + floor(i / 2048)) mod 256`.

| Input | Medium | `spl` | `ndata` | `nroots` | SHA-256 of the augmented image |
|---|---|---|---|---|---|
| 100 sectors | 510 | 2 | 85 | 170 | `9d909de643504e2f1e17ba8fa6ed7d703d9b4e081b6e50a7acb416bf4b1188a3` |
| 400 sectors | 1020 | 4 | 102 | 153 | `549b58b1f3ea5dc3b1fd2ec844c3fa322a5d7d5f00d5f307d5cb30e464ed5d48` |
| 400 sectors and 777 bytes | 1020 | 4 | 102 | 153 | `6deb1dc9e4a9b5889c747893c0e179c17fd3e5c1d634a3d763f61c25c57c0d28` |

For the second, some values along the way: `FP` = `2e0fe105d3fbc3071da877f94068fb63`; the
header's selfCRC `0xd93a941d`; the first CRC block's `crc[0]` `0x3f4c4e8e` and selfCRC
`0x35fa8623`; the first eight bytes of the first parity layer `f2 45 97 70 e6 75 06 4a`.

dvdisaster Light 0.3.0 gives the first two byte for byte. It stops on the third ("Failed
reading sector 400"), although it augments other images with a partly filled last sector to
the same bytes as arv; arv's own images are always whole sectors. `src/rs03/check.sh` checks
all three against arv's encoder and this page's implementation, `spec-check.py`.

## Sources

dvdisaster's source (GPLv3): `rs03-create.c`, `rs03-common.c`, `rs03-fix.c`, `rs-encoder.c`,
`galois.c`, `crc32.c`, `ds-marker.c` and `dvdisaster.h` (the `EccHeader` and `CrcBlock`
structures), as in dvdisaster 0.79.10 and dvdisaster Light 0.3.0. This page is a description,
written for arv; the format is dvdisaster's.
