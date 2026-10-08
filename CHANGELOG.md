# Changelog

arv's versions, and the disc format's (`Version` in each disc's `catalog.rec`; the spec is
[docs/spec/smart-archive-format.md](docs/spec/smart-archive-format.md)). Discs made by any version stay
readable: a reader that knows a later format reads earlier discs, and every disc carries the
exact source that made it in `tools/` (its `Software` field names the commit).

## 1.0: when it is tagged

1.0 is tagged once the first real burns pass, because only real hardware can show that the
format needs no further change. Then the format is frozen at `Version: 1.0`, and later changes
only add fields.

- [ ] The first-burn drill passes ([docs/burning.md](docs/burning.md), issue #5): read back
      against the image hash; Linux, Windows and macOS open the disc and run `tools/arv.com`;
      a damaged copy read with ddrescue is repaired to the very image.
- [ ] The RS03 weak spot (issue #4) is measured on a full-size BD-R image.
- [ ] `FORMAT_VERSION` becomes `1.0` (src/arv/make.c), `VERSION` becomes `arv 1.0`
      (src/arv/arv.h), the samples are remade and published, and the commit is tagged `v1.0`.

## Unreleased (format 0.5), October 2026

What 1.0 is planned to be, as of now. Format 0.5 is what every disc made today carries.

**Discs carry the vocabularies** (format 0.5)
- A disc with a full catalogue snapshot carries the home's `config/sets.rec` and
  `config/tags.rec` in `catalog/config/`, so the words its records use travel with it.
  `arv rebuild` restores them into a home that has none, and says so when the home's differ.

**`arv make FOLDER` records a data object**
- A folder that is not a collection's workflow folder and fits on one disc is a data object, as
  a plan's items are: `arv objects` lists it, `arv status FOLDER` says whether it changed since,
  and making it again after a change records the next version. A folder split over several discs
  is not one (its parts are on different discs).

**Laying a disc out** (issue #23)
- docs/workflow.md, "Laying the folder out": the top level is kinds, from the set vocabulary; the
  level below is a fact in the data (year of capture, project, record type, source); below that,
  only a boundary that already exists. Interpretation stays virtual, and a disc's layout changes
  only with a new edition of its content. docs/philosophy.md calls the disc's layout the third
  order, beside the shelf and the catalogue. Advice only: arv still never moves files.
- `arv status FOLDER` and `arv make` note top-level folders whose names say nothing about what
  they hold (`misc/`, `New folder (2)/`), and, when most of the top level is kinds from the set
  vocabulary, the folders that are not. Folders named by a year count as facts; a repository's
  own tree is left alone. Nothing is recorded, so the format does not change.

**Retiring one disc, and a data object's last copy**
- `arv retire DISC-ID` retires a disc that is not part of an edition (a disc plan's, or
  `arv make FOLDER`'s). It refuses while any of its files is on no other disc that stays, and names
  each data object on it: the version, whether another disc has it, and whether it is the newest.
  An older version that a newer one replaces may go; the only copy of the newest is refused.
  `--yes --accept-loss` records the files as `Lost:` on the disc, and `arv find` lists them as
  `LOST`. An edition's disc is refused: editions are retired together (`arv retire CODE`).

**Icons for kinds of file in the GUI**
- The Mastering file browser, the plan's discs, Search results and Objects show an icon for the
  kind of file (📷 image, 🎬 video, 🎵 audio, 📄 document, 📊 spreadsheet, 🗜️ archive, 💿 disc
  image, ⌨️ code; 📁 folder), with a label for screen readers. Guessed from the extension, as a
  hint only: nothing is recorded, and Siegfried's `formats.csv` stays the record of formats.

**Appraise first: the levels ask for care** (issue #24)
- Each importance level now asks for a number of copies and places: essential, 3 copies in 3
  places; important, 2 in 2; useful, 1; incidental, none (everyday storage is enough).
  `arv todo` reports a disc holding less than the highest level in force on it, on anything on
  it, or on its set; a model's unreviewed suggestion does not count. A disc not yet appraised is
  held to two places, as before.
- docs/philosophy.md: "Appraise first: most things are not preserved", with the levels, what
  each asks for, and the order of the work.

**Archive, Record, Verify: the interface follows the name**
- `arv --help` groups every command under Archive (what goes on discs), Record (what exists, and
  where) and Verify (still good, and can be got back); the commands themselves are unchanged.
- The GUI has three tabs to match, each with its views (Archive: Mastering, From a folder; Record:
  Objects, Discs, Search; Verify: Owed, Check, Restore). It opens on Verify's Owed (`arv todo`), and
  a count of what is owed sits above the tabs. Jobs open from the header. New in the GUI: verifying
  and restoring a disc's files.

**Safer making, and a portable arv**
- `arv make` checks each file's bytes against its manifest as they are written to the image; a
  file changed after hashing stops the make and nothing is recorded (before, the disc would have
  failed `arv verify` later).
- arv finds a `.arv` home beside itself (`tools/arv.com` and `.arv/` on one drive), so it runs
  from a portable drive without a machine config and writes nothing outside the drive.

**Disc plans: discs composed by hand** (no format change)
- `arv plan` composes discs from files and folders anywhere (a film on the PC, photos on the
  NAS): which goes on which disc, and where under `data/`. A plan points at its sources and copies
  nothing; `arv plan show` measures them against each disc's room, and `arv plan make` makes
  one image a disc (`arv make --plan`). The GUI's new Mastering tab is the same, with drag and drop.
  Plans point at their sources; `arv plan add --copy` copies an item into the plan instead (an
  SD card that will not be there at make time), and `plan show`/`plan make` say which items changed
  since they were planned (`arv plan refresh`). Sources inside the home's drive are kept relative.
  A made plan is kept as a template: `arv plan list` hides it unless `--all`, and `arv plan again
  NAME NEW` starts the same selection again as a new plan (the GUI: "Make it again").
- Each planned file or folder becomes a **data object** (`Object` record, format 0.5): known by
  the hash of its content, in linked versions, with no `.arv` marker. `arv status` says whether
  each object archived from a folder is unchanged, changed or gone, and recognises a moved folder
  by content; `arv find` lists objects. Source paths stay in the home catalogue.
- `arv objects [NAME]`: everything kept and where every copy of it is (each data object's
  versions, each collection's newest edition; their discs, every copy's form and temperature,
  read back or not; the original on the PC/NAS). `arv todo` also lists data objects whose newest
  version has no cold copy, or that are no longer where they came from with fewer than two copies.
  The GUI's new Objects tab shows the same (`arv objects --json`).
- `arv retire --yes` refuses while any file is on the retiring discs only; `--accept-loss`
  retires anyway and records those files as `Lost:` on the edition (`arv log`, `arv find`).
  `arv find` marks files on retired discs `[retired DATE]`.

**Collections over time** (format 0.5; design in research/plan.md, being built)
- A *collection* is something kept and made into discs again and again, from one workflow
  folder with one history: `Collection` records (with a Uuid) and `Revision` records (git-like:
  `Tree` and `Node` hashes; checkpoints, and editions, each a set of discs holding a selection
  of the collection and a copy of the catalogue; `--keep` for one never to retire).
- `arv burned --device DRIVE` reads a burned copy back against its image and records it only
  if identical (arv never drives the burner: `arv make` prints the command). An edition is safe
  once each disc has such a copy; `arv retire CODE` then retires the editions it replaces
  (not kept ones), after listing any files found only on them. `arv todo` lists what is owed.
- Copies have a form (disc, iso, folder) and a temperature: hot (in active use), warm (online
  or reachable, left alone: an image on a NAS) or cold (offline: discs on a shelf); places carry
  a default (`arv location add|move --temperature`), and `arv todo` lists discs with no cold copy.
- Git repositories go on a disc as working files plus a compacted `.git` (one pack; no hooks,
  reflogs but the stash's, or credentials in remote URLs), optionally trimmed with
  `--git-since DATE`; `git.tsv` lists each repository's roots, heads and commits. `arv status`
  recognises a repository by its history (archived, ahead of a branch on a disc, diverged, not
  archived) and `arv find COMMIT` names the discs holding a commit.
- `arv stored DISC-ID PATH`: a copy kept on a drive or NAS, as the image file or as a folder,
  checked (read back, or verified as the bag) and then recorded; warm unless its place says.
- Each home catalogue (archive) has an identity: a `Home` record with a Uuid, made by `arv init`;
  every disc names it (`HomeUuid`), a home rebuilt from a disc keeps it, and `arv rebuild`
  refuses a disc of another archive unless `--any-archive`. `arv where` shows it.
- `arv collection init|list|show`; `arv make` on a workflow folder makes the next edition.
  `arv status` (what changed since the last revision, or for any folder which files are on
  which discs), `arv checkpoint`, `arv log`, `arv diff`, `arv link [--past]`; a hash cache
  (path, device, inode, size, time) so a second `status` reads nothing (`--deep` rereads).
- The virtual folders across discs are now *selections* (`arv selection`, `Selection`
  records, `selection:CODE`). No compatibility with earlier drafts: nothing has been burned.

**Making discs**
- One standard output: a closed **UDF 2.50** image by arv's own writer (src/udfwrite), with a
  real metadata mirror, contiguous files in a fixed order, and reproducible bytes
  ([docs/spec/archival-udf.md](docs/spec/archival-udf.md)). Hybrid ISO 9660 images and the NetBSD makefs
  writer are gone; makefs lives on in `upstream/` for the fixes offered to NetBSD.
- **RS03 error correction added by arv itself** (src/rs03), byte for byte what dvdisaster
  writes, then every sector tested. No dvdisaster needed to make a disc.
- Every disc carries arv's source and **`tools/arv.com`**, one executable for Linux, macOS,
  Windows and the BSDs, that verifies, restores, searches and repairs.
- The home catalogue records each finished image's size and SHA-256 (`ImageSectors`,
  `ImageSha256` in its Binding record).

**Checking and repairing**
- `arv check --device` reads a burned disc back against its image hash, past the system's cache.
- `arv check --image IMAGE --repair` repairs a damaged image in place, as dvdisaster does and to
  the same bytes, and works on an image that is in no catalogue at all.
- `arv list --unchecked-since 5y` and `--one-place`: the discs due a check, and those kept in
  only one place.

**Documentation for the long term**
- [docs/spec/rs03-format.md](docs/spec/rs03-format.md): the error-correction format, written so that a
  repair tool can be written from it alone (checked by doing exactly that), with test vectors.
- [docs/burning.md](docs/burning.md): burning, checking the burn, and the drill for a new drive
  or media.
- Each disc's `README.txt` explains reading a damaged disc with ddrescue or dvdisaster and
  repairing it with the arv on the disc.

**The program**
- arv is a C99 program with no libraries (src/arv). It was ported from the Python arv one
  command at a time and held to the same outputs (tests/reference), and the Python core was
  then removed. The optional local AI helpers (`describe`, `tag`, `models`) are arv-assist, in C
  too (src/arv-assist); only the optional web interface, arv-gui (src/arv-gui), is in Python.
  `make` builds everything; arv runs the two helpers for their commands.
- arv's own BagIt validator and digests (src/bagit; also a `bagit` program for any bag), checked
  against the Library of Congress's bagit.py; the disc no longer carries `tools/bagit.py`.

## Earlier formats

From the spec's versioning section:
- **0.5** (2026-10-05): `Selection` (was `Collection`), and the new `Collection` and `Revision`.
- **0.4** (2026-10-03): `Authorship` on events, `Appraisal` records, listing version 2 (links,
  executables).
- **0.3**: the medium's fields moved into `Binding` records.
- **0.2**: per-volume files grouped by volume (`catalog/volumes/<id>/`).
- **0.1**: the first sample discs, never burned; per-volume files kept by kind.

The history before arv, the original shell scripts, is in `scripts/`; the design and the
reasons behind each decision are in [research/plan.md](research/plan.md).
