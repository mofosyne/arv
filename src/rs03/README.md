# rs03: dvdisaster's RS03 error correction, as a small C library

Adds RS03 error correction to a disc image exactly as
`dvdisaster -i IMAGE -mRS03 -o image -c [-n SECTORS]` does, and tests an augmented image. The
output is **byte for byte** what [dvdisaster Light](https://github.com/teaching-droid/dvdisaster-light)
(and dvdisaster 0.79.10 / the speed47 fork) write, so any of them can test and repair it.

C99 and POSIX (threads), no libraries: about 600 lines, against dvdisaster's GLib-based code
base. GPLv3, like dvdisaster, whose format it follows (`rs03-create.c`, `rs03-common.c`,
`galois.c`, `rs-encoder.c`, `crc32.c`, `ds-marker.c`).

```sh
make                      # build/rs03
build/rs03 -n 12219392 disc.iso     # augment for a BD-R 25 GB without defect management
build/rs03 -t disc.iso    # test: header, every data sector's CRC, CRC sectors, parity
make check                # the checks below
```

| | |
|---|---|
| `rs03.h`, `rs03.c` | the library: `rs03_layout_for`, `rs03_augment`, `rs03_verify`, `rs03_describe` |
| `rs03_cli.c` | the program |
| `check.sh` | the checks |
| `upstream/` | a draft offer of this code to dvdisaster Light (not sent) |

## What it covers, and what it does not

- **Augmenting** (dvdisaster's `-c` for augmented images): the ecc header, padding sectors, the
  CRC layer and the parity layers, for a given medium size or the smallest standard medium.
  Separate `.ecc` files (RS03 file mode) are not written.
- **Testing** (dvdisaster's `-t`, for image files): finds the layout from the image itself,
  checks the header, compares every data sector with its CRC, every CRC sector's fields and own
  CRC, and every parity sector with the parity the data gives. It counts what is damaged.
- **Not here:** repair (`-f`) and reading damaged discs from a drive. dvdisaster Light does both
  well; every image this writes is one it can repair.

## How it is checked

`make check`:
- `-t` passes a whole image, and finds exactly the damage done to copies of it: three data
  sectors, sector 0, the ecc header, one CRC sector, one parity sector;
- with dvdisaster Light (or speed47) on PATH as `dvdisaster`: eight images (odd sizes, a chosen
  medium, the automatic one, the redundancy clip, a large one) augmented by both are the same
  byte for byte; each tool's test accepts the other's image; dvdisaster Light repairs the damaged
  copies and `rs03 -t` then finds them whole.

Speed: on four cores, 3.2 s for a 200 MB image on a 400 MB medium (dvdisaster Light: 4.1 s).
