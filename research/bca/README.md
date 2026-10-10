# The BD-R BCA: telling copies of one image apart

Every copy of a disc is burned from the same image, so the copies are sector-identical: that is
what lets two damaged copies repair each other. It also means nothing in the data tells copy A
from copy B. arv names each copy by a letter written on its hub (`Copy: A`), and this note is
about the one thing a drive can read that *does* differ between copies: the **BCA serial**.

## What the BCA is

The burst cutting area is a ring near the hub, written once at the factory, before anything is
burned. On BD-R it holds a few 16-byte units. The AACS specification for recordable Blu-ray puts
its Media ID there, which is why licensed drives must read it; whether a drive *hands it to a
program* without AACS authentication is a separate question, and the one that mattered here.

It is not the **media id** (`VERBAT/IMe`): that is the maker and dye type, from the disc
information in the lead-in, and it is the same on every disc of a pack.

## How it was read

`dev-tools/disc-probe.c` (Linux, SG_IO, read only) sends the MMC commands below and prints what
comes back, with `--raw` for the bytes:

| Command | What | Needs AACS? |
|---|---|---|
| INQUIRY | the drive's vendor, model, firmware | no |
| GET CONFIGURATION, READ DISC INFORMATION | the disc's kind (profile) and state (blank, open, closed) | no |
| READ DISC STRUCTURE, BD, format 00h | disc information: the media id (`DI` unit: type at +8, maker at +100, type id at +106, revision at +111) | no |
| READ DISC STRUCTURE, BD, format 03h | the BCA, 64 bytes | **no**, on the drive below |
| READ DISC STRUCTURE, BD, formats 81h, 82h | the AACS media serial number and media identifier | yes: refused |

arv itself reads only format 03h (`src/arv/drive.c`), in `arv burned --device` and
`arv check --device`.

## Results

| Drive | Firmware | Media | Disc | BCA without AACS | AACS ids | Serials differ within a pack |
|---|---|---|---|---|---|---|
| Pioneer BDR-XD08 | 1.02 | Verbatim BD-R, `VERBAT/IMe` | blank | **yes**, 64 bytes | refused, sense `5/6F/02` | **yes** (2 of 2 discs) |

The probe's output for each disc, as captured: [disc 1](pioneer-bdr-xd08_verbatim-ime_disc1.txt),
[disc 2](pioneer-bdr-xd08_verbatim-ime_disc2.txt). (They predate the probe printing only the
16-byte serial in its last line.)

### The bytes, compared

The 64 bytes are four 16-byte units, each written twice (units 0 and 1 are the same, as are 2
and 3: redundancy against a scratch):

```
             0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
unit 0  d1:  04 1e 10 15 12 62 02 02 24 03 02 14 50 10 48 af
        d2:  04 1e 10 15 12 62 01 02 24 03 02 14 47 28 48 97
                               ^^                ^^^^^    ^^
unit 2:      fc 42 44 52 01 12 01 00 00 00 00 00 00 00 00 00     (both discs)
                "B  D  R" 01 12 01
```

- **Unit 2** is the same on both discs: `BDR` and `01 12 01`, the same bytes as in the disc
  information (`42 44 52 01 12 01` at its offset 8). It says what kind of disc this is, nothing more.
- **Unit 0** differs, so it is the disc's own: arv takes it as the serial, 32 hex digits
  (`041e10151262020224030214501048af` for disc 1).

Within unit 0 (a reading of two samples, **not** from a specification):

| Bytes | Disc 1 | Disc 2 | Reading |
|---|---|---|---|
| 0-5 | `04 1e 10 15 12 62` | same | constant: a format or maker code |
| 6 | `02` | `01` | differs: a production line or machine? |
| 7 | `02` | same | constant |
| 8-13 | `24 03 02 14 50 10` | `24 03 02 14 47 28` | BCD date and time: 2024-03-02 14:50:10 and 14:47:28 |
| 14 | `48` | same | constant |
| 15 | `af` | `97` | differs: a check byte? |

If that reading is right, a serial is a time to the second plus a line, so two discs share one
only if made on the same line in the same second: unique enough to tell copies of one image
apart, which is all arv asks of it.

## What arv does with it

- `arv burned --device` reads the serial (Linux) and records it with the copy (`Bca:` on its
  `replication` event), and refuses a disc whose serial is already recorded: each copy is
  recorded once. `--bca SERIAL` gives it by hand (any system, or a drive that will not read it).
- `arv check --device` reads it and knows which copy is in the drive (`ID copy B: OK`); it refuses
  a `--copy` that disagrees, and a disc that is a copy of another id. A check of a copy recorded
  without a serial adds the serial it read, so the copy is known by it next time.
- The letter stays the copy's name: an image file or folder has no BCA, another system or a USB
  enclosure may not pass the command through, and someone holding the disc decades from now
  reads the letter on the hub.

## Open questions

- **A burned disc**: the BCA is factory-written, so burning should not change it. Not yet run on a
  disc before and after burning.
- **Other media**: M-DISC BD-R, other Verbatim lines, other makers. Is there always a BCA, and is
  unit 0 always the serial?
- **Other drives and firmware**, and drives in USB enclosures (some bridges do not pass MMC
  commands through).
- **Is unit 0 the AACS Media ID?** The AACS recordable spec says the Media ID is in the BCA, and the
  drive refuses the AACS identifier commands without authentication, yet returns the raw BCA. Not
  needed for arv; noted in case it matters to someone reading the bytes later.

## Adding a result

Build and run the probe on two discs of one pack (blank is fine):

```sh
cc -o disc-probe dev-tools/disc-probe.c
./disc-probe /dev/sr0 --raw > drive_media_disc1.txt
```

Save each output here as `DRIVE_MEDIA_discN.txt` (lowercase, hyphens), with only what the probe
printed: no shell prompt, user or host name. Add a row to the results table. The serials of
blank test discs say nothing about anyone; those of discs holding an archive are better left out.
