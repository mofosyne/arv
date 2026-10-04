# rs03: dvdisaster's RS03 error correction, as a small C library

Adds RS03 error correction to a disc image exactly as
`dvdisaster -i IMAGE -mRS03 -o image -c [-n SECTORS]` does, tests an augmented image, and
repairs a damaged one as `dvdisaster -f` does. The
output is **byte for byte** what [dvdisaster Light](https://github.com/teaching-droid/dvdisaster-light)
(and dvdisaster 0.79.10 / the speed47 fork) write, so any of them can test and repair it.

C99 and POSIX (threads), no libraries: about 950 lines, against dvdisaster's GLib-based code
base. GPLv3, like dvdisaster, whose format it follows (`rs03-create.c`, `rs03-common.c`,
`galois.c`, `rs-encoder.c`, `rs03-fix.c`, `crc32.c`, `ds-marker.c`).

```sh
make                      # build/rs03
build/rs03 -n 12219392 disc.iso     # augment for a BD-R 25 GB without defect management
build/rs03 -t disc.iso    # test: header, every data sector's CRC, CRC sectors, parity
build/rs03 -f disc.iso    # repair in place, then test
make check                # the checks below
```

| | |
|---|---|
| `rs03.h`, `rs03.c` | the library: `rs03_layout_for`, `rs03_augment`, `rs03_verify`, `rs03_repair`, `rs03_describe` |
| `rs03_cli.c` | the program |
| `check.sh` | the checks |

## What it covers, and what it does not

- **Augmenting** (dvdisaster's `-c` for augmented images): the ecc header, padding sectors, the
  CRC layer and the parity layers, for a given medium size or the smallest standard medium.
  Separate `.ecc` files (RS03 file mode) are not written.
- **Testing** (dvdisaster's `-t`, for image files): finds the layout from the image itself,
  checks the header, compares every data sector with its CRC, every CRC sector's fields and own
  CRC, and every parity sector with the parity the data gives. It counts what is damaged.
- **Repairing** (dvdisaster's `-f`, for augmented images): the layout comes from any of the first
  CRC sectors or, failing that, the ecc header found in the image, so a cut-short image or one
  with its first CRC sectors damaged can be repaired. Damaged sectors are known from the CRCs,
  dvdisaster's dead sector markers, zero-filled CRC or parity sectors (what ddrescue or dd leave
  for an unreadable sector), CRC sectors failing their own CRC, and a missing end; the decoder
  (errors and erasures, as dvdisaster's) finds the rest. Only positions whose parity disagrees
  are decoded at all, and those on every core. What is beyond the parity is reported, never
  guessed at.
- **Not here:** ecc files, and reading damaged discs from a drive: dvdisaster Light or GNU
  ddrescue read them into an image.

A draft offer of this code to dvdisaster Light, not sent, is in
[`upstream/dvdisaster-light/`](../../upstream/dvdisaster-light/offer.md).

## How it is checked

`make check`:
- `-t` passes a whole image, and finds exactly the damage done to copies of it: three data
  sectors, sector 0, the ecc header, one CRC sector, one parity sector;
- with dvdisaster Light (or speed47) on PATH as `dvdisaster`: eight images (odd sizes, a chosen
  medium, the automatic one, the redundancy clip, a large one) augmented by both are the same
  byte for byte; each tool's test accepts the other's image; dvdisaster Light repairs the damaged
  copies and `rs03 -t` then finds them whole; both repair the same scattered damage to the same
  image;
- `-f` brings back the very image from scattered bytes, zero-filled runs, a dead sector marker,
  CRC and parity damage, data damage whose CRCs are damaged too, damaged first CRC sectors and a
  missing end; with more damage than the parity can mend, it says so and exits 1.

Speed, on four cores: augmenting a 200 MB image on a 400 MB medium, 3.2 s (dvdisaster Light:
4.1 s); repairing it after about 5000 damaged sectors (a 6 MB scratch and 2000 scattered
bytes), 25 s (dvdisaster Light: 117 s), to the same bytes.
