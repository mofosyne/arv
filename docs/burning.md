# Burning and checking a disc

`arv make` stops at the image (`<disc-id>.iso`). The burn is where an archive most often loses
data unnoticed: a burn that "succeeded" but wrote the wrong size, a verify that read the
computer's cache instead of the disc, a burner that reformatted the medium. This page is how to
burn so that none of that goes unseen, and how to check the result.

**Status:** the commands below follow each program's documentation and arv's own checks; the
first real burns (issue #5) confirm them on hardware. Until then, the read-back check (step 3)
is what tells you a burn is good: trust it over any program's "success".

## 1. Before burning

- **Media.** M-DISC BD-R (25, 50, 100 GB) is the standard here; ordinary BD-R is fine for
  extra copies. Use the medium you made the image for (`arv make --medium`, default `bd25`).
- **Size.** arv records the finished image's size and SHA-256 in the home catalogue
  (`ImageSectors`, `ImageSha256` in the disc's `Binding` record):

  ```sh
  recsel -t Binding -e "Volume = 'TRIP-01_2019_4'" -p ImageSectors,ImageSha256 ~/.arv/catalog/archive.rec
  ```

- **Defect management.** A BD-R can be written plain (the whole disc holds data) or formatted
  with spare areas, where the drive checks what it writes and moves bad spots to the spares
  (slower; less room). arv's images fit either way by default: `bd25` images are 11,826,176
  sectors, what a formatted 25 GB BD-R holds. Images made with `--no-defect-management` are
  bigger (12,219,392 sectors for 25 GB) and need the disc written plain.
- **Keep the `.iso` until step 3 passes** (and, for a set, until every copy is burned).

## 2. Burn

Burn the image **as it is**, one session, closed: never "add files", never a data-disc project
in a burning program (that would make a new filesystem without arv's layout and RS03).

**Linux, xorriso** (recommended; package `xorriso`):

```sh
xorriso -outdev /dev/sr0 -tell_media_space      # the blank disc's room, in 2048-byte sectors
xorriso -as cdrecord -v dev=/dev/sr0 -eject TRIP-01_2019_4.iso
```

The room must be at least the image's `ImageSectors`. xorriso writes a blank BD-R as it is,
without formatting it. Drives normally pick the speed the media is rated for (4x for M-DISC);
to choose one, see xorriso's `-speed` (its units differ between CD, DVD and BD).

**Linux, growisofs:** by default it formats a blank BD-R with spare areas, and some tools then
see it differently; tell it not to, and use the image as a whole:

```sh
growisofs -use-the-force-luke=spare:none -speed=4 -Z /dev/sr0=TRIP-01_2019_4.iso
```

**Windows:** right-click the `.iso`, *Burn disc image*. **macOS:** `hdiutil burn TRIP-01_2019_4.iso`.
Both burn the image as it is; check with step 3 like any other burn (from a Linux machine, or
another system with arv: `arv.com check --device`).

Not suitable: burning programs' data-disc modes (Brasero's and K3b's projects, the Finder's
*Burn folder*), and anything that offers to "finalize later" or write a multi-session disc.

## 3. Check the burn, from the disc itself

Eject the disc and put it back (the system then forgets what it cached while writing). Then:

```sh
arv check --device /dev/sr0
```

arv reads the disc's volume label to know which disc it is, reads exactly the image's sectors
from the drive (after asking the system to drop anything cached), and compares the SHA-256 with
the one recorded at creation. `the disc holds exactly the image arv made` and `OK` mean the burn
is good; the check is logged as a fixity check event. Anything else: burn again, on a new disc.

Without arv at hand, the same by hand:

```sh
dd if=/dev/sr0 bs=2048 count=IMAGESECTORS iflag=direct | sha256sum     # must equal ImageSha256
```

(`iflag=direct` reads past the cache; a burned disc may hold a few sectors more than the image,
hence `count`.) A home catalogue made before arv recorded image hashes (October 2026) has none;
then `arv check --device` scans the disc with dvdisaster Light (`dvdisaster -s`) instead, or read
the disc into an image and run `arv check --image disc.iso`.

## 4. Record it

```sh
arv burned TRIP-01_2019_4 --copies 1 --media-id "VERBATIM-MDISC-LOT-1234" --location HOME-PUB-2026
arv burned TRIP-01_2019_4 --copies 1 --location OFFSITE-PUB-2026
```

Write the disc id on the hub (a CD/DVD marker made for discs, not a ballpoint) and on the case.
Then the `.iso` can go, or stay on a drive as one more copy.

## Later: checking and reading damaged discs

- Every few years, per copy: `arv check --device /dev/sr0` (logged; `arv list` and the catalogue
  show when each disc was last checked).
- A disc that will not read cleanly: follow REPAIR in its `README.txt`: read it into an image
  with GNU ddrescue or dvdisaster Light, then `arv check --image disc.iso --repair`. A disc that
  needed repair is a warning: burn a fresh copy from the repaired image.

## The first-burn drill (issue #5)

Do this once before trusting arv with an archive, and again for each new drive, media brand or
burning program. Record the results in issue #5 and in `src/udfwrite/README.md`'s reader table.

1. **Make** a test disc from a folder with awkward names (long, non-ASCII, emoji, deep folders,
   a file over 4 GiB): `arv make --set TEST FOLDER`.
2. **Burn** it (step 2), noting the program, its version, drive, media and speed.
3. **Read back** (step 3): `arv check --device /dev/sr0` must say the disc holds exactly the image.
4. **Open it on every system you may use one day:**
   - Linux: mount it, `sha256sum -c manifest-sha256.txt` from its root, and `tools/arv.com verify .`
     (copied off the disc first, as its README says);
   - Windows 10/11: open it in Explorer, check the awkward names, run `arv.com verify` (as `arv.exe`);
   - macOS: open it in the Finder, check the names, `arv.com verify .`
5. **Damage drill**, on a second burned copy you are willing to ruin: mark a few lines across
   the data side with a permanent marker (or a light scratch), then
   - `ddrescue -b 2048 /dev/sr0 drill.iso drill.map` (let it finish, retries and all);
   - `arv check --image drill.iso --repair` (from the disc's own `tools/arv.com` too);
   - compare with the original: `sha256sum drill.iso` against `ImageSha256`.
   Record how many sectors were unreadable and whether the repair brought back the very image.
6. **Two damaged copies:** damage the first copy elsewhere, read both into the same image with
   ddrescue and the same map file, repair, compare again.

## Known pitfalls

- A burn verify run right after writing, without ejecting, can read the computer's cache, not
  the disc. `arv check --device` asks the system to drop its cache first; ejecting is surer.
- growisofs formats blank BD-R with spare areas unless told not to (above).
- Some burning programs refuse files over 4 GiB in data-disc mode; burning arv's image as an
  image avoids that.
- Storage: see [shelving.md](shelving.md) (cool, dark, upright, in cases, apart).
