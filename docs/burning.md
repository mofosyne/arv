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

## 3. Read it back, and record it

Eject the disc and put it back (the system then forgets what it cached while writing). Then, for
each copy as it is burned:

```sh
arv burned --device /dev/sr0 --location HOME-PUB-2026 --media-id "VERBATIM-MDISC-LOT-1234"
```

Each copy gets the next letter (A, B ...), and arv says which: write it on the hub and the case
after the id. `--copy X` names it yourself instead. On Linux, where the drive reads the disc's BCA
serial (step 7), arv records it with the copy, refuses a disc already recorded, and later
`arv check --device` knows which copy is in the drive without being told.

arv reads the disc's volume label to know which disc it is, reads exactly the image's sectors
from the drive (after asking the system to drop anything cached), and compares the SHA-256 with
the one recorded at creation. Only when they are identical does it record the copy (a
`replication` event with `ReadBack: identical`, and where it is kept); either way the read-back is
logged as a fixity check. A copy that differs or will not read is **not** recorded: burn again, on
a new disc, and keep the bad one out of the archive. An edition of a collection is *safe* once
each of its discs has a copy read back this way; only then does `arv retire` let earlier
editions go (except those kept).

Each copy also gets a *temperature*: `cold` for a disc on a shelf (the default), or the
temperature of the place it is kept (`arv location add NAS "The NAS" --temperature warm`), or
`--temperature hot|warm|cold`. `arv todo` lists discs that have no cold copy.

**Keeping the image, or the files, on a NAS too.** arv copies nothing there itself; copy it
(`cp`, `rsync`), then let arv check and record it:

```sh
cp TRIP-01_2019_4.iso /nas/archive/ && arv stored TRIP-01_2019_4 /nas/archive/TRIP-01_2019_4.iso --location NAS
7z x -o/nas/archive/TRIP-01_2019_4 TRIP-01_2019_4.iso && arv stored TRIP-01_2019_4 /nas/archive/TRIP-01_2019_4 --location NAS
```

The image is read back against its hash; a folder is verified as the disc's bag (every file
against its manifest). Either is recorded as a warm copy unless its place says otherwise. A
NAS copy is a fine second copy, and quick to restore from; it is not a cold one.

`arv burned` without `--device` still records a copy (a disc burned on another machine, say), but
as not read back: `arv todo` keeps listing it until `arv check --device` reads it.

Without arv at hand, the same check by hand:

```sh
dd if=/dev/sr0 bs=2048 count=IMAGESECTORS iflag=direct | sha256sum     # must equal ImageSha256
```

(`iflag=direct` reads past the cache; a burned disc may hold a few sectors more than the image,
hence `count`.)

Write the disc id on the hub (a CD/DVD marker made for discs, not a ballpoint) and on the case.
Then the `.iso` can go, or stay on a drive as one more copy.

## Later: checking and reading damaged discs

- `arv todo` lists what is owed: images not burned, copies not read back, editions not yet safe,
  replaced editions to retire, discs kept in one place, checks overdue.
- Every few years, per copy: `arv check --device /dev/sr0` (logged). `arv list --unchecked-since 5y`
  lists the discs due one, with the date of their last check; `arv list --one-place` those kept in
  only one place. (A check is recorded per disc, not per copy: note which copy in `--note`.)
- A disc that will not read cleanly: follow REPAIR in its `README.txt`: read it into an image
  with GNU ddrescue or dvdisaster Light, then `arv check --image disc.iso --repair`. If arv cannot
  repair it, it prints what to paste next: dvdisaster Light with the disc's medium size (`-n`, which
  arv keeps in the catalogue as `MediumSectors` and in the disc's README.txt), and the reads that
  fetch the missing sectors from another copy. A disc that needed repair is a warning: burn a
  fresh copy from the repaired image.

## The first-burn drill (issue #5)

Do this once before trusting arv with an archive, and again for each new drive, media brand or
burning program. Record the results in issue #5 and in `src/udfwrite/README.md`'s reader table.

1. **Make** a test disc from a folder with awkward names (long, non-ASCII, emoji, deep folders,
   a file over 4 GiB): `arv make --set TEST FOLDER`.
2. **Burn** it (step 2), noting the program, its version, drive, media and speed.
3. **Read back** (step 3): `arv burned --device /dev/sr0` must say the disc is identical to the image.
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
7. **Telling copies apart:** `cc -o disc-probe dev-tools/disc-probe.c && ./disc-probe /dev/sr0`,
   on two discs of the same pack (blank is fine). It prints the drive, the media's maker and
   type, and whether the drive reads a BCA serial, one disc's own. Two serials that differ mean
   arv can tell the copies apart by themselves: on Linux `arv burned --device` records the serial
   with each copy and refuses a disc already recorded, and `arv check --device` says which copy is
   in the drive. Record what it printed in issue #5.

   Tried so far: a Pioneer BDR-XD08 (firmware 1.02) reads the BCA of Verbatim BD-R (`VERBAT/IMe`)
   with no AACS authentication; two discs of one pack gave two serials (a factory time to the
   second, and a line number). The AACS media identifiers themselves are refused without it
   (sense 5/6F/02), and arv does not need them. The bytes, compared, and how to add a result:
   [research/bca/](../research/bca/README.md).

## Known pitfalls

- A burn verify run right after writing, without ejecting, can read the computer's cache, not
  the disc. `arv check --device` asks the system to drop its cache first; ejecting is surer.
- growisofs formats blank BD-R with spare areas unless told not to (above).
- Some burning programs refuse files over 4 GiB in data-disc mode; burning arv's image as an
  image avoids that.
- Storage: see [shelving.md](shelving.md) (cool, dark, upright, in cases, apart).
