# Plan

Working plan for turning the scripts into an `archive` tool. Background is in
`research-notes.md` and `metadata-standards.md`.

## Principles

The project's overall philosophy is in [philosophy.md](../docs/philosophy.md); these are the technical
principles that follow from it.

1. **Plain text is the source of truth.** Everything else (SQLite, HTML,
   RO-Crate) is generated and can be rebuilt.
2. **Every disc stands alone.** It can be verified, browsed and understood with
   what is on it plus a web browser, and repaired with dvdisaster.
3. **Every disc indexes the archive.** It carries a snapshot of the whole
   catalogue, so the newest disc is enough to rebuild the index.
4. **No re-scanning.** Catalogue data is produced while the image is built.
5. **Degrade gracefully.** Useful with a browser without JavaScript, or even
   with `cat`, `grep` and `sha256sum` only.

## Disc layout

```
index.html   README.txt                        ← open these first
bagit.txt  bag-info.txt  manifest-*.txt  tagmanifest-*.txt
catalog.rec                                    this disc: Disc/Copy/Event records
catalog/                                       snapshot of the whole archive at burn time
  archive.rec
  volumes/<disc-id>/                           each volume's index: manifest.sha256,
                                               listing.tsv, formats.csv, tags.tsv
tools/
  arv/                                         uncompressed snapshot of this repo (HEAD)
  arv.bundle                                   git bundle, only with --tools-history
  dvdisaster/                                  source tarball + static Linux/Windows binaries
  bagit.py
data/                                          payload (embedded EXIF/XMP untouched)
[dvdisaster RS03 ECC after the filesystem]
```

All non-`data/` files are BagIt tag files, covered by the tagmanifests.

## On-disc copy of this repo

- Uncompressed snapshot of the last commit (browsable with no tools). The full history
  (`git bundle`) is opt-in with `--tools-history`: by default discs don't carry it, so
  large files removed from the repo (e.g. old model weights) never ride along.
  No tar.gz: a stream is fragile after the first bad byte.
- The commit hash is recorded in `catalog.rec` as the agent of the creation event.
- Keep the CLI **Python standard library only** (vendor `bagit.py`) so the
  on-disc copy runs without `pip`.

## On-disc HTML viewer

- `index.html`: static, no JavaScript. Disc description, notes, location, a
  folder tree with relative links into `data/`, and sizes and checksums.
- ~~`search.html`~~: removed 2026-10-01 (see Decisions). Searching across
  discs is catalogue software's job, or `arv --home catalog find` run from
  the disc. The notes below are kept for the record.
- One search box with scope **This disc / All discs**, plus disc-level search
  over titles, descriptions, notes and subjects. Results show disc ID, title,
  physical location and path:
  - hits on **this disc** link straight to the file;
  - hits on **past discs** say which disc to fetch and where it is (e.g.
    "PRJ25-01, Projects 2020-2025, Shelf A / Box 3"), and copy the path.
- Only discs burned **before or with** this one are searchable, since the
  snapshot is frozen at burn time.
- Doubles as the RO-Crate preview if the RO-Crate export is enabled.
- Limits to test: browser memory with ~1M entries across discs, and very
  large single-disc listings in the no-JS page (it may need splitting per
  top-level folder).

## Phases

### Phase 1: single-disc MVP
- [x] `arv make <folder>`: bag → prompt for Title/Description/Notes/Subject →
      `catalog.rec` → `tools/` → `index.html` → image → RS03 → `dvdisaster -t`
- [x] Home catalogue `archive.rec` (Disc/Copy/Event), short disc IDs, `Location`
- [x] `arv find <pattern>` (plain scan of manifests to start)
- [x] `arv note <disc-id> <text>` (+ `arv locate`)
- [x] Tests (`tests/test_arv.py`)

### Phase 2: whole-archive retrieval
- [x] `catalog/` snapshot with `--snapshot full|set|disc` (+ `listings/` with sizes and dates)
- [ ] ~~Snapshot hash chain~~ dropped for integrity (the tagmanifests already cover it); back as a *history* graph, see "Design: discs as nodes in a history graph" (2026-10-02)
- [x] Per-copy tracking: each copy a letter (`Copy:` on its replication event), its own place; checks name the copy
- [x] The BD-R BCA serial as a copy's automatic id: read by `arv burned --device` and `arv check --device` (Linux, `drive.c`), or `--bca`; a Pioneer BDR-XD08 reads it from Verbatim BD-R without AACS authentication ([research/bca/](bca/README.md), issue #5)
- [x] ~~`search.html` across the snapshot~~ (built, then removed 2026-10-01)
- [x] ~~Generated `archive.sqlite` (`arv index`)~~ retired 2026-10-04: in C, scanning the
      plain-text lists finds a name among 2 million paths in 0.3 s, as fast as Python did with
      the index, which took 9 s to build and was 83% the size of the catalogue it indexed
- [x] `arv check --device|--image`: dvdisaster scan/test → `fixity check` Event
- [x] `arv rebuild <disc>`: merge a disc's catalogue into home (idempotent)
- [x] `arv burned <id> --copies N`: record manual burns (`replication` Event)
- [ ] Import from VVV exports

### Phase 3: scale and standards
- [x] Split a source folder across N discs (`Bag-Count: n of N`, `Bag-Group-Identifier`); exact sizing via `genisoimage -print-size`
- [x] Medium sizing (BD 25/50/100/128), `--no-defect-management`, `--min-redundancy` (default 20%)
- [x] Siegfried/PRONOM `catalog/volumes/<id>/formats.csv` (+ `format identification` event), RO-Crate 1.2 export (`--ro-crate`, crate inside `data/` per the RO-Crate BagIt notes)

### Phase 4: extras
- [x] GUI over the CLI (`arv gui`)
- [x] NetBSD `makefs -t udf` builds on Linux as a C library: `src/udfmake/`, now `upstream/netbsd-makefs/` (UDF 2.50,
      metadata partition, two upstream bugs fixed).
- [ ] Confirm `upstream/netbsd-makefs/BUG-REPORT.md` by hand, then send it to NetBSD.
- [ ] Metadata mirror duplication in udfmake (upstream lacks it).
- [x] `arv make --filesystem udf250` using it (the default since 2026-10-03).
- [ ] Test the images with a Linux kernel mount, Windows and macOS.
- [ ] Standalone RS03 library (see below). Needs the licence decision first.

## Decisions (2026-09-29)

- CLI: **Python, standard library only**.
- Disc ids: **year range of the files + set + number**, e.g. `2020-2025_PROJECTS_01`.
- Discs for other people or external parties: **minimal catalogue**
  (`--snapshot set`, only this set). Own off-site copies can take `full`.
- Encryption: optional later, not implemented now (direction: "Direction (2026-10-07): locking, the data object as the unit").
- Media: **M-DISC BD-R** as standard.
- Copies: not managed. The tool makes the ISO; you burn it and record the count with `arv burned`.
- Physical disc identity: our disc id (volume label + bag-info + written on the disc). Drive-reported
  Media IDs identify the product line, not the disc; stored only as optional info.
- Redundancy: **RS03 inside every image + whole extra copies** rather than
  off-disc parity files. Burn copies from the **same ISO** so they are
  sector-identical: dvdisaster can then fill an image's unreadable sectors by
  reading another copy into the same image file before applying RS03. Off-disc
  RS01 `.ecc` files are only worth it for old discs burned without ECC.
- Default target: **BD-R 25GB with >= 20% RS03 redundancy** (~20 GB data per disc). Requires the
  speed47 dvdisaster fork to fill the disc; the stock build is detected and warned about.
- RO-Crate is opt-in (`--ro-crate`) because it adds two files to `data/`, so the payload is no
  longer byte-identical to the source folder.
- GUI: **local web UI** (`arv gui`), not Tk. tkinter is a separate distro package
  (`python3-tk`) and was missing even here; a browser is always present and matches the
  on-disc HTML. Bound to 127.0.0.1, token-protected, and it only runs `archive` commands.
- Manifest paths are written unencoded (bagit-python and `sha256sum -c`
  compatible). Names with CR/LF or a literal `%0A`/`%0D`/`%25` are rejected.

## Decisions (2026-09-30)

- Language: **Python for the workflow while it is still changing; C for durable, low-level
  format code** (`src/udfmake`, later an RS03 library). A full C rewrite is worth it only
  once the workflow is settled. To keep that port cheap:
  - the formats are the contract: `docs/spec/smart-archive-format.md` is the spec, not the Python code;
  - tests should move toward language-neutral fixtures (sample catalogues, ids, listings and
    expected outputs as files) that a C version can be checked against;
  - heavy parts live in separate C libraries and tools that a port reuses unchanged;
  - Python modules stay small with one job each (recfile, discid, catalog, bag).
- Disc ids: superseded the 2026-09-29 entry. Scheme `set-seq-coverage/1`
  (`PHOTO-07_2015-2024_Q`), derived from the record's Set, Sequence and Coverage.
- UDF 2.50: available through `src/udfmake` (NetBSD makefs as a C library and program). The
  hybrid ISO9660 + UDF 1.02 image stays the default. *Superseded 2026-10-03: UDF 2.50 is the
  default (the Blu-ray standard, long Unicode names, no genisoimage needed); `--filesystem hybrid`
  stays for very old systems and is named `<id>.hybrid.iso`.*

## Optional: local LLM metadata help (done)

- `arv describe` (folder or disc), `arv make --llm` / `--draft`, GUI "Suggest" panels.
- OpenAI-compatible HTTP API, standard library only; loopback only unless `--llm-allow-remote`.
- The model gets an inventory (names, counts, sizes, dates, types, short README text), returns
  title / description / subjects / folder tags / questions as JSON. The owner reviews every
  field; Q&A answers become notes; accepted changes are PREMIS `metadata modification` events
  with agent `llm:<model> + owner review`.
- Folder tags: `catalog/volumes/<disc-id>/tags.tsv`, searched by `find` and the GUI.
- Images (done, `--vision`): sampled images and video frames go to a local vision model only
  (loopback enforced, no override, since file contents leave the inventory-only design).
  Captions feed the text model and are stored in the tags file (third column); searchable.
  Tiny models (SmolVLM-500M) caption well but ignore output formats, so the prompt asks for
  plain "Caption:/Tags:" lines, never JSON mode, and echoed instructions are discarded.
- Possible later: per-file captions for all images (not just samples) as an overnight job.

## Next: hybrid AI tiers (agreed direction, 2026-09-29)

See research-notes.md section 5 for the measurements behind this.

1. **Built-in (no setup):** a ~37 MB embedding model (bge-small-en-v1.5) for topic tags from a
   fixed, editable tag vocabulary, "more like this", and nearest-neighbour learning from the
   owner's past tag reviews. Runtime managed by the tool (a local process on a random loopback
   port). Model and runtime are a pinned, SHA-256-checked download to
   the home's `cache/models/`, never committed (every disc carries the repo history).
   Done: `arv tag` runs llama.cpp's `llama-embedding` CLI as a subprocess (no server/API),
   with `--embed-url` (OpenAI-compatible /v1/embeddings) as a fallback engine.
2. **Bring your own AI (optional, OpenAI-compatible):** >= 1.5B model for SemIf-style decisions
   (logprob readout over lettered options: private/shareable, content type, rule-based yes/no);
   >= 3B for descriptions and questions (existing); local vision model for captions (existing).
   Capabilities detected; fall back to tier 1 when no server is present.

All tiers remain optional, suggestion-only, and recorded as PREMIS events with the model as agent.

## Decision (2026-10-02): the name `arv`

- **`arv`** is both Norwegian for "inheritance" and short for **Archive, Record, Verify**.
  Short and Unix-style; free in Debian/Ubuntu. Not `ark` (KDE's Ark ships `/usr/bin/ark`, and
  ARK is the Archival Resource Key identifier scheme).
- **The folder beside the files is always `.arv`.**
- The on-disc format keeps its name, *smart-archive* (written into every disc's `catalog.rec`).
- Done: the command is `arv` (`make install`, Linux); the home is found by walking up to a `.arv`
  folder or `.arv` pointer file (past `.git`), or a disc root, then `~/.config/arv/homes.rec`
  (machine-local paths, never on a disc); `arv init`, `arv where`.
- Later: `Root` records (the archive's trees by name, in the catalogue) and `Mount` records
  (where each is mounted on this machine, in `homes.rec`), for `arv status` across every tree.

## Decision (2026-10-02): arv makes discs, Katalog browses them

- **Separate concerns.** `arv` (and `arv gui`) makes, describes and checks archive discs.
  Browsing and searching across many devices, virtual folders and FUSE mounts are catalogue
  software's job, Katalog's in particular; `arv find` stays only so one disc can be searched
  with nothing installed.
- **They meet through the disc format, not through code.** A Katalog "smart import" would read
  `catalog.rec` and `catalog/` (smart-archive-format.md, "Reading a disc" and "Mapping to
  Katalog") instead of scanning. To propose to Katalog's developer
  ([StephaneCouturier/Katalog](https://github.com/StephaneCouturier/Katalog)), with the sample
  discs as test data.
- **Sharpened 2026-10-10:** the line is the archive's care, not who made the disc. arv records
  everything in the archive's care, old discs it takes in included (`arv found`, below);
  Katalog indexes everything else. See "Decision (2026-10-10): keep the record, let other
  tools do the work" and philosophy.md, principle 7.

## Design: four layers, and a binding per volume (2026-10-02; Binding done)

Blu-ray and UDF are today's medium, not the format (philosophy.md, principle 4). Drawn and
explained in [architecture.md](../docs/architecture.md).

| Layer | What it is | Depends on the medium? |
|---|---|---|
| 1. Content | the files, untouched, and their hashes (BagIt manifest) | no |
| 2. Description | the catalogue: volumes, events, locations, collections, appraisals, history | no |
| 3. Container | how a volume is laid out: UDF 2.50 or hybrid ISO on Blu-ray today; possibly LTFS, exFAT, tar, an AFS reel | yes |
| 4. Protection | repair for that medium: RS03 for disc images; tape's own ECC; parity on drives | yes |

Nothing in layers 1-2 may depend on layers 3-4. Container facts go in a per-volume record:

```
%rec: Binding
Volume: TRIP-01_2019_4
Container: udf-2.50                    (iso9660+udf-1.02; later ltfs, exfat, tar, afs ...)
Protection: rs03                       (or none)
Media, Filesystem, Ecc                 (the same for people), MediumSectors: 12219392
Extents: volumes/TRIP-01_2019_4/extents.tsv   (planned, optional: path, start, length in the
                                               container's units: sectors, blocks, frames)
```

- [x] Move `Filesystem`, `Media` and `Ecc` from the Disc record into Binding (readers accept both;
      format 0.3; `Container: iso9660+udf-1.02 | udf-2.50`, `Protection: rs03 | none`).
- [ ] Record file extents as Binding data off the disc (home catalogue, later discs' snapshots),
      written by the archival disc writer; see the 2026-10-03 decisions (Piql lesson 3).
- [ ] Per-binding recovery steps in README.txt (dvdisaster for disc images).
- [ ] Name checks and size limits become per-container profiles (already `--filesystem`).
- [ ] Keep Piql compatibility in reach: per-file records stay mappable to AFS `tocdatafile.xsd`
      (id, name, parent, date, size, checksum, format id, metadata, start/end); later, an AFS
      table-of-contents export via afslib (GPLv3, it can write) or an `afs` binding. Piql's
      ingest (PiqlConnect) runs Archivematica, which accepts our BagIt bags directly.

## Decisions (2026-10-03, revised): one codebase, an archival disc writer, a binding per medium

Agreed after pushing back on "UDF for every medium". Supersedes the first drafts of the C port
and the minimal UDF writer.

**Reuse, at the right level.** "Don't redo a solved problem" applies to whole programs: if an
arv-like program already did 90% of this, we would join it and argue for the other 10%. It does
not mean calling other people's programs at run time: that breaks the self-contained, long-term
goal. arv owns its parts, built from source in this repository.

**One codebase in C, as an Actually Portable Executable.** The point of C is to bring the C
libraries together, so arv is one repository and one program, shipped on every disc. One
`arv.com` (Cosmopolitan) runs on Linux, macOS, Windows and the BSDs, next to its source in
`tools/`.
- The contract: `tests/fixtures/` (language-neutral TSV cases) pass in C, then whole images from
  both versions compare byte for byte (ids and dates fixed). Python stays the reference until then.
- Order: (1) format pieces: recfile (our own small parser, checked with `recfix`/`recsel`), disc
  ids, names, listing, catalogue; (2) BagIt and SHA-256/512; (3) `arv make` with the archival disc
  writer linked in; (4) `check`, `find`, `rebuild` and the rest; (5) the GUI server: `gui.html`
  stays, the backend becomes a localhost-only HTTP server with a session token and about ten JSON
  endpoints. Until then the Python GUI drives the C binary (it already runs every action as an
  `arv` subprocess).
- **MVP reached (2026-10-03): `src/arvc/`.** Reading first (`info`, `verify`, `ls`, `restore`,
  `find`, `list`, `id`), then making and recording (`init`, `make`, `burned`, `check`, `note`,
  `locate`): the whole archive, record, verify cycle without Python, C99 + POSIX, udfwrite linked
  in, buildable from any disc's `tools/` with one `cc` line. The contract is held three ways: all
  181 `tests/fixtures/` cases; discs made by both versions compared file by file (and validated
  by bagit.py); catalogues after the same record-keeping commands compared byte for byte. The
  shared texts (descriptors, README.txt template, index.css, default sets) moved to data files
  both versions read. Still Python only: `--split`, hybrid images, drafts and AI helpers,
  Siegfried, RO-Crate, and the catalogue-editing commands (`access`, `location`, `collection`,
  `appraise`, `rebuild`, `index`, `gui`); dvdisaster and (in a checkout) git/tar are still
  called as programs. Then (2026-10-04) the catalogue commands (`access`, `location`,
  `collection`, `appraise`, `sets`, `names`, `where`; 35 runs compared with Python's, output,
  exit codes and catalogue), and **the installed `arv` became the C program**, handing what is not
  ported (AI helpers, `--split`, hybrid images, interactive `make`, `gui`, `index`, `rebuild`) to
  the Python arv. Then `--split` (make.py's fit: the same estimates, rebalancing and discs) and
  `rebuild` (catalog.merge). Next: the interactive prompts and `init --pointer/--name`; `index` was retired instead
  of ported (plain text is fast enough); the AI helpers stay Python longest.
- **Burn test discs first.** Before the port goes far, burn a few real test discs with today's
  Python arv, read them back on other machines, scratch one and repair it (#5, #4). What they teach
  goes into the C version instead of being found after it.
- Risks to test early: reading damaged discs needs dvdisaster's OS-specific drive code, which may
  not work inside an APE binary on Windows or macOS (repairing an image is pure computation). The
  optional AI helpers stay Python, or come last.

**An archival disc writer: one library, UDF and RS03 together.** A disc is written once, whole and
closed, never appended to: that is what "every disc stands alone" means, and it lets the writer
place every structure deliberately for damage resistance.
- **Two modules, one seam.** A UDF 2.50 container writer and an RS03 encoder in one library
  (working name `libarvdisc`), separable as the four layers require, so another medium can reuse
  or replace either half.
- **RS03 stays dvdisaster's format, written by dvdisaster's code** (a pinned copy of dvdisaster
  Light's source in this repository, built by us: "vendored"). In 20 years someone must be able to
  repair our disc with dvdisaster even if arv is gone. GPLv3, like arv; the UDF module can still be
  published separately under BSD-2-Clause if wanted.
- **The UDF subset:** 2048-byte sectors; one partition plus a metadata partition with a *real*
  mirror (udfmake's points at the same blocks); a closed, read-only volume; regular files and
  folders only (no links, devices, extended attributes or named streams); names in OSTA
  compressed Unicode up to 255 bytes, refused if they don't fit; each file in one contiguous run
  (split only at the 1 GiB extent limit) in a fixed order; timestamps from the files, identifiers
  from the disc's UUID, so the same folder and record give the same bytes.
- **The disc's own map is the UDF metadata and its mirror,** protected by RS03: RS03 repairs
  damaged metadata first, and a reader falls back to the mirror if the main copy is unreadable.
  Our writer writes a real mirror and places it far from the main metadata.
- **`extents.tsv` is Binding data kept off the disc,** not a file on it: a file inside the image can
  only be found through the very tree it would replace. The writer plans the whole layout before
  writing a byte, so it knows every file's start sector for free, and records them in the home
  catalogue and every later disc's snapshot (`catalog/volumes/<id>/extents.tsv`). Uses: a disc
  damaged beyond RS03 and both metadata copies still has its map on its siblings; and dvdisaster's
  list of unreadable sectors becomes "these files are affected" by a lookup. (Since files are
  contiguous, in a fixed order, from a fixed start, the offsets could even be computed from
  `listing.tsv` and a written layout rule; the explicit record is kept anyway.)
- **RS03 reads the laid-out image:** each codeword takes bytes from across the whole image, so the
  encoder runs over the image after the filesystem is laid out, as dvdisaster does; inside the
  library, not as a second program.
- **The profile is written down** in `docs/spec/archival-udf.md` (layout, rules, how to check a disc).
  Writing it found a real issue: RS03 appended after the filesystem leaves the two end anchors
  mid-disc, so readers find only the anchor at sector 256 (inside RS03's protection).
- **Checking it:** the Linux kernel mounts it; udftools (`udfinfo`, `udfdump`) and 7-Zip parse it;
  extracted files match the manifest; images compare against udfmake's for the same folder; then
  Windows, macOS and a real burn. `src/udfmake/` stays as the reference and for upstream fixes.
- **A restricted writer was the right call for robustness** (2026-10-03): makefs leaves the mirror
  as a to-do (`/* XXX no support for metadata mirroring yet */`, shared with rewritable formats),
  while ours writes a real one, and the Linux kernel mounts through it with the main metadata
  destroyed. To do: offer NetBSD a patch giving makefs a real mirror for read-only images (#7),
  after the three bug reports (#6).
- **udfwrite is the default writer** (2026-10-03), after Linux mounted all three test images
  (clean, metadata destroyed, anchor 256 destroyed). Windows and macOS are checked as part of the
  first real burn (#5). `--udf-writer udfmake` keeps the reference writer available.
- **No UDF reader of our own.** Reading is the operating system's job, and Blu-ray video keeps
  readers around.

**A binding per medium; the bag never depends on it.** The bag (layers 1 and 2) is the archive; how
it sits on each medium is a binding, and a future medium (something like Project Silica) gets a new
binding, not a new format. The spec states, for each binding, where the bag sits, so arv and Katalog
recognise it the same way: a `catalog.rec` at the bag's root.

| Binding | Medium | The bag sits | Notes |
|---|---|---|---|
| UDF 2.50 image (default on optical) | Blu-ray: presets `bd25`, `bd50`, `bd100`, `bd128` set the capacity and so the space RS03 fills | at the image root | written once, closed; mounts read-only |
| folder | NAS, USB, any drive with a normal filesystem | in a folder named after the disc id | files directly usable; mark read-only; the manifest detects any change |
| zip (when asked for) | NAS or cloud when the archive should behave as one sealed object | at the zip root | store mode by default (compression opt-in; ZFS lz4 already compresses transparently); zip64; UTF-8 names |

- **Accidental changes** are caught by the BagIt manifest whatever the binding (`arv check`,
  `sha256sum -c`); ZFS snapshots make them reversible. Read-only images and one-object zips also
  make them less likely.
- **Not `.iso.gz`:** gzip is one stream over the whole image, so there is no random access (one file
  means decompressing everything before it) and one bad byte destroys everything after it. Zip
  compresses each file on its own.

## Decision (2026-10-04): one standard output, and arv is C (done)

- **One kind of image.** Hybrid ISO 9660 + UDF 1.02 images and the udfmake writer option are
  gone: every disc is UDF 2.50 by udfwrite, with a real metadata mirror. Whoever finds a damaged
  disc never has to guess its layout. Older discs stay readable.
- **The Python core is removed.** Every command was ported to C and gave the same discs,
  catalogues and output (now frozen in `tests/reference/`, and `make check` holds the C arv to
  it, with no Python needed). What stays in Python is an optional add-on: the local AI helpers
  (`describe`, `tag`, `models`), which write drafts that `arv make --draft` takes, and `gui`.
  `make --llm` became those two steps.
- **arv.com on every disc.** Built with Cosmopolitan (`make ape`), one file for Linux, macOS,
  Windows and the BSDs, x86-64 and ARM64, carried as `tools/arv.com` beside the source.
- Still open: the burn test of a real disc on Windows and macOS (#5).

## Decision (2026-10-03): presets, added only when someone needs one

- A volume is made from one **preset** picked by name, with sane defaults: today only the
  Blu-ray sizes (`--medium bd25 | bd50 | bd100 | bd128`). Other presets are added **only when
  someone needs one** (an issue asking for it), not ahead of time. Next candidates: `folder` and
  `zip` (see the bindings table above). Every volume keeps `data/`, the catalogue, `README.txt`
  and `tools/` inside.
- **File names mark deviations** from the preset's defaults with an infix before the extension,
  from a short fixed vocabulary: `TRIP-01_2019_4.noecc.iso` (done: `--no-ecc`). Normal use has
  none. The name is a hint for people; the Binding inside the volume is the truth.
- Disc ids never contain a dot (done), so everything before the first dot is the id.
- If one volume is ever kept in two renditions (an .iso for the shelf, a .tar for the NAS), it is
  one volume with several Bindings; its identity is the payload manifest, not the whole image.

## Decision (2026-10-03): links are noted, never written (done)

- Discs hold files and folders only (BagIt and the copy-anywhere rule), but a folder like a git
  clone has links. As git does, the link is kept as a **description**: every link gets a row in
  `listing.tsv` (listing version 2: kind, link target), and the policy applied is logged as an
  `ingestion` event, the first entry of the archivist log.
- Policy (`arv make --links default|record|copy`): links to files inside the folder are copied
  (the file under the link's name); links to folders inside are noted; links outside the folder
  are refused unless the owner chooses `record` or `copy`; broken links are noted with a warning;
  copying a folder link that loops is refused. Devices, sockets and pipes are refused.
- The execute bit survives (listing kind `file executable`, and on the disc, readable and
  executable by all), so a cloned project's scripts run from the mounted disc.
- Next: the full archivist log (appraisals: why something was kept or left out, for whom, how
  important, when to review), extending Event records.

## Direction (2026-10-03): personal first, hand-off ready; code is software heritage

Agreed: arv serves the owner first, and records cheaply whatever an archivist would need to
take the archive over later (docs/philosophy.md, "Plain text first..."). Its likely users keep
their own code, so it is also a small software heritage archive. Not started:

- [ ] **Notable objects.** Detect objects that are whole folders, or files an archivist must
      treat specially, and list them per disc in the catalogue (a record with path, kind and
      what to know), with a one-line summary in the disc's README.txt and index.html:
      git repositories (`.git`), WARC/ARC web archives, disc and VM images, mailboxes (mbox,
      Maildir, PST), photo libraries, nested BagIt bags, databases (SQLite), build trees
      (Yocto `build/`, `node_modules`). Per-file formats stay with Siegfried (formats.csv);
      this adds the folder level and the "this needs an expert" flag.
- [ ] **Is the history complete?** The commit log is the "why" of a codebase; without it an
      archivist pieces intent together from code alone. For each git repository, record what
      is and isn't there: commits and branches, shallow clone (history starts at X), submodules
      (their history is elsewhere), Git LFS (pointers only, objects missing), uncommitted
      changes and stashes, the remotes it came from. Warn at `arv make` when history is cut.
      Also note what git never holds: issues, pull request reviews and mailing-list threads
      live at the host (`origin: github.com/...: issues and reviews are not in git`); an export
      of them is a candidate preset, when someone needs one.
- [ ] **Software Heritage identifiers (SWHID).** Compute `swh:1:dir:` for the folder (and for
      each git repository's working tree) from data/ and the listing alone: the listing's kind
      and link target columns are exactly git's tree model. Checks a restore, matches a disc
      against Software Heritage or a git commit, and proves the listing keeps enough.
- [ ] **History:** a git bundle per repository (`git bundle verify`-able, one file) as an
      option, when someone needs it.
- [ ] **Dependencies:** referenced by identifier (SWHID, URL + checksum) rather than copied,
      with "keep a copy anyway" for those at risk; recorded as appraisals in the archivist log.
- [ ] **Restoring trees with links:** `--links keep` (note every internal link, copy none) and
      `arv restore` (recreates links; offers to repoint absolute links at the new location, from
      the source path recorded at ingest, private like any path), hard links stored once. What
      corporations do instead: backup tools keep links as links and expect restore to the same
      path; Yocto shops archive inputs (layers, download mirror, build container), not `tmp/`.

## Direction: a chain of small programs, each carried on every disc (2026-10-01)

*Revised 2026-10-03: the steps stay, but as libraries linked into one `arv` program rather than
separate programs run in order (see "one codebase" above); the reasons below for each step hold.*

The end state is a series of programs run in order, every one of them on every disc, so a
disc can be read, checked, repaired and searched with what is on it. Each program is small,
does one step, reads and writes plain files, and has a written format behind it. On the disc
each one travels as **source**, plus **static binaries** for the common platforms (zig cc builds
Linux x86_64/arm64/armv7/riscv64, macOS and Windows from one machine; tested 2026-10-01), plus
a **`.wasm`** build for anything else (any WASI runtime). Python glue stays while the workflow
settles; settled steps move to C.

Making a disc (`arv make` runs these in order):

| # | Step | Now | Later |
|---|---|---|---|
| 1 | Scan, hash, check names | Python (`arv`) | C, once settled |
| 2 | Describe: vocabulary, tags, catalogue snapshot | Python | Python (optional local LLM) |
| 3 | Bag (BagIt) | Python | C |
| 4 | Image: hybrid ISO9660/UDF 1.02, or UDF 2.50 | genisoimage, or our udfmake (C, also `.wasm`) | udfmake |
| 5 | Add RS03 | dvdisaster Light (recommended) or speed47: byte-identical output (tested) | librs03, if dvdisaster Light splits into libraries ([issue](https://github.com/teaching-droid/dvdisaster-light/issues/1)) |
| 6 | Verify the image | dvdisaster `-t` | same |
| 7 | Burn and record | any burner; `arv burned` | a safe-burning note (xorriso) |

Reading, checking and repairing (what a disc must carry for itself):

| # | Step | Now | Gap |
|---|---|---|---|
| 1 | Read a damaged disc to an image | dvdisaster `-r` (`--ignore-iso-size` if the RS03 header is unreadable); Light `-r --rescue --mapfile` | needs a real drive: native binaries only (SCSI), no `.wasm` |
| 2 | Combine two damaged copies | read copy B into copy A's image (stock dvdisaster reads only what is missing; tested) | a documented procedure in README.txt |
| 3 | Repair the image | dvdisaster `-f` | a small portable RS03 decoder (C → `.wasm`) that, unlike lcsas-ecc, survives damage to the CRC/ECC sectors and the header |
| 4 | Check the files | `sha256sum -c`, `tools/bagit.py` | a tiny C `sha256` checker for the `.wasm` set |
| 5 | Get files out without mounting | OS mount, or 7-Zip | a userspace reader for UDF 2.50 (and Rock Ridge/Joliet for hybrid discs) |
| 6 | Search the archive | `arv --home catalog find` (Python) | — |

Details and measurements: research-notes.md, sections 7-9.

## Design (2026-10-04, agreed in outline): collections, editions and the workflow folder

Not implemented. Supersedes "changed files go on the new disc whole" in the history-graph design
below: every edition is a set of discs made together. The rest of that design (node hashes, a log
on every disc, appraisals) carries over.

**Two kinds of folder.** A *tracked* folder (the NAS, loose drives) holds data objects that stay
where they are; arv only knows which of its files are on which disc, by SHA-256. A *workflow*
folder is where a person sorts and organises something into shape and builds discs from it: one
**collection**, with one history log, shared by every disc made from it.

| Term | What it is |
|---|---|
| collection | the thing being kept, e.g. FAMILY: one workflow folder, one code, one log; its settings (title, set, categories, access) are given once, so `arv make` asks nothing |
| edition | a **set of discs** made together from the collection as it was on a date, numbered 1, 2, 3 ...; each disc holds a selection of it and a copy of the catalogue; with as many copies as its owner makes; `Keep` for one never to retire |
| volume (disc) | as now: one bag, one image; belongs to exactly one edition |
| binding | where an edition's volumes sit: Blu-ray images, and later the same bags as a folder or a zip on the NAS (the binding table above) |

Example:

```
FAMILY  (one workflow folder, one log)
  edition 1  2024-03  FAMILY-01               BD25, 18 GB    1 disc
  edition 2  2024-11  FAMILY-02               BD25, 20 GB    1 disc         replaces edition 1
  edition 3  2025-06  FAMILY-03 + FAMILY-04   2x BD25        2 discs + NAS  replaces edition 2
  edition 4  2026-01  FAMILY-05 + FAMILY-06   2x BD100, kept 2 copies + NAS replaces 1-3 (4 is never retired)
```

**Why a set of discs, not a chain.** For a power user, not an institution: the discs of an
edition together hold what was archived, so there is no chain to replay and no disc that is
useless alone (each carries a copy of the catalogue). It costs discs; provisional discs are
cheap BD-R, and they are retired.

**Lifecycle**
1. `arv collection init FAMILY --title "Family photos" [--set PHOTO] [--access private]` in the
   workflow folder writes a marker (`.arv`: the pointer file, with a `Collection:` line). The
   marker never goes on a disc, and arv writes nothing else in the folder.
2. Sort and organise freely. `arv status` in the folder: what changed since the last edition
   (added, changed, removed, renamed: by SHA-256), the size, and the media it fits.
3. `arv make` in the folder: the next edition, provisional by default, a set of discs, split as
   needed. `--final [--medium bd100]` makes the final one.
4. Burn and check each disc (`arv burn`, see the workflow review). An edition is *safe* once
   every volume has a copy that passed its read-back.
5. **Retiring.** When an edition is safe, the provisional editions before it are *replaced*:
   `arv status` says so, and `arv retire` records it (an event; the discs leave every location).
   Before retiring, it lists files that exist only on the discs being retired (removed from the
   collection since), so nothing goes by accident. The person decides; arv never deletes.
6. After a final edition, changes start a new provisional run (edition 5, 6 ... then final).
   Only two stages: no provisional-on-provisional chains, so the log stays readable.
7. **Another medium.** The same edition can also be bound as a folder or zip on the NAS
   (`arv make --binding folder DEST`): the same bags, so the same manifests and hashes, and it
   counts as a copy in copy health ("final edition: 1 disc copy + 1 NAS copy").

**Git's model, for any folder: a UUID for the lineage, hashes for each state.** Three
identifiers, one job each:

| Identifier | Says | Git's equivalent |
|---|---|---|
| collection `Uuid` (v4, made by `arv collection init`) | *which thing*: the same collection, however renamed, moved or changed | (none: git infers it from the root commit) |
| `Tree` (SHA-256 of the sorted manifest: path and SHA-256 of every file) | *which exact contents*, wherever they are | tree hash |
| `Node` (SHA-256 of Tree, Parent(s), Date, Message) | *which state in the history* | commit hash |
| disc id and image `Uuid` (as now) | *which physical image*; copies share it | (none) |

- The UUID is in the folder's marker and the catalogue, never in the payload: a folder copied,
  moved or restored keeps it (`arv restore` writes the marker back), so it continues the same
  lineage.
- **Revisions are commits; editions are revisions that became discs.** `arv checkpoint` (or
  `arv status --record`) logs the folder's state with no content stored: the NAS holds the
  content, the log holds hashes. (Not "snapshot": that word already means a disc's copy of the
  catalogue.) It costs a manifest (about 100 bytes a file) in the home
  catalogue, and gives `arv log`, `arv diff A B` and a fixity history between burns. `arv make`
  records a revision and turns it into an edition.
- **Forks**: a folder copied to two places carries the same UUID. When both record revisions,
  the history branches, as in git: `arv status` says "FAMILY has two heads: /nas/family and
  ~/family" and the person merges by hand (or `arv collection fork` gives one a new UUID).
- Identical contents are found across collections by `Tree` alone, without comparing files.

**The log** is `Revision` records in `archive.rec`, so every disc carries the whole history in
its catalogue copy (limited by access, as now); each revision's manifest is in the home
catalogue, and an edition's is on its discs:

```
%rec: Revision
%key: Node
Node: 3f9a...                      SHA-256 of Tree, Parent, Date, Message
Collection: 0b6c2f1e-...           the collection's Uuid
Tree: 81d0...                      SHA-256 of the sorted manifest
Parent: c47e...                    (two for a merge)
Date: 2026-01-10
Stage: final                       checkpoint (no discs) | provisional | final
Edition: 4                         revisions that became discs are numbered
Volume: FAMILY-05_2001-2025_X
Volume: FAMILY-06_2001-2025_Q
Changes: +312 ~4 -17 files since the parent
Message: the 2025 sort, final
```

`Retired` is an event on a volume (with a reason: `replaced by edition 4`); a `Collection`
record holds the code, Uuid, title, description and defaults.

**Git repositories in a collection.** A repository already keeps its own provisional history
(commits); a disc is a snapshot of the working tree **and all history up to that point**, so a
disc holds the repository's whole history. Any folder in the collection with a `.git` is handled
this way:
- **The working tree as plain files**, as now: readable with no git at all, uncommitted changes
  included (and flagged, as arv flags its own `+uncommitted`).
- **The history as a compacted `.git` in the same place**: `HEAD`, `config`, `packed-refs`, the
  index and one pack (`git repack -a -d` on a temporary copy; the source is never touched). The
  restored folder is a working repository with a clean `git status`, and nothing needs
  reassembling. Commit hashes are unchanged.
- **Left out**: hooks (code that would run on restore), reflogs, and credentials embedded in
  remote URLs. Kept: every branch, tag and the stash. All of it noted in the ingestion event
  (`git: 3 branches, 12 tags, history full`), as links are.
- **Trimming history to fit**: `--git-history since DATE` (per collection) makes the pack
  shallow: git's own `shallow` file marks the boundary, the repository still opens, clones and
  logs back to that date, and the hashes still match the full history elsewhere. Default: full;
  `arv make` offers the trim only when an edition does not fit, and shows what it would drop.
- **Dropped history is tracked like removed files**: the Edition record names each repository's
  heads and its boundary commits, and `arv retire` refuses to quietly retire the last safe
  edition holding commits older than a later edition's boundary (it lists them; the person
  decides).
- `arv status` reports per repository: new commits since the last edition, uncommitted changes,
  and the size of ignored files (`node_modules/`, build output) with `--skip-ignored` to leave
  them out (default: keep everything, as for any folder).
- **Recognising a repository by its history, not its path.** For each repository on a disc the
  catalogue keeps its root commit(s), its heads, and the list of commits it holds
  (`catalog/volumes/<id>/git-commits.txt`: repository path, then one hash per line; about 41
  bytes a commit, so 10,000 commits is 400 KB, kept beside the listing). Then, for any
  repository on the NAS or a laptop, `arv status` answers from the home catalogue alone, with
  no disc in the drive:
  - **same project**: its root commit is on a disc, whatever the folder is called now or
    wherever it was cloned;
  - **already archived**: its HEAD is in a disc's commit list (it is that disc's state, or older);
  - **ahead**: an archived head is an ancestor of its HEAD (`git merge-base --is-ancestor`):
    "14 commits newer than PROJ-02";
  - **diverged**: both have commits the other lacks (a rebase, or work on two machines):
    "3 commits on no disc";
  - **not archived**: no root commit matches.
  `arv find <commit>` names the discs holding a commit. Repositories nested in another's tree
  (submodules, or a clone inside a folder) are checked the same way, each on its own.
  History rewritten from the root (filter-repo) matches nothing; that is the honest answer.
- arv runs the `git` program for all of this and never reads git's files itself; without git
  on PATH it copies `.git` as is (today's behaviour) and says so in the event.
- Submodules and worktrees (a `.git` file pointing elsewhere): archive the repository it names.
- Not handled: Git LFS. Left until someone actually needs it.

**Each home is an archive with its own identity** (2026-10-05): a `Home` record (Uuid, Name, Date)
heads `archive.rec`; discs name it (`HomeUuid`); archives are separate privacy spheres, and
`arv rebuild` keeps them apart (`--any-archive` to merge on purpose).

**No provisional and final** (2026-10-05): an edition is an edition, a set of discs with as many
copies as its owner makes (one disc and one NAS copy, say). A newer safe edition replaces older
ones, except those kept (`arv make --keep`, `arv collection keep CODE N`).

**Decisions this needs**
- **One workflow folder, one collection** (recommended). An inbox where things wait to be sorted
  is an ordinary folder arv does not know about; things move into a collection's folder when
  they are ready. Several collections in one folder can come later if needed.
- **Disc ids**: the collection code becomes the id prefix (`FAMILY-05_...`); the sequence runs
  across editions and is never reused. Set and Category become classification only. A disc
  made outside any collection (`arv make FOLDER`, as today) is a collection of one edition,
  coded by its set as now, so today's discs need no change.
- **A name clash**: at this design stage, `Collection` meant virtual folders across discs ("Best
  of Kyoto"). They were renamed `Selection` in format 0.5 so "collection" could mean the thing
  in a workflow folder; issue #42 later removes those records (see the decision below).
- **Tracked folders** get `arv status PATH` too: for each file, archived on which discs, or
  not yet; the first part of the organiser below.
- **Recognising a plain folder (no git) as the same but changed**, strongest evidence first:
  1. *Declared*: a workflow folder's marker carries its collection's Uuid. Exact; no guessing,
     whatever happened to the folder's name, place or contents.
  2. *Content*: otherwise the folder's files are matched against the discs' manifests by
     SHA-256. Each file is **unchanged** (same path, same hash), **changed** (same path, new
     hash), **moved** (same hash, new path), **new** or **removed**. The folder as a whole is
     reported against its best-matching discs with the numbers, never a silent verdict:
     "probably TRIP-01_2019_4: 92% of its files here, 40 new, 3 changed, 1 moved, 2 removed".
     Shapes: identical; grown (the disc's files are a subset); partial (the folder is a
     subset of a disc); changed (they share at least half); unrelated.
  3. *Where it was*: `Source` (host and path) recorded at `arv make`, home catalogue only (it
     names your machine and folders, so it never goes on a disc): a hint for the report, and
     a fallback when everything changed but the place.
  - **Only three ways make a link** (decided 2026-10-05): the `.arv` marker; the person saying
    so (`arv link FOLDER COLLECTION|DISC-ID`, nothing written in the folder, recorded with the
    folder's path); and, for a git repository, its root commit. Content and place only
    *suggest* a link; the person confirms it, and the confirmation is a declaration. Name
    conventions, a chosen file or a pattern in a file are not used: each is one more rule to
    remember, and a declaration covers the same need.
  - **Every link is logged**: an `Event` with `Type: identification`, the folder, the
    collection or disc, `How: marker | declared | git-root | content-confirmed`, the evidence
    ("92% of TRIP-01_2019_4's files, 40 new"), and the agent (`human:` or `arv`). The same
    goes for unlinking, a fork, and "this folder is now past": `arv link --past` records that a
    folder is an older state kept for reference, so `arv status` stops proposing it for discs.
  - **Cheap enough for a NAS**: a file whose path, size and modified time match the disc's
    listing is taken as unchanged without reading it (as rsync and git do); only the rest are
    hashed. Hashes are cached in `<home>/cache/hashes` (path, size, mtime, inode → SHA-256),
    so a second `arv status` reads almost nothing. `--deep` rehashes everything (bit rot on
    the NAS shows up as "changed" with an unchanged modified time: reported as such).
  - Expected noise, reported not hidden: photo tools writing tags into JPEGs change their
    hashes (changed, same path); an edited file that was also renamed is new + removed.

**Steps**
- [x] Format 0.5: `Collection` (with its Uuid), `Revision` (Tree, Node, editions), `Selection`;
      the spec and reference outputs (2026-10-05; the `Retired` event comes with `arv retire`).
      No compatibility with earlier drafts: nothing has been burned yet.
- [x] `arv checkpoint`, `arv log`, `arv diff`, and the hash cache they share with `arv status`
      (2026-10-05; `arv make` fills the cache for a collection's files).
- [x] `arv collection init|list|show`, the marker, and `arv make` taking its settings from it and
      recording each edition (2026-10-05).
- [x] `arv status` (a workflow folder: changes since the last revision; any folder: archived or
      not, and the discs it most resembles) and `arv link` (2026-10-05).
- [x] No `arv burn` (decided 2026-10-05: arv never drives the burner; it prints the command).
      `arv burned --device` reads a copy back and records it only if identical; edition safety;
      `arv todo`.
- [x] `arv retire`, with the list of files only on the retiring discs (2026-10-05).
- [x] Git repositories (2026-10-05): the compacted `.git` (one pack, refs also loose, no hooks,
      reflogs but the stash's, or URL credentials), `--git-since DATE` (shallow; every ref's tip
      kept), `git.tsv` per disc, `arv status` per repository (archived, ahead of a named ref,
      diverged, not archived, uncommitted), `arv find COMMIT`. Revision manifests leave `.git`
      out (a repository's state is its commits). Without git on PATH, `.git` is copied as is.
- [ ] `arv retire`: also list commits found only on the retiring discs (git.tsv), as for files.
- [x] Copies on a drive or NAS (2026-10-05): `arv stored DISC-ID PATH` checks and records the image
      as a file (`Form: iso`) or the disc's files as a folder (`Form: folder`), warm by default; arv
      copies nothing itself. Every copy has a temperature (hot: in use; warm: online, left alone;
      cold: offline).
- [ ] Zip copies (one sealed file, store mode), when someone needs them.
- [x] docs/workflow.md rewritten around: init, collection, status, make, burned / stored, todo, retire (2026-10-05).

## Design: discs as nodes in a history graph (2026-10-02, not implemented)

Git's model, with the content kept where it already is. Each disc image is a **node** (a
commit): it holds new or changed files whole, plus a log of every earlier node.

| git | Here |
|---|---|
| commit hash | node hash: SHA-256 of the disc's `tagmanifest-sha256.txt`, which already lists the hashes of every manifest and catalogue file; computed before RS03, so error correction does not change it |
| parents | the newest node(s) when the disc is made; a DAG (split discs share a parent, histories can merge) |
| `git log` | `catalog/history.rec` on every disc: each earlier node's hash, parents, disc id, date, message |
| objects | files, stored whole on discs (and on the NAS) |
| working tree | the NAS (everyday storage) |
| `.git` | the home catalogue: history log, manifests, appraisals; **no file content and no diffs** (content lives on the discs and backups) |

- **A lost disc does not break the chain:** later discs carry its node hash, parents and full
  manifest, so what was on it is known exactly; a copy found later is proven by re-hashing.
- **Tamper-evident:** changing a file on an old disc changes its node hash, which no longer
  matches the log on newer discs.
- **Versioned collections:** a collection snapshot maps logical paths to `disc:path` + SHA-256
  as of a node, with its previous snapshot as parent. Changed files go on the new disc whole
  (`Supersedes: OLD-01_...:path`); unchanged ones are referenced. Each disc still stands alone;
  only the snapshot spans discs, as collections already do.
- **Appraisals** (`Appraisal` records: target, for whom, importance word, note, by
  `human:`/`bot:` with basis, date, review) attach to nodes, paths or collections, cascade from
  set to file, and are appended, never edited. A person's appraisal outranks a bot's.
- **A git-like CLI:** `archive status` (NAS vs. manifests by hash), `arv make` (the next
  node), `archive log`, `archive show NODE`, `archive diff A B`, `archive verify-chain`.
- **Not a git backend.** History is kept per disc, not per edit: each disc's catalogue snapshot
  is the whole archive's state at that node, so changes between two discs are a diff of their
  snapshots (`archive diff A B`). Events and appraisals carry their own dates. No `.git` or git
  bundle of the catalogue goes on a disc, and the tool does not need git; git's model is
  borrowed, not its software.

## Later: an archive organiser (2026-10-02)

Not everything fits on Blu-ray, and not everything needs to (philosophy.md). The NAS and
loose drives hold everything; discs hold a curated selection. An organiser would propose
what is worth a disc:
- [ ] rank folders on everyday storage by importance and audience (rules, tags, collections,
      optionally a local model), and show why;
- [ ] skip what is already archived (by SHA-256 against the catalogue manifests);
- [ ] fill discs with the most important material first, by set;
- [ ] report copy health: discs with fewer than two copies, or all copies at one site
      (git-annex's `numcopies`, per disc rather than per file);
- [ ] report duplicates: at `arv make`, list files whose SHA-256 is already on another disc,
      and on request, which files are on several discs. Report only, never deduplicate: every disc
      stands alone, so a file archived twice stays on both discs unless the person drops it.

The tool proposes; the person decides.

## Separate track: standalone RS03 library

Neither dvdisaster nor the speed47 fork has a library or API; it is one GPLv3 C
program. The RS03 on-disc format is effectively frozen, so a library is
practical: extract the RS03 encoder/decoder from speed47's `src/`, keep
bit-for-bit compatibility (tested against the dvdisaster CLI), and expose
create/verify/repair. The result stays GPLv3, which constrains the licence
of anything linking to it (this repo's licence is still undecided).

Found 2026-10-01, both worth starting from:
- [dvdisaster-light](https://github.com/teaching-droid/dvdisaster-light): RS03-only, CLI-only
  fork, bit-identical to 0.79.10-pl6, with a `ddrescue`-format map file and a read-until-complete
  `--rescue` mode. GPLv3. Tested (section 8): byte-identical to speed47 on all samples including
  custom `-n` sizes, same repair results; a drop-in replacement for `arv make` and recovery,
  now the recommended build. Proposed upstream (2026-10-01): split it into an RS03 codec library
  and a drive-reading library with the CLI on top, for GUIs and automation such as a jukebox:
  https://github.com/teaching-droid/dvdisaster-light/issues/1
- `lcsas-ecc` in [LCSAS](https://github.com/mikmorg/lcsas) (`recovery/src/lcsas-ecc/`, plus
  `docs/DVDISASTER_RS03_FORMAT.md`, a written RS03 spec): a 1,500-line C89 RS03
  verify/repair/augment tool, stdio only. Tested on our sample discs (research-notes.md, section
  8): it repairs unreadable *data* sectors up to the RS03 limit, natively and as a WASI `.wasm`,
  but fails whenever RS03's own header, CRC or ECC sectors are damaged, holds the whole image in
  memory, and only augments to the standard media sizes. A reference and a last resort; our
  portable decoder must tolerate damage to RS03's own sectors. Licence unclear: LCSAS says MIT, but the code is
  "transcribed to match dvdisaster" (GPLv3); ask the author or treat it as GPLv3.

## Decisions (2026-10-01)

- ~~Browser search data stays as it is~~ (superseded the same day, below).
- **No search page on the disc.** `search.html` and its JavaScript copy of the catalogue
  (`catalog/web/`) are removed; `index.html` stays (static, no JavaScript). The bet is on
  two things outlasting any browser API: **Python 3**, and a **WebAssembly runtime that runs
  WASI command-line programs** (several independent ones exist; udfmake already builds as one,
  `make wasi`). Every disc carries the tool's source in `tools/`, so from the disc alone:
  `python3 tools/arv/arv --home catalog find PATTERN`. Convenient
  searching is catalogue software's job (Katalog). If a browser view is wanted later, it can
  be a WASI program that serves the catalogue over local http, not data duplicated on the disc.
- When the C pieces needed to read or repair a disc exist (verify, RS03 repair), each goes on
  the disc as source plus a `.wasm` build, so a WASI runtime alone is enough to run them.
- **README.txt on every disc carries the tested recovery steps:** the exact image size (so a
  read that lost the RS03 area is noticed; read again with `--ignore-iso-size`), dvdisaster
  Light's `--rescue`, and reading a second copy into the same image. It opens with a plain
  paragraph for whoever finds the disc (from LCSAS's START_HERE idea).
- **No PAR2.** Protection is RS03 over the whole image (which also covers the filesystem's own
  records, unlike file-level PAR2) plus **identical full-disc copies at different sites**. Copies
  are burned from one `.iso`, so they are sector-identical: read one with a map of bad sectors
  (`ddrescue`), fill the gaps from the other, and RS03 repairs what both lost. Parity spread
  across discs (PAR2 sets) only pays without whole-disc copies.
- **Encryption, when it comes (not now).** Superseded 2026-10-07: see "Direction (2026-10-07):
  locking, the data object as the unit" below; kept for the comparison. Reconsidered 2026-10-05 as optional per-item *locking*
  with key custody in the archive's record (docs/philosophy.md, "Locked, but never lost";
  issue #21); the whole-payload direction below stays as the alternative for sealed discs.
  Earlier direction: sealed discs only, opt-in. The disc stays a normal
  disc (bag, README, `catalog.rec` and tools in the clear); only the payload is encrypted, as one
  piece, so no file names leak. Two candidates, chosen at implementation time:
  - **age over a filesystem image** (`data.img.age`). age has a small published spec, several
    independent implementations and is in every major distribution. The tool does it all
    (`age -r ... < image > image.age`). Encrypt to two recipients: your key and a rescue key
    printed on paper with the estate papers (age cannot mix a passphrase with key recipients in
    one file). Inner image: SquashFS (compressed, very mature, 7-Zip reads it) or a UDF image from
    our udfmake (Windows and macOS mount it natively; one filesystem format across the project).
    Costs: age stops at the first damaged 64 KiB chunk, and decrypting needs scratch space the
    size of the image (no random access).
  - **VeraCrypt container, bring your own** (the ovenmitts model): you make the container in
    VeraCrypt; the tool only burns it and writes the read-only mount command
    (`veracrypt --text --mount-options ro ...`) into README.txt. Sectors are encrypted
    independently (XTS), so damage stays local; it mounts in place; Linux's `cryptsetup --type
    tcrypt --veracrypt` opens it too (a second, in-kernel implementation). Costs: one password
    rather than several recipients, and VeraCrypt itself is a heavier install outside Debian/Ubuntu.
    Carry over ovenmitts's rules: refuse a container that is currently mounted
    (`veracrypt --text --list`); a **fresh container per archive generation** (diverged copies of
    one container share a master key, permanently on write-once media); keep an **external backup
    of the volume header**, its single point of total failure.
  - **Reminder: error correction is already handled below this layer.** RS03 over the whole
    image plus identical copies at other sites repair the ciphertext before anything is
    decrypted. So the encryption layer does not need to tolerate damage itself, and the choice
    should favour **simplicity of implementation** over self-healing features. VeraCrypt's local
    damage is a bonus, not a requirement, which leans towards age.

## Direction (2026-10-07): locking, the data object as the unit (early days)

Not scheduled; this records where the thinking stands (issues #21 and #13, #22 folded in).

**What practitioners say** (sources checked 2026-10-07):
- The preservation community's default is not to encrypt archival copies: encryption "should be
  avoided if possible for archival copies", and lost keys make data inaccessible
  ([DPC handbook](https://www.dpconline.org/handbook/technical-solutions-and-tools/information-security));
  long-term content must not be protected in ways that stop custodians copying and migrating it
  ([LoC, sustainability factors](https://www.loc.gov/preservation/digital/formats/sustain/sustain.shtml)).
  Data hoarders who do encrypt name forgotten passphrases, not broken crypto, as the risk.
- Security engineers do recommend encrypting offline backups, and age over PGP for files
  ([Latacora](https://www.latacora.com/blog/2019/07/16/the-pgp-problem/)). OpenPGP has split in
  two (LibrePGP in GnuPG, RFC 9580 at the IETF) with newer formats that do not interoperate
  ([LWN](https://lwn.net/Articles/953797/)): not the stable target it looks like.
- age: a published spec ([C2SP](https://github.com/C2SP/C2SP/blob/main/age.md)), interoperable
  implementations in Go, Rust and TypeScript. Its 64 KiB chunks can be seeked by the spec, and the
  Go library has seeking decryption since v1.3.0 (`DecryptReaderAt`); `rage-mount` mounts only
  age-encrypted tar and zip. v1.3.0 also adds hybrid post-quantum recipients.
- Symmetric encryption is not significantly weakened by quantum computers
  ([NCSC](https://www.ncsc.gov.uk/whitepaper/next-steps-preparing-for-post-quantum-cryptography));
  public-key recipients (X25519) are exposed to "record now, decrypt later".
- Failures are about keys and tools, not ciphers: TrueCrypt ended in 2014 and VeraCrypt dropped
  its format about ten years later (cryptsetup still opens it); an overwritten header is the most
  common way LUKS volumes are lost (cryptsetup FAQ). Succession practice: split the passphrase,
  not the data, with Shamir shares (`ssss`, Paperback), on paper, and test that the shares
  decrypt ([an example](https://sprocketsecurity.com/blog/how-to-securely-share-your-backups-and-passwords-upon-your-death)).
- **The one active project with the same shape:** [brb](https://github.com/jzbz/brb) (research-notes.md,
  section on similar projects): SquashFS per disc, age, PAR2 over the ciphertext, ISO to BD-R. Its
  restore decrypts the whole image, and its path index is encrypted too. Worth watching, and
  comparing with once arv builds anything here.

**Direction:**
- **The unit is the data object**, not the file and not, by default, the disc. A locked object
  goes on a disc as one SquashFS image encrypted with age, under a name made from its id; its
  `Object` record says it is locked and which key opens it. Getting one thing back decrypts only
  that object (often a few GB), so no seeking reader is needed. Plain and locked objects can
  share a disc. Per file is rejected: it leaks counts, sizes and structure for nothing.
- **A sealed disc is the special case** where every object is locked and the disc's own catalogue
  is locked too (a small age file); other discs carry only its id and place, as now. One mechanism
  covers #21 (locking) and #13 (sealed discs). What a mixed disc gives away: that a locked object
  of about a given size exists.
- **The envelope stays plain:** README, tools, the disc id, how to open it, and RS03 over the
  whole image, which repairs the ciphertext before anything is decrypted (brb does the same with
  PAR2).
- **One archive passphrase by default** (age's scrypt stanza; symmetric, so no quantum exposure),
  not a key per object: more keys is more of the risk the sources warn about. Kept on paper and
  split with Shamir shares; arv would require a test decryption before a locked disc is made.
  Keys per audience (family, heirs) only later, if wanted.
- **No PGP; no cryptography of ours.** The disc carries age's spec and a decryptor's source in
  the clear, and at least two independent tools must be able to open what arv writes.
- **Locking stays opt-in, with the warning in README.txt:** a locked object lasts only as long as
  its key arrangements.

Open: whether the inner image is SquashFS (compact; 7-Zip and unsquashfs read it) or UDF (native
mounts); how `arv verify` and `arv check` treat a locked object (the manifest of the ciphertext,
and the plaintext manifest inside the lock); the custody record (#21).

## Decision (2026-10-09): no fallback home; a `.arv` says which catalogue, not what is tracked

- **Before:** with no `.arv` here or above and no registered default, arv started an archive in
  `~/.local/share/arv` without a word. In a first-hour test (research-notes.md, 10) `collection
  init` and `make` did exactly that: an archive nobody chose, in a folder nobody backs up, and a
  second one waiting to happen the first time arv runs outside the tree (half the discs in each,
  `todo` and `find` seeing half).
- **Now, as git does outside a repository:** no archive found means arv stops and says how to
  make one (`arv init FOLDER`; `--name NAME --default` to use it from anywhere). `home_try` is the
  lookup without the stop, for what needs no archive: `arv id`, `arv describe` on a folder, and
  reading a disc. The older `bluray-archive` folder and `$BLURAY_ARCHIVE_HOME` went too (nobody
  has arv yet).
- **Kept:** the registered default, because arv is often run from *outside* the archive (at the
  drive, a disc in hand: `arv burned`, `arv find`), unlike git, which is run inside the project.
  It is opt-in and visible (`arv where`), and deleting `homes.rec` loses nothing. Names
  (`--archive NAME`) stay either way.
- **Bubbles, not cascades:** the nearest `.arv` wins outright; nothing is inherited from one
  above. Archives are privacy spheres, so a sealed archive's records must never surface in
  another's. arv walks past `.git`, so one `.arv` covers many repositories.
- **A `.arv` decides which catalogue is used, never what is tracked.** Nothing scans the tree:
  only `collection init`, `make`, `plan add` and `checkpoint` record anything, and `status`,
  `objects` and `todo` look only at a folder named, or at the source paths already recorded.
  `~/.arv` with a home full of caches costs nothing.
- Open: dropping `Default:` for a pure "bubble only" model; what a writing command run from a
  disc root (found as a read-only catalogue) does.

## Decision (2026-10-09): each copy is a letter; images stay sector-identical

- **Why not a copy id inside each image:** different bytes per copy would end the best recovery
  there is, reading two damaged copies into one image with ddrescue and one map file, then
  repairing with RS03; it would also mean an image per copy (25 GB each) and no single hash to
  check every copy against. So the identity is outside the image: on the label and in the
  catalogue.
- **A copy is its `replication` event** (as the spec already said), now with `Copy: A`, `B` ...
  (the next free letter, or `--copy X`), written on the hub and case after the id. Each copy
  keeps its own `Location`, `MediaId` and `Bca`; checks name the copy they read; `todo` lists
  copies never read back by letter. `--copies N` writes N events.
- **No counter.** The disc's `Copies:` field (which `--copies N` raised by N while writing one
  event, so the two disagreed) is gone: copies are counted from events, which only grow, so two
  catalogues of one archive merge without losing any (next section).
- **The BCA serial** identifies a BD-R by itself where the drive reads it
  ([research/bca/](bca/README.md)): on Linux `arv burned --device` records it and refuses a disc
  already recorded, and `arv check --device` knows which copy it holds. The letter stays the
  name: an image file or folder has no BCA, other systems and USB bridges may not pass the
  command through, and a person decades from now reads the hub.
- Open: moving one copy (`locate --copy`), a copy lost or destroyed (an event naming it), and two
  forked catalogues both giving out the same letter (a merge should flag it, not combine them).

## Design (2026-10-09, not implemented): several catalogues of one archive

Nothing stops two catalogues sharing one archive id: copy a `.arv` folder, or `arv rebuild` from
a disc into a second place, and both take writes. Merging them back is `archive_merge` (what
`rebuild` uses).

- **Merges cleanly:** everything that only grows: events, revisions (by `Node`), data object
  versions and appraisals. A union, in any order.
- **Breaks, silently:**
  - *Numbers.* The next disc sequence and edition number come from the local catalogue
    (`archive_next_number`), so two forks each make `FAMILY-02_...`, each "edition 2": two discs,
    one id. The merge matches discs by id and keeps one record; the other's events attach to it.
  - *Counters.* `Copies:` was one (fixed: copies are events now, previous section).
  - *Fields that change* (a disc's places, notes, a location's parent): one side wins, by a flag
    (`--prefer-disc`), with no report that they differed.
- **What works today is one catalogue written, many read-only copies:** every disc carries a
  snapshot with the same archive id, but a snapshot never moves on by itself.

The way there, each step useful alone:

1. **"This is a copy" guard.** The catalogue records where it lives; a write to a copy found
   elsewhere stops ("this is a copy of the archive at /nas/.arv: use a pointer, or
   `arv init --move-here` if the old one is gone"). Moving becomes deliberate and logged; reading
   a copy (`find`, `list`) stays fine.
2. **Remotes you pull from:** `arv remote add NAME PATH` (same archive id only), `remote remove`,
   `remote -v`; `arv pull [NAME]` (the rebuild merge, the remote's fields winning); `arv clone PATH
   DEST` (origin set). The local copy stays read-only between pulls, by step 1. A disc is a remote
   too (`arv pull /media/disc` is `rebuild`). Remotes live in `config/remotes.rec`, which never
   goes on a disc (only `sets.rec` and `tags.rec` do), so local paths and host names do not leak.
   Paths only at first (a mounted share); ssh later through `rsync` or `scp`, as `make` runs git.
3. **Push, only when writing on two machines is needed:** numbers no two catalogues can both give
   out (a short prefix per catalogue, or a check against every catalogue seen), and conflicts on
   changing fields reported rather than resolved. Then merging is a union, and `push` is `pull`
   the other way.

## Decision (2026-10-10): keep the record, let other tools do the work

- arv is a command-line tool first and keeps to the Unix way. Its one job is **the record** of
  what is in the archive's care, made trustworthy by checking (read-back, hashes, verification).
  The work the record describes (burning, copying, imaging, listing, indexing) belongs to tools
  built for it, which arv already leans on: `burned` checks what a burner wrote, `stored` what
  cp or rsync copied, `check --repair` an image ddrescue read. Written into philosophy.md as
  principle 7.
- **The boundary is care, not maker:** an old disc you keep is the archive's from the day it is
  found (as an archivist records a box on arrival, before it is sorted); everyday storage is not,
  until something on it is taken in.
- **The test for anything new:** does it keep the record, or do the work? Where arv does work
  itself (writing the image: UDF, BagIt, RS03), it is because the image is the format, and those
  parts are programs of their own.
- Talked through first: a custody record that grew a scanner and an index (a listing of every
  old disc's files, made by arv) was dropped. Listing is `7z l`, `isoinfo -l` or fiwalk's job, and
  browsing is Katalog's; arv may later *read* a listing they made, never make one.

## Design (2026-10-10, not implemented): `arv found`, an old disc taken into the archive

Old data discs (CD-R and DVD-R from the 2000s, BD-R not made by arv) are often the only copy of
what is on them, and failing. Born-digital archives handle them in an order arv can follow:
**record the carrier, image it once, appraise and process later from the image**, never from
the original. arv does the first and keeps the record of the rest; imaging is ddrescue's.

```sh
ddrescue -b 2048 /dev/sr0 old-2004.iso old-2004.map      # the work: another tool's
arv found old-2004.iso --device /dev/sr0 --map old-2004.map --set OLD --title "Holiday 2004" \
          --location BOX2 --note "CD-R, marker on the label: Holiday 04"
arv plan add rescue old-2004.iso                         # later: the image onto an arv disc
```

- **What it reads, all read only:** the image's SHA-256 (one pass, as `stored` reads one); from a
  few sectors, the volume label and the file system's own creation date and size (ISO 9660's
  primary volume descriptor, or UDF's); from the drive, if given, the media id and the BCA
  serial (`drive.c`); from the ddrescue map, if given, whether the image is whole or how many
  bytes could not be read. No walk through the files, no look inside them.
- **What it records:** a `Disc` record with `Origin: found` (not made by arv), an id from its set
  with a check character (`OLD-03_2004_K`, written on the sleeve), the label, a title, the
  coverage from the file system date, the place, and `ImageSha256` of the image as read, so
  `arv check --device` can later read the old disc back against it and see it decay. The
  original disc is its copy A (`Form: disc`, with its `Bca`); the image file, if kept, its copy
  B (`Form: iso`, `Path`), as `stored` would record it. An event records the capture: the tool,
  the map's verdict, the note.
- **Rescue needs nothing new:** `arv plan add NAME old.iso` puts the image on an arv disc as a data
  object. Its `Tree` is the image's SHA-256, which is the found disc's `ImageSha256`, so arv
  knows by content alone which arv disc now holds the old disc, and `todo` can list found discs
  held on no arv disc yet ("not rescued"). Taking files out of the image into a collection is a
  later, separate step; the image, the disc as it was found, stays.
- **Snapshots** carry a found disc's identity record like any other disc's, and no file listing
  (it has none). A listing made by another tool (`7z l`, `isoinfo -l`, fiwalk's DFXML) may come
  later as `--listing FILE`, kept at home for `arv find`; arv never makes one.
- **Name:** in the Record group (it records, as `burned` and `stored` do). `arv found` today,
  `arv record found` if commands get group prefixes (open).
- Open: the PREMIS event type for the capture; whether a found disc that will not read at all
  (no image) is recorded by identity alone (`arv found --device DRIVE`, the BCA and label only);
  CD-R and DVD-R have no BCA readable this way, so they are told apart by id and label only.

## Later: an interface for other software (2026-10-10, after arv is stable)

Katalog and other archival software should not have to parse arv's text output, which is
written for people and may change any time. Not worth building until the commands settle (a
promise made now would freeze what is still moving), but the shape is clear:

- **Reading: the files are the interface,** and already are: the format spec, versioned and only
  ever added to, with "Reading a disc", the virtual-tree section and "Mapping to Katalog". The
  home catalogue (`.arv/catalog/`) is laid out as a disc's `catalog/`; its home-only parts (object
  sources and manifests, revision manifests, plans) need documenting in one place.
- **Writing: only through arv.** Nothing locks the catalogue, so another program writing
  `archive.rec` while arv runs can lose records. To be said in the spec ("For other software"),
  with documented exit codes (perhaps `sysexits.h`: 64 usage, 65 bad data ...).
- **Machine output: `--json` on the commands others call** (`list`, `find`, `todo`, `status`, `id`,
  `where`, and the results of `check`, `burned`, `stored`; `objects --json` and `plan show --json`
  exist). The stable "ABI" is a versioned contract, not a command id: every reply says
  `"format": "arv-json 1"`, and within 1 fields are only added, as on discs (COM's rule for its
  interface GUIDs and D-Bus's for `Name2`, with a readable number). As in FreeBSD's libxo, each
  command would build one record of its result and render it as text or JSON, so the two cannot
  drift; text stays free to read better. The GUI could then use these instead of reading the
  catalogue in Python itself.
- **Perhaps:** `arv --help --json` (every command, its options and its output format), so other
  software discovers what this arv offers; a CWL description of the main commands, for research
  workflow systems. Only if someone asks.
- Prior art: git `--porcelain`, libxo, jc (which exists because most tools have no machine
  output), `sysexits.h`, CWL.

## Later: catalogue snapshot size

Each disc carries every earlier disc's manifests, listings and format IDs:
about 430 bytes per file in the archive. Up to about a million files that is under 3% of a
25 GB disc, and nothing changes. When an archive heads past that (5 million files would be
12%), do the following, in this order:

- [ ] Leave other discs' format IDs (`volumes/*/formats.csv`, about half the size) out of
      snapshots; each disc keeps its own.
- [ ] Compress only the *other* discs' copies (`volumes/<id>/listing.tsv.gz`, `manifest.sha256.gz`,
      `formats.csv.gz`; about 28% of plain). This disc's own catalogue, manifests and listing
      stay plain, so it stays readable without tools. `rebuild`/`find` read `.gz`.
- [ ] Do it automatically, only when the snapshot would exceed about 2% of the disc's data
      budget, so small archives stay entirely plain.

## Open decisions

- Licence for this repo (GPLv3 fits if the RS03 library happens)
- **`archival-udf.md` as a standalone UDF reader's spec** (2026-10-10, a to-do). Today it is a
  profile: the rules on top of ECMA-167 and UDF 2.50, not the byte layout of their structures, so
  a reader cannot be written from it alone. It could be: describe only the structures arv writes
  (anchor, descriptor sequences, partition and metadata maps, file set, extended file entries,
  identifiers, `short_ad`/`long_ad`), and prove it complete with a reader written from it alone,
  as `dev-tools/rs03-spec-check.py` does for RS03. Less pressing now that discs can be read
  without any UDF reader (spec, "Extents"), with `udfwrite`'s source on every disc as the
  executable description.
- **What every disc, and the package, carry in `tools/`** (2026-10-10, issue #32). Decided: a
  whitelist, `disc-tools.txt`. The UDF and ECMA standards' zip (69.6 MB, 97% of the old tree, its
  licence and origin not written down) stays in the repository but off the discs and out of the
  package; discs carry our profile (`archival-udf.md`). *Settled 2026-10-15:* the profile points
  to the published standards (Ecma's own copy of ECMA-167; OSTA's UDF 2.50 through the Internet
  Archive, its site having closed; and Wikipedia's UDF article, which links the same collection the
  zip held, with a saved copy), and the zip leaves the repository too: its terms were never
  written down (a Wikipedia user's preservation of OSTA's and Ecma's PDFs), and arv needs only the
  two documents the profile names.
- **Editions under a distributed collection** (2026-10-06). The archive is the union of the
  workflow folder, warm images and cold discs; a disc holds a selection plus a catalogue copy
  ([concepts.md](../docs/concepts.md)). But "a newer safe edition replaces older ones" still
  assumes the workflow folder holds everything: anything moved out of it is then on retirable
  discs only. Decided 2026-10-06: `arv retire --yes` refuses while any file is only on the
  retiring discs; `--accept-loss` overrides, and records each such file as `Lost: SHA256  PATH`
  on the retired edition's revision (`arv log`, `arv find`; unioned on rebuild). Done.
- **Disc plan / mastering window** (built 2026-10-06). `arv plan new|list|show|add|move|drop|disc|make|delete`
  keeps `drafts/plans/NAME.rec` (a Plan record, Item records: Disc, Source, Path); it points at
  the sources, copies nothing, and `arv plan make` runs `arv make --plan FILE` (each entry's
  bytes read from its own source; one image a disc; no rebalancing). The GUI's Mastering tab
  drives it (drag and drop, a fill bar a disc). Made plans are kept as templates (2026-10-07):
  `list` hides them unless `--all`; `arv plan again NAME NEW` copies one into a new open plan
  (`From: NAME`); deleting one is always safe. Links stay the default (2026-10-07), with
  `plan add --copy` (the plan's own copy, `Origin:`), `Seen:` stamps and `plan refresh`, relative
  sources, a home beside arv (portable), and make checking bytes against the manifest as written.
  `--formats` for a plan: Siegfried on each item where it is (2026-10-08). `plan show --archived` marks what is
  already on discs, by hash (2026-10-08). Open: whether a plan can make an edition of a collection.
- **Data objects** (built 2026-10-06). Each plan item becomes an `Object` record (Uuid lineage,
  Version, Tree, Disc, Path, Kind); `Source` and the manifests (`catalog/objects/`) stay at home.
  Same Tree = same version; same Source, new Tree = next version; empty objects never match.
  `arv status` reports objects from a folder exactly, and a folder whose whole Tree matches an
  object by content; `arv find` lists them. `arv make FOLDER` records the folder as one object
  when it fits on one disc (2026-10-07). `arv retire DISC-ID` retires one disc of no edition,
  refusing the only copy of an object's newest version (and any file on no other disc) unless
  `--accept-loss` (2026-10-08).
- **The union view** (built 2026-10-06). `arv objects [NAME]`: per data object (each version)
  and per collection (its newest edition), the discs holding it, every copy of them (form,
  temperature, read back) and whether the original is still there. `arv todo` adds: a newest
  version with no cold copy; one no longer where it came from with fewer than two copies.
  Open: the same in the GUI; a hot original counted as a copy only once `arv status` has
  checked it unchanged.
- **Snapshot fallback.** `--snapshot full` is the default with no automatic step down when it does
  not fit; the intent is full where it fits, partial only when needed. What "partial" keeps (this
  set, this edition, recent discs) and how it treats access levels is undecided.
- ~~**Vocabularies on disc.**~~ Done 2026-10-07: full snapshots carry `config/sets.rec` and
  `config/tags.rec` in `catalog/config/`; `arv rebuild` restores them into a home that has none.

## Decision (2026-10-10): describing from anywhere, without a new record

Asked: can a person or a model going through the archive add descriptions, searchable tags and
links? Considered: an `Annotation` record (tags, notes and typed relations on any item, keyed by
content so it follows moved files), written in batches by `arv annotate`.

- **Most of it was there.** A disc's `Title`, `Description`, `Subject`s and `Note`s, folder tags
  and captions (`tags.tsv`), all searched by `arv find`; and a draft (JSON) that `arv describe
  --apply` records with an event naming the agent and its authorship. What was missing was the
  way in from outside, not a place to keep it.
- **Built instead:** the draft is documented as the interface (README, "Describing from
  anywhere"); `--apply -` reads it from standard input; `--suggested` keeps an unattended
  model's draft a suggestion (before, applying always made it `accepted` by a person, which is
  untrue for a script); folder tags merge into the disc's (applying one folder's used to drop the
  others'); a folder the disc does not have is skipped with a warning (a model can invent one).
- **Not built, and why:**
  - *Tags on single files.* Tens of thousands of rows in a catalogue every disc carries, for what
    photo tools already keep in the file (XMP, IPTC) and read there. arv describes the
    folder (an event, a trip, a project) and writes those tags into the files when asked
    (`arv keywords --format exiftool`).
  - *Typed links between items* ("scan of", "same event as"). Nothing reads them yet; selections
    group items, collections and data objects carry lineage. Worth a field when a reader needs
    `Relation` (Dublin Core) is the name to use then. Cross-disc virtual folders were removed by
    the later issue #42 decision below.
  - *Notes on a data object, following it across moves.* The one real gap; it waits until data
    objects are used enough to show whether disc and folder notes fall short.

## Decision (2026-10-10): history is per thing; events get ids; the discs are the witnesses

Talked through: how two catalogues of one archive stay in step, and how a change to the record
is noticed. This replaces "Remotes you pull from" and "Push" in the 2026-10-09 design above.

**The catalogue's history is not a line.** A history that is one line is a series of snapshots of
everything. A catalogue is facts about many separate things, and each has its own history:

| Thing | Its history | Shape |
|---|---|---|
| A disc | made, copies burned, checked, moved, retired | events, mostly added; their order barely matters |
| A data object | versions (`Uuid`, `Version`, each a `Tree` hash) | a line, which can fork |
| A collection | revisions (`Node`, `Parent`) | a graph |

Facts about different things do not depend on each other's order, so two catalogues that recorded
different discs, files or copies merge by keeping both. Order matters in two places only: a field
that changes on one thing (a title, an access level, a place), and numbers handed out (disc
sequences, versions), where the hash behind the label stays unique but the label can clash. A
whole-archive log is a view (sorted by date), not a record.

1. **Every event has an id derived from its content** (`EventId`: the first 32 hex digits of the
   SHA-256 of its fields as written, PREMIS's eventIdentifier). The same event has the same id in
   every catalogue and on every disc, so merging is exact (no matching by text), and an event
   changed after it was written no longer matches its id. Two events with the same content get a
   `Nonce` to keep them apart. Built 2026-10-10.
2. **A field change names the change it replaces** (`Sets: FIELD`, `Replaces: <EventId>`; built
   2026-10-11): each field gets its own small history. Two changes that replace the same one are a
   conflict, reported by rebuild and audit, never silently resolved; the home's value stands
   until the field is set again.
3. **Duplicate disc ids are reported at merge**, not merged; before burning, one side renames
   (built 2026-10-10).
4. **Moving catalogues is not arv's job:** rsync, a mounted share or any sync tool copies the
   folder; `arv rebuild` merges another catalogue by event id (built 2026-10-10: notes add up,
   a disc id naming two images is not merged, two copies lettered alike are reported). No
   remotes, clone or push. A copy is named in later events by its replication's EventId
   (`Replication:`), so `arv relabel` can give it a new letter without losing its history (built
   2026-10-11).
5. **A copy does not take writes:** `config/paths.rec` records where the catalogue lives, and a
   write to it found elsewhere stops, so a copied catalogue cannot quietly become a second one;
   `arv where --here` accepts a move. Built 2026-10-10.
6. **Copies are counted as physical discs** (built 2026-10-11). A copy's identity is what is on
   it, `Identifier: scheme:value` (PREMIS objectIdentifier): `bca:` the factory serial,
   `barcode:` a sticker, `label:` the pen, `hub:` the maker's batch code. Several per copy,
   gathered from every event that names it. `bca:` and `barcode:` name one disc, so two
   records sharing one are one disc recorded twice, counted once and reported; the letter stays
   the name people write and say. The image `Uuid` cannot do this: copies share it by design.
7. **Carrier and copy are two things** (built 2026-10-11). A disc holds one image, so it is the
   copy; a tape or a drive holds many, so its barcode names the medium, not a copy. A stored copy
   names its `Carrier`; copies are counted as independent copies (one per physical disc, one per
   carrier), and copies of one disc on one carrier count once. For jukeboxes and tape libraries
   (built 2026-10-11): `arv todo --tsv`, the worklist; `docs/examples/library-check.sh`, an
   `mtx` loop to adapt; and `arv inventory PLACE`, the library's barcodes against the record.
   Presence is reported, not recorded: a nightly inventory as events would bury the record, and
   where a copy should be is already in it (`Location`). Untested on a real library yet.

**Tampering: evident, not impossible.** A hash proves nothing unless someone holds an earlier one
elsewhere. arv has those holders already: every disc is write-once, dated, kept in several
places, and carries the whole catalogue as of its burning, and later discs carry the earlier
images' SHA-256. So:
- content-derived event ids (above) make an edited event detectable;
- **`arv audit`** checks the home's events against their ids, and against each disc's snapshot
  given to it: an event the disc has that the home lost or changed is reported. Built 2026-10-10.

Not done: one hash chain through the whole catalogue. It forces one order on per-thing histories
(every merge rewrites it), makes a hand edit look like tampering, and without copies held
elsewhere proves nothing the discs do not already. Nor a version-control system under the home:
the events are the history, the discs are the backups, and `rebuild` merges.

## Design (2026-10-12): data objects describe themselves; questions wait for an answer

Asked: should arv have a chat agent, and could data objects describe themselves (a git
repository from its own README and history)? Talked through:

- **No chat agent in arv.** `arv describe` already has the useful part: a model's suggestion,
  its questions, the owner's answers kept as notes. An agent that works through the whole archive
  is the "bring your own AI" tier, outside arv, writing drafts ("describing from anywhere");
  the local models that run well here (3B for one good round) are not reliable at multi-step tool
  use, and a chat loop would be a product of its own (principle 7).
- **The gap was the data objects.** A disc and its folders could be described, its objects not:
  an `Object` record had a name, a hash and a place, so five repositories on one disc showed as
  five names.

### Objects: described by rules, then by people or a model

- **Fields** (format 0.5, only added; all optional, as on a `Disc`): `Title`, `Description`,
  `Coverage` (EDTF), `Rights`, `Origin` (where it was published: a repository's `origin` remote
  as `host/path`, no scheme, user or `.git`; a local path is left out), `Note`*.
- **By rules, at `arv make`, no model** (authorship `automatic`, under the disc's creation event),
  from the object's own files, at its root:
  - `Title`: the README's first heading, else a package manifest's name;
  - `Description`: the manifest's one-line description, else the README's first paragraph of
    prose (badges, images, HTML and quoted warnings skipped; cut at a sentence, about 300 characters);
  - `Rights`: the manifest's licence, else a `LICENSE`/`COPYING` recognised from its first lines;
  - a git repository: `Coverage` from its first to its last commit (author dates, every ref),
    and `Origin`.
  Manifests read: `package.json`, `Cargo.toml`, `pyproject.toml`, Arduino's
  `library.properties`. A file object gets none of this.
- **Versions.** `Coverage`, `Origin` and `Rights` are facts of each version, read from it.
  `Title`, `Description` and `Note`s carry over to a new version once a person or a model has
  described the object (a `metadata modification` event, `Object: object:UUID`); until then
  each version reads its own. The same content again (a copy) has the version's fields.
- **By a person or a model:** `arv describe NAME|UUID` describes an object's newest version (its
  records on every disc), as a disc is described: interactively, `--save`, `--apply DRAFT`.
  Subjects and folder tags are a disc's and are left out. A disc's draft may carry
  `objects`: `{PATH: {title, description, notes}}`, PATH as the object's on that disc.
  The event names `Object: object:UUID`, and full snapshots carry it with the object, as they
  carry a place's.
- **Seen in:** `arv objects` (and `--json`, the GUI's Objects tab), `arv find` (titles and
  descriptions are searched), and the model's inventory: a disc's objects, and each git
  repository in a folder being described, as they describe themselves.
- **Privacy:** no author names or emails (the catalogue goes on every disc); `Origin` is the remote
  the disc's own `.git/config` already carries.

### Questions that wait

A model's questions were asked only at the terminal or in the GUI's panel, then lost: a draft
written unattended (`describe --save`, a script, a model elsewhere) had them, and applying it
dropped them.

- **Kept** in `<home>/drafts/questions.rec`, home only (a model's unanswered guesses are not
  archival): `Question` records, `About` (a disc id, or `object:UUID`), `Text`, `Asked`,
  `Agent`, and `Skipped: DATE` once skipped.
- **From:** a draft's `questions`, by `describe --apply` and by `make --draft` (for the disc it
  makes; the first, when split); and by rules, with no model: a git repository with no README or
  manifest gets "What is NAME for? It has no README to say."
- **Kept in check:** at most 5 open per disc or object; the same question twice (open, skipped, or
  answered in a `Note`) is not added again.
- **Shown** last in `arv todo`, one line per disc or object, and never counted as owed: a missing
  description is never as urgent as a missing copy. The GUI's Verify tab shows `todo`.
- **Answered** with `arv describe ID --answer` (no model needed): each open question in turn; an
  answer becomes a `Note` ("Q: ... / A: ..."), as in the interactive describe, with an event by the
  person; Enter skips it for good; `.` stops, leaving the rest open. Plain `arv describe ID` at a
  terminal asks them first, and the model then sees the answers with the rest of the record.
- **Later:** answering in the GUI's disc details.
- **Done (2026-10-12):** all of the above (`src/arv/about.c`, `src/arv/questions.c`,
  `arv describe NAME`, `--answer`); the GUI's Owed count leaves the questions out.

## Decision (2026-10-13): other software reads the disc through its standards, not through arv

Which software, other than Katalog, could use arv's metadata? Most of it needs no work from arv,
because the disc is built from standards. What a reader needs is a note on how to use them.

- **Done:** application notes, `docs/notes/bagit.md` (bagit-python, which every disc already
  carries, validates the disc as it is; tested, including a changed byte and a longer file) and
  `docs/notes/recutils.md` (recsel queries, all run against a real catalogue). The disc's
  `README.txt` names both.
- **Done:** the descriptors declare the fields that name another record (`rec` types), so
  `recsel -j` joins a disc to its place, a copy to its disc, and a place to its parent. A join
  drops a record whose value names nothing (free-text places); the note says so.
- **Worth targeting later, not built:** Archivematica takes an unzipped bag as a transfer, so a
  disc can go into an institutional archive as it is (untested); VVV, another disc cataloguer,
  would need the same import as Katalog.
- **Not targeted:** DROID and Siegfried make metadata rather than read it; Recoll, plocate and
  FSearch index files, and `arv find` already searches every listing (a tree of stub files for
  FSearch was considered and not built: it would be a second copy of the catalogue to keep in
  step); backup software keeps its own catalogue.
- **What helps all of them** is the stable machine-readable output put off until the commands
  settle ("Later: an interface for other software"): every one of these reads files or JSON,
  not arv's text for people.

## Decision (2026-10-13, issue #42): no cross-disc selection records

Approved: remove `arv selection` and the `Selection` record type. Catalogue snapshots and rebuilds
carry and merge no such records or their events. Appraisals target discs, files, folders and sets.
Places belong to copies only: `Disc.Location` is removed, `arv locate DISC-ID COPY PLACE` appends
a copy-specific `Location` change, and disc summaries and place filters derive from recorded copies.
`burned` and `stored` record a copy's initial place; `make --location` is not accepted. `Location`
records remain. There are no discs to migrate before 1.0.

## Decision (2026-10-14, issue #42): the clarity round, part 1

What a review of every command found to cost more than it gives, removed (the list and the
evidence for each item: issue #42). The test for each was principle 7, then use: is it in the
workflow, is it tested, does another command or tool already do it.

- **Removed:** the `make` options for other tools' work (`--ro-crate`, `--tools-history`,
  `--git-since`, `--extra-tools`, `--sf-home`) or that hid problems (`--ignore-names`,
  `--no-verify`) or guessed (`--media`); `plan refresh`, `plan add --copy`, `restore --no-links`,
  `verify files`, `tags --vocab`, `keywords --format tsv`, `init --pointer`; the weak copy
  identifiers `label:` and `hub:`; `arv models` (downloading models and building llama.cpp is a
  package manager's work); the GUI's own reader of the catalogue; the code reading old formats.
- **Kept, though suggested:** the UDF/ECMA standards zip and `upstream/` stay in the repository (off
  the discs) for reference and for the work offered upstream.
- **Found on the way:** the reference homes had no event ids, so with the old-format code gone their
  copies could not be named; they now carry ids (and temperatures), checked by `arv audit`. The fake
  embeddings server in the checks makes bag-of-words vectors, as the fake llama-embedding did, so
  the test of remembered tags means something.
- **Next (part 2):** one way to do each thing: `arv check SOURCE` for every kind of copy; `id`, `info`
  and `ls` as `info`; one spelling for recording a state; `--barcode` with one meaning; one rule for
  what is due a check; each command's own help.

## Decision (2026-10-14, issue #42): the clarity round, part 2

One way to do each thing.

- **`arv check SOURCE`** for every kind of copy. What SOURCE is (a device, a file, a folder) says
  which check: every sector read back, the image against its hash, or every file against the
  manifests. Before, the same question had three spellings (`check --device`, `check --image`,
  `verify`, and `stored --copy X` at its own path), and `verify` recorded nothing. A check is
  logged whenever the disc is in the catalogue. `--drive` stays for a file that is a whole disc
  read (a ddrescue dump is the medium's size, past the image), and stands in for a drive in the
  tests. An image whose hash differs is also tested with its RS03 data, which says which sectors.
- **`arv info`** takes a disc root, an image or a disc id (`id` and `ls` were the same question
  asked of different things).
- **`checkpoint`** is removed (decided 2026-10-14): a state with no discs is not in the archive's
  care, and recording only its hashes keeps nothing that could be got back. Snapshots of the
  everyday storage (ZFS, btrfs, Time Machine, restic) keep a folder's history, files and all
  (principle 7). Revisions are editions only, so `Stage` is gone; `status`, `log` and `diff` stay,
  as the history of what went onto discs.
- **`--barcode`** always means the copy's own; `stored` names a tape with `--carrier`.
- **One rule for "last checked"** (`disc_last_check`), one age syntax for `todo` and `list`.
- **Help:** each command has its own usage entry, with a line saying what it does; the manual,
  the man page, completion and `--help` all come from it, so they agree.


## Design (2026-10-14, not implemented): rearranging in a plan, and a plan as a collection's edition

Asked for: during planning, split folders and rearrange files (rename photos, sort them into
albums) without touching them on the drive, like `git mv`: the disc gets the new names and
places, the originals stay as they are, and arv still knows each file is the same file.

What a plan does today (tried 2026-10-13): `plan add SOURCE --as PATH` names and places an item on
a disc, a file or a folder, and `plan move` moves items between discs; nothing on the drive
changes. What is missing:

- **A folder split** (done 2026-10-14). A file added again from inside a planned folder ended up on
  the disc twice, silently (tried: `plan add trip DCIM --as Unsorted`, then one of its photos `--as
  Kinkaku-ji/pond.jpg`). Built as a rule rather than a stored `Except:` on the plan item: of two
  items holding a file, the more specific places it (nothing to go stale; dropping the specific
  item puts the file back). `plan show` says what each folder leaves to others. What is stored is
  on the archived data object, a fact about what was archived: `Except: PATH`, so `status` does
  not take a split folder for a changed one and invite a version that duplicates the file.
- **`arv plan rename NAME PATH NEWPATH`** (done 2026-10-14): an item's new place on its disc, a
  folder with all under it (the `git mv` of a plan); PATH inside a planned folder splits that file
  or folder off, under the new name.
- **`arv plan add NAME --list FILE|-`** (done 2026-10-14): `SOURCE<tab>PATH[<tab>DISC]` a line, so
  the sorting is done by what is good at it (exiftool for dates, a spreadsheet, a script, a model
  looking at the photos) and arv records the result (principle 7); all or none.
- **The original name kept** (a format question: decide before 1.0). `git mv` keeps a file's
  identity through a rename because the content is the same; arv already matches files by hash
  (`status` reports moves, `>`), so a renamed photo is found again by its content. What is lost is
  the name it had: `IMG_0001.JPG`, which ties it to the card and the camera. The disc's listing
  could keep it (an `Origin` column: the source path relative to the item), or the home only (as
  an object's `Source` is today), so the disc does not carry the layout of someone's drive.

**A plan as a collection's next edition.** A plan and a collection answer different questions:
the plan is how this disc's contents are chosen and laid out (by hand, from anywhere); the
collection is what is kept over the years, a lineage of editions (`status`, `diff` and `retire`
work on it). Since checkpoints went (issue #42), a collection is exactly that lineage, and
`plan again` is nearly an edition without one. So: `arv plan new NAME --collection CODE` (or
`plan make --edition CODE`) makes the plan's discs the collection's next edition. The collection
says what is kept; the plan says how this edition is arranged (the albums above). `diff` between
editions then shows the renames as moves, by content, as `git log --follow` would.

**Two kinds of move** (added 2026-10-14; the first kind was superseded the same day: see "after burning, an object's name and tags change, never its place"). A burned disc cannot change, and its files may no longer
be on any drive, so a move is either:

- **In the record only**, for files already on discs: "on disc D, `day1/IMG_0001.JPG` is now
  `Fushimi Inari/torii gates.jpg`". An appended event, as every change is (`Sets`, `Replaces`, so
  two catalogues merge), carried by the next disc's catalogue snapshot. It is the file's one name
  now, a correction of the record, and the commands honour it: `find` finds it by either name,
  `restore` puts it back under the new one (`--as-burned` for the old), and the next edition made
  from those discs uses it. The spec says how, so Katalog can show the current names too.
- **Planned for the next edition**, for files still on a drive: a plan's `--as` and `rename`. In a
  collection's own folder no arv command is needed: `mv` the files; `status` sees the move by
  content (`>`), and the next edition carries it.

They join: a move in the record becomes a move on discs the next time those files are made into
discs, as `git mv` waits for the next commit. So there is no need to re-burn to rename. The
physical rename waits for a remake that happens anyway: new media, several old discs gathered
into one, a damaged copy replaced.

**Remaking from discs, with no staging area.** When the files are only on discs, a plan reads
them from the copies:

- A plan's source may be a disc's files: `arv plan add NAME DISC-ID[:path]` (arv reading its own
  format, within principle 7).
- The copy read is the handiest the record knows: a stored image or folder on the NAS (no disc
  to fetch), else a disc in a drive (warm before cold, as `objects` lists them).
- Each file is read straight from the image or disc by its sectors (`extents.tsv`, on every disc)
  and checked against the old disc's manifest as it is read: a damaged file stops the make rather
  than being copied onto the new disc. No copy to a staging folder.
- With one drive and several source discs, `plan make` reads in disc order and asks for each disc
  once; a jukebox loads them from the same kind of worklist as `todo --tsv`.
- The names on the new discs are the record's: each object's name now, then the plan's `--as` and
  `rename`. (Moves in the record were dropped: see the decision on names and tags after burning.)

A disc that cannot be read or repaired cannot be remade; that is the old problem of two good
copies, which `todo` already watches.

**Open:** albums made by moving photos (one name and place a file) fit the above. Albums that share
photos, or a "best of" across many discs, would be the cross-disc virtual folders removed in issue
#42 (`arv selection`, given to Katalog): wanted or not is the owner's call.

**Folders, tags, and "best of"** (decided 2026-10-14). A folder is a tag a file has only one of (a
move changes it); a tag is a grouping a file may have any number of (`best`, `person:alice`). So
"best of" across discs is a tag, not a cross-disc virtual folder: `arv find best` lists it,
`arv keywords` writes it into the files for photo software, Katalog shows it. The one thing tags
do not give is order (a slideshow's sequence): that is names (`01 torii gates.jpg`), by a move or
`--as`. Selections stay removed.

- **Done (2026-10-14): tags on files.** A line of `tags.tsv` names a folder or a file; a file has
  its folders' tags and its own. `make --draft` and `describe --apply` take a file's path,
  `find` shows it as the file, `tags` counts files apart from folders, `keywords` writes one
  file's keywords. A path is a file when the disc's listing names it (folders are not listed).
- **Done (2026-10-14): tags added after burning are events.** One event an apply, a `Tags:
  PATH<tab>TAGS` (or `Caption:`) field for each path that changed, with `Sets: Tags:PATH` and
  `Replaces`, so field histories, merges and conflicts work as for any field. The disc's
  `tags.tsv` stays as burned; readers lay the newest change of each path over it
  (`disc_tags_text`). A later disc's snapshot writes the other discs' tags as they are now, so
  readers without event support see them (applying the events again changes nothing). Not yet: a
  command in arv itself to set tags by hand (today it is `arv-assist`'s `describe --apply` with a
  draft, or `tag --apply`).
- **Done (2026-10-14): declared namespaces.** `Namespace` records in the tag vocabulary (`Name`,
  `Description`, `Values: one|many`, `From: file-date`); `arv tags` shows them, marks undeclared
  ones, and lists a path given two values of a one-valued namespace and a `date:` that is not EDTF.
  Two corrections, found by the reference scenario's own data (a disc tagged `place:kyoto` with
  folders `place:fushimi-inari`, `place:arashiyama`): **places nest**, so `place` takes many values
  (a layout cannot file by it until places have parents, as set codes do); and for a one-valued
  namespace **a path's own value replaces its folder's**, the rule the folder split uses (the more
  specific wins). A folder `date:2019` with one scan inside `date:1974` is not a conflict; two
  dates on one path is. Not yet: `From` is declared but nothing suggests from it.
- **Done (2026-10-14): layouts in disc plans.** `plan tag` (and `--list`) gives files tags before
  they are on a disc, kept in the plan as `Tags` records by source; `plan layout` gives a disc
  its layout. What was decided on the way:
  - **A layout is the disc's, and places objects whole** (corrected the same day, the owner's
    point: an object has tags, not a path; a disc has a tree). First built per item, placing each
    file of a folder by its own tags: that split data objects across folders, which this design
    says never to do, and needed per-item layouts only so a project would not be scattered by its
    files' dates. Placing objects whole needs neither: a project tagged `project:weather-station`
    goes to `project/weather-station/station/` with its tree, and a photo is placed on its own
    only when it is its own object.
  - **`plan add --each`**: a folder's every file an item, and so a data object, of its own. Both
    levels are normal, as in archival description (ISAD(G): a file, or an item): a folder of 500
    photos added whole is described and placed as one, by choice (the "More Product, Less
    Process" approach); `--each` when each photo deserves its own tags and name.
  - **Original order kept**: an object placed by a layout records `Arranged` (its path in the plan,
    before the layout), and `Name` keeps its name where it was read from (PREMIS `originalName`).
  - **Year from the files' time** when there is no `date:` tag, as `From: file-date` declares
    (for a folder, its earliest file). `plan show` says where each item goes before anything is
    made, which is where a scan dated by its scanning shows up.
  - **Two at one path** (two cameras' `IMG_0001.JPG`) are refused, not renamed: a silent
    `IMG_0001-2.JPG` would be a name nobody chose. `plan rename` gives one another name, and the
    layout keeps placing it, under that name (the slug of the design above).
  - **The object is the source, not the layout**: its manifest and `Tree` are its files' paths
    within it, so the same object placed two ways is the same version.
  Not yet: titles, and a layout for `arv make` of a folder that is not a plan.

## Done (2026-10-14, issue #41): the README for readers without English

A short note (two to four lines each) near the top of every disc's `README.txt`, after the facts
and before the description, in Chinese, Spanish, Hindi, Arabic, French, Russian, Portuguese,
Japanese, German and Bengali: an archive disc; ordinary files in `data/` any computer opens; the
instructions below are in English, a translation tool or AI assistant can translate them, and the
commands work as written. Not full translations, as the issue says: each would be another copy to
keep in step, and a wrong translated repair step is costly.

**The translations are drafts, not reviewed by native speakers.** Before the first real burns,
each wants a native speaker's check (wording, and in Arabic the right-to-left rendering of the
`data/` name). Later, perhaps: the owner's language added through the config.

Two fallback checks in VERIFY, for a person with only a shell: `shasum -a 256 -c` (macOS has no
`sha256sum`), and a count of the payload's bytes and files (`find data -type f -exec wc -c {} + |
awk ...`, which prints `BYTES.FILES`) to compare with `Payload-Oxum`, since `sha256sum -c` does not
notice a missing or extra file. Tried on a disc: it matches, and is one file and its bytes short
after one is removed. `wc -c` takes a file's size from the file system, so this is quick.

## Decision (2026-10-14): no remaking discs from discs

Built the same day (`arv plan add NAME DISC-ID[:PATH]`, read from a copy the record knows as files,
each file checked against the old disc's manifest, each object continuing its lineage, times,
tags and names carried) and removed: the owner asked whether it was important, and it is not.
What preservation needs over the years is refreshment and replication (OAIS): the same image on
new media (`arv burned`, another copy of the same disc), or the images kept on newer storage
(`arv stored`, a `Carrier` for a tape or drive holding several). The bits stay identical, the
check stays trivial, and the copying is other tools' work (principle 7). Remaking adds only
re-arranged or gathered discs from discs whose originals are gone, which is rare (arranging is
done before burning, from the originals, which plans do), and it cost about 250 lines, and wanted
reading images by their sectors to be right. The design above stays, for if the need turns up.
What it taught stays too: an object's tags and name follow its Uuid, and a plan of a source
archived before places it by its tags now and names it by its name now.

So a remake is a new plan, not a clone (the owner's words): restore the discs to folders, plan
them. What was missing for that was knowing the restored files are the same objects. They are
(the owner's point), and arv can know it from provenance rather than bytes, because it wrote them:
`arv restore` records where it put a disc's files (a `dissemination` event with `Path`, in the
home that knows the disc), and a plan of a file or folder under it continues the object at the
same path on that disc. (First built as a `.arv` file left in the folder; a check caught that a
restored git repository was then not exactly its commit, and the manual says restore writes only
what it restores, so the record keeps it instead.) For copies
arv did not make, `plan add --of OBJECT` says the same; `SameBytes` suggests it. An object's tags
now are its own (set on it), else what its disc gives its path (its folders' too), so a burned
`date:1974~` still files a scan under `decade/1970s/` on the remade disc.

## Done (2026-10-14): a plan as a collection's next edition

`arv plan new NAME --collection CODE` (a plan setting, like `--set`, kept as `Collection`): `plan
make` makes the plan's discs the collection's next edition, through the same path as a make of
the collection's folder (its title and set as defaults, the id prefix, the `Revision` with its
manifest and changes, `--keep` and `--message`). The plan's items are still data objects. So the
collection is what is kept, a lineage of editions, and the plan is how this one is arranged:
`diff` shows the arrangement as moves, by content.

## Decision (2026-10-14): after burning, an object's name and tags change, never its place

Built first as moves in the record (`arv rename DISC-ID PATH NEWPATH`, `Moved: BURNED<TAB>NOW`,
honoured by `find`, `objects` and `restore`), following "Two kinds of move" above, which was written
before an object's place became its tags'. The owner's point: it is just tags, not paths. So:
- **A disc's paths are facts about that disc**, and stay. Nothing moves.
- **Re-filing is a tag change**, and **renaming is the object's name** (its slug), both events
  about the object by its Uuid (`Object: object:UUID`, `Tags:` or `Slug:`, `Sets`), so they follow
  it to every copy and every disc made from it, as Archivematica's identifiers do, rather than a path on one
  disc. `arv retag OBJECT TAGS`, `arv rename OBJECT NEWNAME`.
- **Tags by object**: `describe --apply` and `tag --apply` record a change to an object's own path
  as the object's tags; path-keyed changes stay for files inside a folder object, which have no
  identity of their own.
- **`restore` restores the disc as burned.** Today's arrangement is a new disc, not a restore.
- **A plan of the same source places by its tags now and names by its name now**: in the test, a
  photo re-tagged `event:osaka-trip` and renamed `torii.jpg`, planned again from the drive, goes to
  `year/2019/event/osaka-trip/torii.jpg`.
The path moves, `--as-burned` and the name mapping in `find`, `objects` and `restore` were removed.

## Decision (2026-10-14): identity is the lineage, never the bytes

The owner's point: the same bytes can be different data objects (the colour of the bits, after
Skala's *What Colour are your bits?*; in legal terms, provenance: the same PDF from a solicitor
and from a download), and one object changes bytes from edition to edition, with an identity
joining them. That is OCFL's model (an object id, versions mapping paths to digests, bytes shared
only within an object) and PREMIS's (an Intellectual Entity above its representations and files).
Archivematica gives every package and every file a UUID and keeps checksums for fixity, not
identity, but has no lineage of a file across ingests.

arv had it half right: an object's `Uuid` and `Version` are a lineage, but any matching `Tree`
was taken as the same version, wherever it came from. Now the lineage decides (the object
archived from the same `Source`); the same `Tree` elsewhere is a new object with `SameBytes`,
noted and shown (`make`, `status`, `retire`), not merged. A person who knows says "these are one":
`arv same A B` (done 2026-10-14), an event about A (`SameAs: object:B`); every load reads A's
records as B's versions with the same `Tree`, so a merge or rebuild that brings A's records back
from discs reads them the same way, and the records on discs need not change. Only when each of A's
versions has bytes B had: A with a history of its own (other bytes) stays apart, as joining two
histories would need a person to order them. For a source not yet archived, `plan add --of`. Files are still found by hash (`status`
reports moves, `retire` counts files on discs that stay): that is fixity and finding, not identity.

Considered, not built: sharing identical bytes on one disc (UDF lets two names point to one File
Entry, with a link count, which FAT cannot). It is a format feature to test on real readers, for a
saving nobody has measured: first a report of byte-identical files, then hard links only if the
report shows them worth it, after the first burns.

**A layout is a view of tags** (planned; corrected 2026-10-14). First written the other way round
(a declared folder layout from which tags are derived), but the owner's experience is that the
order changes between discs: year first on one, event first on another, as each is handier. So the
tags are the facts about a file (`year:2019`, `event:kyoto-trip`, `best`, `person:alice`), kept with
it, and a disc's folders are one view of them, chosen when the disc is made:

- **The folders name their namespace**, so the tree describes itself (the owner's idea): a layout
  level is a namespace folder holding a folder for each value, `event/kyoto-trip/IMG_0001.JPG`.
  Anyone browsing the disc decades on reads `event/kyoto-trip/` without arv, and turning the tree
  back into tags is reading the path. (The compact form, `event=kyoto-trip/`, is what Hive and Spark
  use; plain nested folders are kinder in a file manager, so they are the default.)
- **A plan or make takes a layout**, chosen a disc at a time and recorded on it (a `Layout` field,
  for tools; people need not read it): `/` nests, `,` falls back.
  - `--layout 'year/event, year'` (`year` and `decade` are levels made from `date`): a photo with an
    event goes to `year/2019/event/kyoto-trip/`, one without to `year/2019/`. Year first never needs its fallback (every photo has a date),
    but an event across New Year is filed by its start, and a photo of its second day may be dated
    in the new year: tag the photos with the event, and they stay together under `event/`.
  - `--layout 'event/year, year'`: events whole (`event/kyoto-trip/year/2019/`), and the photos
    with no event by year. Which comes first is the disc's purpose (browsing by time or by
    occasion), and it changes between discs, which is why it is chosen a disc at a time.
  - Each file appears once, under the first alternative it has every tag for; the tags a layout
    does not use stay tags (`find 2019` finds a photo filed under `event/` by its date). A file with
    none of the alternatives goes to the layout's last resort (its plain folder from the source),
    and is listed. So there is no `unsorted/` folder in the usual case.
- **Only namespaces with one value a file make folders.** The vocabulary declares them:
  `Namespace` records in `config/tags.rec` (`Name`, `Description`, `Values: one|many`, and `From`
  for where a value may be suggested). `date`, `event` and `project` are `one` (`place` was, until
  places were seen to nest: see "declared namespaces" above); `person`, `place`,
  `source` and plain tags (`best`) are `many`, and a layout refuses them (a photo of two people
  cannot be in two folders). `arv tags` lists a file with two values of a `one` namespace, before it
  reaches a disc. (Not "categories": a disc's categories are already the set vocabulary's.)
- **Dates are EDTF, and may be ranges** (corrected 2026-10-14: an object can span years). A project
  folder of 2020 to 2023, letters of 1995 to 2008, a trip over New Year: one object, many years. So
  the namespace is `date`, not `year`, holding one EDTF value (`2019-07`, `1995/2008`, `197X`,
  `1995~`), as a disc's `Coverage` does. It is the date of the content: the file's own date (EXIF,
  its time) is only a suggestion (`From: file-date`), right for a camera photo, wrong for a scan of
  a 1970s print made in 2019. A layout files a range by its start (`year/1995/` for the letters,
  whole), or by `decade` (`decade/1990s/`, kind to long ranges and approximate dates); the range
  stays in the record (`list --covers`, `find`). A data object is never split across folders.
- **Tags come from where they can**: `year` from the file's own date (EXIF, or its time) with no
  one tagging it; the rest from a person, a model (`arv describe`, `arv tag`) or a folder the
  files came in (an existing `Kyoto trip/` gives `event:kyoto-trip` once, when the file is first
  archived). After that the tag, not the folder, is the record.
- **Moves become tag changes**: re-filing a photo under another event is changing its `event` tag;
  the next disc made with a layout puts it there. There is no "move" in a layout plan: a place is
  worked out, not chosen (the owner's point, 2026-10-14). `plan rename` and `--as` stay for plans
  placed by hand, and for what a layout does not apply to (a project keeps the tree it came with).
- **Names: a title and a slug.** The title is for people ("Torii gates at Fushimi Inari"), in the
  record and searched, as data objects' `Title` already is. The slug is the file's name on the disc
  (`torii-gates.jpg`): short, safe on every system, the last part of the path a layout builds. The
  slug is the original name (`IMG_0001.JPG`) unless one is given, which settles keeping the
  original name: nothing is lost unless a slug is chosen, and the record keeps both. So "splitting
  a file off and renaming it" in a layout plan is giving it a title, a slug or tags: a metadata
  change, an event like a tag change, not a placement.
- **Limits:** files with no structure to speak of (a software project, a document folder) keep the
  tree they came with: a layout is for collections whose structure is their tags (photos, scans,
  recordings).

Order: the folder split first (it fixes the silent duplicate), then `rename` and `--list`, then
the original name (format), then moves in the record, then plans as editions and plans from discs,
which build on all of them. Tags after burning as events come before layouts (a layout needs every
file's tags to be in the record).

## Done (2026-10-15, issue #37): fast on a large catalogue

Measured on a synthetic home (`dev-tools/bigcat.py`): each disc with three copies (two discs on
shelves, an image on a NAS), a fixity check of each copy, a move of every third disc's copy, one
data object, 200 files. Seconds, on a cloud container:

| Command | 2,000 discs, before | 2,000 discs, after | 10,000 discs, after |
|---|---|---|---|
| `list` | 17.8 | 0.05 | 0.23 |
| `objects` | 85 | 0.11 | 0.71 |
| `objects --json` (the GUI) | 107 | 0.13 | 0.76 |
| `todo` | over 300 | 0.11 | 0.65 |
| `todo --tsv` | 50 | 0.08 | 0.42 |
| `audit` | 8.8 | 0.08 | 0.43 |
| `find` | 44 | 0.26 | 2.2 (reads every listing) |
| `rebuild` into a home that has it all | 5.4 | 0.14 | 1.9 |

The cause was the same everywhere: a question about one disc (its copies, a copy's place or last
check, its tags) went through every event in the catalogue, and was asked once a disc, so a
command took the square of the catalogue's size. Now the events are indexed once a run by what
they are about (`disc_events`), as are discs by id (`archive_disc`) and objects by Uuid and by disc
(`records_by`); an index is rebuilt when the list it indexes grows. `rebuild`'s merge looks events,
discs and object versions up in a hash set. Comparing identifiers across copies sorts them, and
the pass that joins objects made the same (`arv same`) is skipped when there are none.

Not done: a cache on disk. The catalogue is a text file read whole each run (a fraction of a second at
10,000 discs), and it stays the one source of truth; a second file would have to be kept in step.
Not worth it until a catalogue is many times this size.

## Done (2026-10-15, issue #38): the interface catches up with copies

What #34 gave the command line, in `arv gui` where a person does it by hand: a barcode for a copy
(the field takes a scanner's Enter), a new letter for one of two copies lettered alike (offered
only then, naming each copy by its place, or by an identifier when both share one), an inventory
of a shelf or library, and the audit. Each runs the command and shows what it printed; the
interface adds no behaviour of its own. Findings are shown by their fixed first word, with one
line on what each means.

Not in the interface: `arv drives` (a drive is local hardware; the command line is the place), and
a library loop (a script's job: `docs/examples/library-check.sh`). The demo is not re-recorded: it
already shows copies by letter, and the rest is for archives past their first few discs.

## Decision (2026-10-15): the interface's server knows no features; later, in C

`arv gui` stays a web page in the browser, not a native window. The browser is on every machine
and brings what a toolkit would have to: text input in every script, scaling, dark mode, screen
readers. It works on a NAS with no display, through `ssh -L`. And HTML is already the format of
every disc's `index.html`. Native toolkits were weighed: raylib, Nuklear and Dear ImGui are made for
games and are weakest at forms, long names and text input; GTK, Qt and FLTK are heavy and packaged
per platform; libui-ng is still alpha; a webview window is still HTML, with WebKitGTK on Linux.
Each would be a library linked into arv, which builds with none.

Step 1, built: the server runs commands and nothing else. The page builds every command and
reads arv's output, so a feature is a change to `gui.html` alone. The server is four routes:
run arv or arv-assist (to its end, or as a job), follow a job, list a folder, and the page.
Drafts go to `arv make --draft -` and `arv describe --apply -` on standard input, so the server
writes no files. `arv location list --json` was added rather than parse its text. The first run
asks `arv where --json`, and after `arv init` opens on the new archive without a reload.

Step 2, issue #48: the same API in C, a separate program as now, so `arv` does not grow. HTTP/1.0
on 127.0.0.1 only: TLS protects traffic between machines, and this never leaves one. llama.cpp's
server is large for other reasons (a general HTTP library with TLS, and a SvelteKit page compiled
in); ours needs neither. The page is read at run time from `share/arv`, not embedded, so no
generated file is committed and `data.c` stays as it is. The tests stay in Python: they speak
HTTP, so they test either server.

## Decision (2026-10-15): a stable interface is arguments in, JSON out

Asked: would JSON input give arv a stable interface for other software? No. The arguments are
already a structured input (a list of strings, no shell), and the interface showed it: its page
builds every command as one. A second way in, a JSON request for each command, would be a second
parser to keep in step with the first, and two ways in can disagree. What makes an interface
stable is that its commands, options and outputs stop changing: a promise, kept from 1.0 (#31).

What was missing, and is built:

- **The version of each output's shape.** The TSV outputs had one (`# arv todo 1`); the JSON did
  not. Each now starts `"format": "arv-json 1"`, the form the 2026-10-10 note agreed. A reader
  checks it; a new field does not change it, a renamed or removed one does.
- **A result from the commands that write.** Reads had `--json`; writes printed only sentences, so a
  script learned a new copy's letter by parsing "Write its letter on the hub ...". Now `--json` on
  any command that changes the record prints the events it made, as `list --json` shows events.
  It is done once, not per command: the record is its events, so "what this did" is the events
  this run added, which `archive_save` already knows (they are the ones still waiting for an id).
  What a command says to a person goes to standard error; after a failure, the events written
  before it, and the exit status.

Still to do for 1.0, under #31: write down each output's fields as the interface (the manual's
"Output for scripts"); structured input only where many things go in at once, per command, as
`plan add --list` and drafts on standard input already do (recording a stack of burned discs is
the next such case); perhaps `arv help --json`, the commands and their options, for a program to
discover them (the C server of #48 could use it for its list of commands).

## Done (2026-10-15, issue #48): the interface's server in C

Step 2 of the 2026-10-15 decision: `src/arv-gui/arv-gui.c`, about 800 lines, against the API
step 1 settled, so the page did not change and the tests (Python, over HTTP) passed against it
as they were. How it is built, and why:

- **A process for each connection** (fork), not threads: no shared state to lock, and a slow
  command holds up nothing else. That needs the jobs out of memory: each is a folder under the
  server's own temporary folder (its command, input, output, and exit status once done, written
  by a process that outlives the connection). Removed when it stops; never anything of the
  archive's.
- **HTTP/1.0, read and answered, then closed.** No keep-alive, chunks or TLS: one machine.
- **arv's own modules, linked** as arv-assist links them, for the JSON reader and strings: no
  second JSON parser. Output is cleaned to UTF-8 before it is quoted, since JSON must be.
- **The page is a file, read at each load**: from `share/arv` when installed, beside the program
  in a checkout. Nothing generated is committed; `data.c` stays arv's.
- Python is now only the tests' language. The Debian package recommends `xdg-utils` (to open the
  browser) instead of `python3`.

## Decision (2026-10-15): arv stays C99, and TinyCC proves it

C99 came from the port to C, not from a choice made on its own; asked whether a newer standard
would do, the owner chose to keep it. The audience for the source on a disc is someone decades
from now, with whatever compiler they have: an old one, a small one (TinyCC, cproc, chibicc), or
one written to bootstrap a machine. Every C compiler of the last twenty years takes C99; C11 is
near universal in the large compilers but only partly in the small; C23 (GCC 14 and 15, Clang 18
and 19) is in almost none. What the newer standards add is convenience (`_Static_assert`,
`_Generic`, `bool`, `typeof`, `constexpr`); the one feature that would change anything here is
C23's `#embed`, and arv-gui's page does not need it (below). So the floor stays where the most
compilers are.

The bet was untested until now. TinyCC 0.9.27 builds arv with the very line each disc's README.txt
gives (`cc` replaced by `tcc`), with no warning, and the binary passes every check, the reference
scenarios included. arv-assist and arv-gui build with it too, and the GUI's tests pass against
the TinyCC-built server. `make -C src/arv check-tcc` repeats it (about a minute); not part of
`make check`, which would double its time, so run it before a release and after anything that
might reach past C99.

**arv-gui's page, built in as well as read** (the same day). The page stays a file read at each
load (`share/arv`, or beside the program in a checkout), so it can be changed without building. A
copy is also built in, for `arv-gui` copied on its own: `make` writes `build/page.c` with
`dev-tools/embed.c`, which arv's `data.c` already uses, so no tool beyond a C compiler is
needed (not `xxd`, and not `objcopy`, whose output is tied to one CPU's object format), and
nothing generated is committed. Built by hand without it, arv-gui needs the page beside it.

## Done (2026-10-15, issue #31): every read has --json, and two questions settled

`todo`, `status`, `info` and `help` now have `--json`, so every command that reads gives a script
the same answer as a person, and every command that writes gives the events it made. Each is
rendered from one walk of the record, so the text and the JSON cannot drift: `todo` passes each
finding through one function that prints its line or adds its item (a stable `kind`: `no-copy`,
`read-back`, `edition-not-safe`, `replaced`, `one-carrier`, `missing`, `no-cold-copy`,
`fewer-places`, `below-appraisal`, `overdue`, `object-no-cold-copy`, `object-gone`), and the
reference scenarios show the text unchanged. `status` gives what the folder is and every change
(the text shows twenty); what its other helpers say (data objects, repositories, layouts) comes as
`notes`, lines for a person, since a script would rarely act on them. `help --json` reads the same
usage text as the manual, so a program can find the commands without parsing `--help`.

**Exit codes stay 0, 1, 2.** sysexits' 64 to 78 tell a usage error from bad data from a missing
file. A script calling arv needs to know: done; something needs a person (and stderr or the JSON
says what); not used as meant. Finer codes would be a second vocabulary for what the messages say
better, and few scripts branch on them. Documented in the manual's "Exit status".

**No general batch input.** Most commands act on one thing, and many things in a row are a shell
loop (`for d in ...; do arv burned "$d" --copies 1 --location SHELF; done`): each is its own
event, and one failing stops nothing else. Input in bulk is for where all or none matters, and
arv has it there already: `plan add --list` and `plan tag --list` take lines and apply all or
none, and a draft goes in on standard input. Recording a stack of burned discs is a loop.

What #31 still holds for 1.0: freezing the fields as the interface (the manual lists every output
and its main keys), the spec's "For other software" section, and the home catalogue's parts in one
place.

## Done (2026-10-15): arv in the desktop's menu

`make install` puts `arv.desktop` in `share/applications` and arv's mark (`docs/img/logo-mark.svg`)
as its icon, so the packages carry them too; `desktop-file-validate` passes it. It runs
`arv gui --idle 15`. Started from a menu there is no terminal and no Ctrl+C, and each launch would
leave a server running, so the server stops by itself: the page says it is open once a minute,
and with `--idle` the server stops when no request has come for that long and no job is running
(a job outlives a closed page: it stops after). Started in a terminal it runs until stopped, as
before. With that, a person can set up an archive, make and record discs, keep places and
collections, and check and restore without a terminal; installing is still `make install` or a
package.

## Note (2026-10-15): room left for files once RS03 is added

How much of a disc is left for files, at each disc size and error correction level. The fixed part
(each medium's sectors, and the most image each takes at each redundancy) is in the spec, "The room
on a disc"; this adds what `arv plan` leaves for files. GiB of 2^30
bytes; "image" is everything on the disc before RS03 (files, `tools/`, the catalogue, the bag's
tag files), "files" what `arv plan show` offers after it sets aside about 128 MiB for `tools/`,
0.5% and up to 2 MiB more (the catalogue snapshot, which grows with the archive, comes off too).

| Medium (formatted, with defect management) | 10% | 20% (default) | 30% | 50% |
|---|---|---|---|---|
| BD-R 25 GB (22.6 GiB) | 20.3 / 20.1 | 18.7 / 18.4 | 17.2 / 17.0 | 14.9 / 14.7 |
| BD-R DL 50 GB (45.1 GiB) | 40.7 / 40.4 | 37.3 / 37.0 | 34.5 / 34.2 | 29.9 / 29.6 |
| BD-R XL 100 GB (90.2 GiB) | 81.4 / 80.8 | 74.7 / 74.2 | 69.0 / 68.5 | 59.8 / 59.4 |
| BD-R XL 128 GB (115.2 GiB) | 103.9 / 103.3 | 95.3 / 94.7 | 88.1 / 87.5 | 76.4 / 75.8 |

(image / files, GiB.) How it is worked out (`data_budget` in make.c, `room` in plan.c): RS03 cuts
the medium into 255 layers, and each codeword takes one sector from every layer; with redundancy r
at least, at most floor(255 / (1 + r)) - 1 layers hold data (never more than 246), so 20% gives 211
data layers and 44 of error correction: 20.9% in fact, the next whole layer up. RS03 then fills
the rest of the disc, so a disc that is not full gets more than asked for: the minimum is what is
promised. Burning without defect management (`--no-defect-management`) gives back the spare area,
about 3% (19.3 GiB of image on a BD-R 25 at 20%), for discs that are read back and checked anyway.

## Decision (2026-10-15): packed objects, chosen per object and per disc; the disc stays UDF

Asked for compression, the alternatives were a compressed filesystem (SquashFS) in place of UDF,
or compression inside the bag. SquashFS was set aside for optical discs: Windows, macOS and players
do not mount it, it has no standards body (the kernel's source is its specification), compressed
blocks and fragments end reading with `dd` from `extents.tsv`, and its one copy of compressed
metadata turns a bad patch into many lost files (UDF 2.50 mirrors its metadata). It stays a
candidate for the encrypted inner image (above) and for carriers that are not optical.

Archives mostly compress inside formats (FFV1, FLAC, lossless JPEG 2000) or per record (WARC's
gzip members), seldom across a volume; Archivematica compresses each package (7-Zip by default)
and many choose uncompressed. arv follows the per-object way: a plan says, for an object or for a
disc, whether it is kept as one file and how; the rest of the disc stays plain files. Some discs
compressed, others not, as the person chooses: compression pays for text, source and mail, where
it can also leave room for more RS03, and for many small files, each of which takes a sector.

- **One rule for every pack:** member paths are relative to the folder the pack is in, so
  unpacking a pack where it is puts each object at its `Path`. An object's own pack sits next to
  where it would be (`data/projects/station.tar.zst`); `--together` puts a disc's packed objects in
  one `data/pack.*`, for the gain of compressing them together, at the cost of a bad patch reaching
  several objects.
- **The record keeps the object, not the pack:** `Path` is where it unpacks to, `Pack` the file,
  `Packing` what it is; `Tree` is over its files, so packed and plain copies are the same version,
  and `arv status` reads the object's manifest to find its files on discs.
- **arv writes the tar** (about 150 lines): the system's tars differ (GNU, bsdtar, busybox) in
  headers, owners and order, and making them agree takes GNU-only options; members come from
  anywhere under names of the plan's choosing; and writing it lets arv hash each file as it goes
  in and stop if it changed since it was scanned. The compressor is the system's, run with options
  that give the same bytes (zstd `-12 -T0`, the same at any thread count; xz in blocks; gzip `-n`).
  `-19` was measured at about 0.5 MB/s, too slow for a Blu-ray. Zip is the zip program's, over a
  folder of links, its files checked afterwards.
- **Tar or zip:** tar through a compressor packs best (one stream); zip opens anywhere with a
  double click and loses only a damaged file. Both are standards (POSIX; ISO/IEC 21320-1).
- **A file that is itself an archive** (a `.zip` someone made) gets `Packing` alone, by its name,
  confirmed by its first bytes, so `.docx` and `.epub` (zips inside) are documents, not packs.

A pack holding objects of several discs is never made: a pack is on one disc.

**Reading packs back** (2026-10-16). The disc carries what is inside its packs:
`catalog/volumes/ID/packed.tsv`, each file's SHA-256, its path as unpacked, its pack and packing,
so a found disc can check what it unpacks without the home (the objects' manifests in
`catalog/objects/` stay at home). arv reads its own tar (and pax) and runs the compressor's `-dc`
(or `unzip`), and with that: `plan make` reads each pack back as soon as it is written, and stops
if any file differs, so an image identical to the one made has good packs; `arv check` of a disc's
files reads inside each pack against `packed.tsv`; `arv restore` unpacks each pack where it is,
with the files' times and modes, and restores a damaged pack (or one whose compressor is missing)
as it is, saying so. `plan show --packed` makes each pack in a temporary folder and measures it,
and the disc's fill uses the sizes: no estimate, since what is compressed already does not shrink
and only the compressor knows by how much the rest does.

## Decision (2026-10-16): describing without a model, by questions; thumbnails later

The owner wanted a way to describe that needs no AI. Twenty questions works because each answer
splits a set of known possibilities; arv has that set, its own vocabulary: the tags and aliases
in `tags.rec`, the namespaces (event, place, person, project, source, date) and the values the
archive has used. So `arv describe --ask` (arv-assist, `ask.c`) asks over the vocabulary, not in
the open:

- **Clues first, with no question:** the kinds of files (by extension), dates in their names
  (`IMG_20190712`, `2019-07 Kyoto`) or EXIF (`exiftool`, sampled, when installed; principle 7),
  a camera's model, names made by a camera or phone, the words of the folders' names. A word that
  is a tag or an alias marks that tag; a Match pattern that claims a tenth of the files marks its
  tag; a value used before whose words are in the names marks it.
- **Then a question for each thing still unknown,** in an order that narrows (topics, then event or
  project, places, people, what made it, when), with the likely answers marked: Enter takes them,
  a number picks, anything else is typed. A place's words are not offered again as a person. A
  typed date must be EDTF. At most 20 questions; '.' stops and keeps the clues for the rest. The
  first-level folders can be gone through too, asking only what may differ (places, people, a
  date outside the whole's).
- **Free text is made, not asked:** the title and description come from the answers by template
  ("Kyoto trip, 11 to 13 July 2019"; "6 photos, from a Pixel 7, in Kyoto, with Ann, of the Kyoto
  trip, ..."), to accept or edit.
- **Authorship:** `--ask` is the person's (`human`); `--clues`, the clues alone, is `automatic`.
  Both write the same draft as the model's path, so applying, events and the GUI are shared.

It lives in arv-assist, next to `--answer` and `--apply` (also model-free), because it shares the
draft and applying code; arv-assist is always built and installed.

**Later, in this order:** learned rules (an answer that the clues did not foresee becomes an
Alias or a Match in the vocabulary, so the question is not asked next time); the questions in the
GUI (a JSON question protocol, buttons for the choices; for now the GUI has "Fill from the files");
a small model only to rank each question's choices, never to write: text through arv-assist's
embeddings (`llama-embedding`, bge-small, about 37 MB), images through llama.cpp's `llama-server`
with a small vision model whose answer is held to the choices by a JSON schema. The smallest
vision models llama.cpp runs are a few hundred MB, not under 100; a CLIP-style encoder would be
smaller but needs a second runtime, and a model is on the computer, never on a disc.

**Thumbnails on discs (designed, not built).** Each disc could carry thumbnails of its own photos,
videos (a frame) and documents (a first page), so a found disc, the home and the GUI show what a
disc holds without reading it, and the questions above can show them. About 256 px JPEG, 6 to 10
KB each: 10,000 photos is about 80 MB of a 25 GB disc. Named by the content's SHA-256 (one per
picture, whatever its names), plain files under `catalog/thumbs/` with an index TSV, so
`index.html` and any file browser show them with no reader. Made by tools built for it
(`vipsthumbnail`, ImageMagick, `ffmpeg`, `pdftoppm`) when present; none otherwise. A disc carries
its own thumbnails, not every other disc's (100,000 photos would put about a gigabyte on every
disc): those stay at home. They follow the disc's access level, since a thumbnail shows content.

## Done (2026-10-16): words and tags, counted; text and PDFs; rules offered

Learned rules from the entry above, done by counting, so that each answer makes the next folder's
questions shorter. No model: every suggestion says the counts behind it.

- **Rarity:** every folder of every disc's listing, its name and its files' names, gives how many
  folders each word is in. "img", "final" and "2019" are everywhere and say nothing; "kicad" and
  "fushimi" are rare and say a lot. `--ask` offers the rare words first, and leaves out a word in
  more than a fifth of the archive's folders.
- **Words with tags:** every tagged path (the discs' tags now, and objects' own tags where their
  disc has none for their path) counts its names' words, and each two in a row ("birthday party"),
  against its tags. A word seen in 3 tagged paths or more, 80% of them with a tag, marks that tag:
  "'kicad' in 9 of 10 folders tagged electronics". Kept in `cache/words.tsv`, rebuilt when the
  catalogue changes (its size and time), never on a disc.
- **Text and PDFs:** a folder's text files (the first 64 KB) and PDFs (`pdftotext`, the first three
  pages, when installed) are sampled, 20 a folder, and their 30 most telling words kept: frequent
  there, in two files or more, rare in the archive's names, with a short list of common English words
  left out (rarity alone cannot judge a short text). The text is never kept, only those words. A
  PDF's own Title (`pdfinfo`) goes into the description. Text words weigh half a name's: a word in a
  folder's name was chosen, one in a letter is incidental.
- **What was told:** each `--ask` answer is kept in `config/word-examples.tsv` (home only, not
  rebuildable: the text's words are in no record), with the words of the names and of the text, and
  counted with the rest.
- **Rules:** a word that has been with a tag in 3 or more of the folders it was in, 90% of them,
  counting this answer, is offered as a rule: `Match: *kicad*` under the tag in `tags.rec`, inserted
  after its Name line so the file keeps its comments (a new Tag record, before the Namespace records,
  when there is none). Then the vocabulary has it, on discs too, and anyone can read and edit it.

Limits: it starts empty and learns habits, mistakes too, so it only marks answers and never tags
alone (`--clues` takes what is marked, as `automatic`). Names written without spaces (京都旅行) are
one word to it; short runs of characters would split them, later. Office files are not read yet
(`pandoc`). OCR is not arv's (decided 2026-10-17): it is slow (seconds a page, again on every
round of the GUI's questions), often wrong on handwriting and old type (bad words make bad clues),
and needs choices (languages, page layout) made once with the tool built for it. Run before
describing, `ocrmypdf` gives a PDF a text layer and `tesseract` a `.txt` beside an image, which arv
already reads; the text is then kept on the disc too, searchable with `grep` when no OCR tool is
at hand, rather than worked out and thrown away while describing. Contents are not searched: `arv find` stays on names,
tags and descriptions, and full-text search is for tools built for it (Recoll, ripgrep).

**Later: a model as a reader, not a writer.** With this in place a model's work can shrink to what
counting cannot do, reading: pulling the markers out of a letter or a photo (a place, a person, an
organisation, a date), which the counts and the vocabulary then turn into questions, and the person
confirms. A model that extracts names is a smaller job, easier to check, than one that writes
descriptions. Places come next, as their own vocabulary (`places.rec`, Natural Earth's populated
places, public domain, with the nearest place to a photo's EXIF GPS as a clue); its format goes here
when it is built.

**Common words, phrases and names in text** (the same day). Rarity is counted over names, which
hardly ever hold "hello", "name" or "address", so those came through from forms and letters; and a
dictionary is the wrong test, since "invoice", "wedding" and "tax" are dictionary words and among the
best clues. What says nothing is the language's commonest words, so:

- **A list to start from:** `src/arv-assist/data/common-words.txt`, the 1,500 most frequent English
  words of three letters or more over 24 public-domain books (Project Gutenberg), each in half of
  them at least so no character's name gets in, and a short list of today's letter, form and email
  words the books lack. Made by `dev-tools/common-words.sh`; built into arv-assist at each build
  (`dev-tools/embed.c`), never as generated code in the repository. A word of the vocabulary (a tag
  or an alias) is never common: "family" stays a clue though the list has it.
- **Then the archive's own texts:** once ten described folders have kept their text's words, a word
  in more than half of them is common too, in any language, which a fixed list cannot be.
- **Phrases:** two words in a row, nothing but a space or a hyphen between them, count as a phrase
  when the pair is seen twice or more and mostly where its rarer word is ("self assessment", "anne
  smith"); a common word may be half of a phrase ("self") though never a clue alone. A phrase
  replaces its words in the folder's 30 when they are mostly in it. Three words in a row add little
  for the cost, and are not counted.
- **Names:** a word written with a capital where no sentence starts (not after `.`, `!`, `?`, `:` or
  a line break), not common, seen twice, mostly so written, is likely a name; a pair of them is one
  name ("Anne Smith"). `--ask` offers them in the place and person questions ("a name in the text"),
  never marked. A crude finder of names, with no model, for the markers a model would read better.

## Design (2026-10-16): known names, as authority control

What `--ask` finds by counting (a word written as a name, a rare word that keeps going with a tag)
is a suggestion. Once a person confirms it, it should be known: recognised the next time by plain
matching, with no model and no question when nothing is ambiguous. Libraries and archives have done
this for over a century as **authority control** (Wikipedia, "Authority control"): one agreed form
for each person, organisation, place, work and subject, with its variants, kept in authority files
(the Library of Congress's, the national files joined by VIAF; for archives ISAAR(CPF), encoded as
EAC-CPF; for places the Getty TGN and GeoNames), and persistent identifiers (VIAF, ISNI, GND,
Wikidata). It is done by cataloguers, increasingly with a machine suggesting: ePADD finds names in
email archives for the archivist to confirm, OpenRefine reconciles lists against Wikidata. arv's
version is the same loop at one person's scale: counting suggests, a person (or a model, as a
suggestion) confirms, the confirmed entry is matched after; the authority list grows as a side
effect of describing, not as a task of its own.

### An entry

Known names are entries of the tag vocabulary (namespaced `Tag` records in `config/tags.rec`), and
places of the gazetteer (`config/places.rec`, below). The fields follow SKOS, which arv's set
vocabulary already uses (`Parent` for broader):

```
Name: person:anne-smith          the key: made once, never made again from the label
Label: Anne Smith                the preferred form now (SKOS prefLabel)
Alias: Anne                      other forms it is found as: "see" references (altLabel), repeatable
Alias: A. Smith
Qualifier: b. 1931               what tells two of one name apart (dates, "grandmother", a place)
Related: organisation:acme       "see also" (related), repeatable
Same: wikidata:Q...              the same entity elsewhere (exactMatch): wikidata, viaf, isni, geonames
Basis: confirmed by a person in arv describe of 2019-07 Kyoto trip, 2026-10-16
Private: yes                     stays home: never on a disc whose access is wider (see Places)
```

- **The key is an identifier, the label is a name.** Authority files keep the two apart because
  names change (marriage, a pen name, a renamed city or company). A key is made once from the
  first label and kept; a later name becomes the `Label` and the earlier one an `Alias`, and every
  tag already naming the key stays right.
- **The basis** is the authority record's statement of justification: where and when the name was
  confirmed, by whom (a person, or a model's suggestion accepted), in what form.

### What a name can be: the namespaces

A name is not always a person or a place: "KiCad" is software, "Weather station" a project of your
own, "Acme" a company. Libraries control persons, families, corporate bodies, works and places; the
default vocabulary's namespaces become, with four added. Software has its own, not `work`: it is
what a personal archive holds most of among things others made, and what one looks for by name
(and a layout's `software/kicad/` reads better than `work/kicad/`); `work` holds the rest until one
kind is common enough to earn its own:

| Namespace | What | Values |
|---|---|---|
| `person` | someone in it, or who made it | many |
| `family` | a family as a whole (its papers, its house) | many |
| `organisation` | a company, school, club, authority (HMRC) | many |
| `place` | where (the gazetteer) | many |
| `event` | an occasion | one |
| `project` | a piece of your own work | one |
| `software` | a program it is made with or about: KiCad, Blender, a game | many |
| `work` | anything else made by others that it is about: a book, a film, an album | many |
| `source` | what made the files: a camera, a scanner, an account | many |

One form may be several entries ("Jordan" a country and a person; "KiCad" the software, and a word
that goes with the topic electronics): a form names an entry for certain only when it names one
alone. A topic and an entry are linked by `Related` ("software:kicad" related to "electronics"), so
confirming one offers the other.

### When a known name is taken without asking

Recognised by its label or an alias, in the names (a folder named "Anne's 80th") or the text
(written as a name is, as now). Taken without a question, as `automatic` with its reason ("Anne
Smith: known, person:anne-smith, in 5 family folders before"), only when the form is one entry's
alone, it is written as a name where it is found, and its context agrees (the tags its folders had
before go with this folder's, by the counts of words and tags). Otherwise marked and asked: a form
of two entries, a context unlike its others, or an entry confirmed only once. A model's proposals
are entries too, but `suggested` until a person accepts them: a model's mistake must not be
matched for ever after.

### Changing them later

The record is appended to, never edited; entries change as the archive learns, and each change is
an event (`Object: name:KEY`, `Type: metadata modification`), so the record says when an entry was
made, merged, split or renamed, and why, as authority files keep their decisions.

- **Rename** (a new preferred form): `Label` changes, the old one becomes an `Alias`; nothing else.
- **Merge** (two entries were one person; a misspelling, as authority files gather over time):
  every use of the second key moves to the first, each an ordinary tag change (an event with
  `Sets` and `Replaces`), and the second is kept, retired, its key, label and aliases now aliases
  of the first, so anything typed or found as the old later means the new. Moving the uses, not only
  aliasing the key, keeps what is read (find, layouts, discs made later) right without every reader
  having to look through aliases; nothing earlier is rewritten. Likely duplicates (close labels, few
  uses, the same context) are to be offered for merging.
- **Split** (two people of one name, found later): a new entry is made with a `Qualifier` to tell
  them apart, and every place the old key is used (discs' paths, objects) is asked about in turn, in
  the questions' way ("this Anne Smith: b. 1931, or the colleague?"), with the clues of each place
  marked; each answer that moves a use is an ordinary tag change, an event with `Sets` and
  `Replaces`. Uses not yet answered stay with the old entry, which gets its own `Qualifier`. From then
  on the form names two entries, so it is never taken without asking.
- **Retire** (a mistake): the entry is kept, marked `Retired: yes` with why, and no longer offered;
  tags naming it stay readable. Nothing in the vocabulary is deleted, as nothing in the record is.

### Places

The gazetteer is the same kind of entry with a position: `config/places.rec`, `%rec: Place`, with
`Name`, `Label`, `Alias`, `Qualifier`, `Same`, `Basis`, `Private`, and `At` (ISO 6709, WGS84
decimal degrees: `+35.0116+135.7681/`), `Kind` (country, region, city, site), `Parent` (broader:
fushimi-inari, kyoto, japan), `Country` (ISO 3166-1). The default list is Natural Earth's populated
places and countries (public domain, about 150 KB), copied into the home like the other
vocabularies and carried on discs with them. A photo's EXIF GPS is a clue for the nearest place
within reach, never kept in the record itself: the record keeps the place, at the level of a city,
so no catalogue holds where anyone lives to the metre. Mapping is for map tools (`arv places
--geojson`) and an offline outline in the GUI and on each disc's `index.html`.

### Private entries

People's names are personal data, and the vocabulary goes on discs. An entry with `Private: yes`
(the default for `person` and `family`, and for a place you add with exact coordinates) stays in the
home's vocabulary and goes on a disc only when the disc's access is `private` or narrower; a tag
naming it still goes on the disc, as the key alone (`person:anne-smith`), so a disc says no more
than its own tags do.

### Order of building

1. The fields, read and written: `Label`, `Qualifier`, `Related`, `Same`, `Basis`, `Private`,
   `Retired`, and the four new namespaces (`family`, `organisation`, `software`, `work`); `--ask` confirming a name makes or extends an entry, with
   the questions for one form of two entries.
2. Taking a known name without asking, by the rule above.
3. Changing them: `arv names` to list, rename, merge, split and retire, each an event.
4. Places: the gazetteer, EXIF GPS, the GeoJSON export and the map (done, below).
5. Later: export of people and organisations as EAC-CPF; a model proposing entries in bulk.

**Done (2026-10-16): steps 1 to 3.** `src/arv/known.c` reads and edits the entries in
`config/tags.rec` (a field set or added after the record's lines, a new record before the Namespace
records, the file's comments kept) and records each change as an event about `name:KEY` (at home:
snapshots carry the events of the objects and places on them only). `arv tags show|add|label|alias|
merge|split|retire KEY` make the changes; split asks about each use in turn (a disc's path, or a
data object at its own path, as `describe --apply` treats it). `arv describe --ask` makes an entry
when a person confirms a `person`, `family`, `organisation`, `software` or `work` value
(`Private: yes` for people and families), and offers known entries found among a folder's clues
(its names, its text's names and words) by their key's value, label or aliases: marked when the
form is one entry's alone; taken without a question when, besides, the entry was used twice or
more, it is in the names or written as a name, a word of the folder went with it twice before,
and nothing else as strong is offered; never when two entries share the form. A name that is also
a common word (Rose, Grace) counts as a name when it is mostly written with a capital. `arv tags`
shows each entry's label, qualifier and whether it is private or retired; a public disc's
vocabulary leaves the private entries out. The four namespaces are in the default vocabulary.
Not yet: places (step 4), likely duplicates offered for merging, EAC-CPF export.

**Done (2026-10-16): step 4, places.** The gazetteer is `config/places.rec` (`%rec: Place`), its
own file beside `tags.rec` so the vocabulary stays readable: Natural Earth's 242 countries and its
1,251 populated places of the 1:50m scale (about 150 KB; the 1:10m list's 7,000 would be a
megabyte on every disc for villages a person adds better themselves), written by
`dev-tools/places.py`, compiled into arv and copied into a home on first use like the other
vocabularies. Keys come from the English name in ASCII; a name two places share takes the country's
ISO code, then the region (`place:portland-us`, `place:victoria-ca`) and a `Qualifier`, so the form
names two entries and is asked about, never taken. A city's `Parent` is its country; positions are
to two decimals. `known.c` reads both files as one list and writes a place to the gazetteer (unless
`tags.rec` has it); `arv tags add place:KEY` takes `--parent`, `--at` (ISO 6709, or `lat,lon`),
`--kind` and `--country`, and a place given a position is private unless `--public` (it may be a
house). The position is written to the gazetteer, never as an event: the record keeps places, not
coordinates.

`describe --ask` reads the photos' GPS with `exiftool -n` and takes the median, so one photo from
elsewhere does not move it; the nearest place within reach is offered, marked: a place of your own
within 3 km (it is a site), a city within 40 km (the 1:50m list is sparse), else, within 400 km of a
city, its country. The gazetteer's places found in a folder's names are offered marked, those its
text mentions unmarked (a letter naming Lima is not from Lima), each saying where it is ("Lima,
Peru"). A city that is also a word was first left out by the common-words list, but that list holds
London and Paris (they are common in books), so the person sees where instead. A place typed in
answer becomes an entry, in a known place named with it or the city nearest the GPS.

`arv map` lists the places the tags name with their positions (own, or the nearest broader
place's), uses and discs; `--json`, `--geojson` (for QGIS, uMap, geojson.io: Principle 7) and
`--svg`. The picture is an equirectangular outline of Natural Earth's 1:110m land as one path (23
KB, `data/world.txt`), framed to the places shown, dots sized by use; each disc's `index.html` has it
inline when its tags name places (a public disc shows a private place's tag only), and the GUI has a
Map view.

**GPS by outlines and boxes (2026-10-17).** The nearest point within a radius put a photo taken in
Kehl in Strasbourg (the nearest city, across the Rhine) and one far from any city of the sparse
1:50m list in the wrong country, or none. Now: a place of your own may have an `Extent` (a box: two
ISO 6709 corners) and a `Reach` (`2km`); a photo inside the box, or within reach (3 km unless it
says), is there, the smallest box first. Then a city within 40 km, but only of the country the
point is in; then that country. Which country is a point-in-polygon test (a ray's crossings) over
Natural Earth's 1:110m outlines, 96 KB built into arv-assist (`data/countries.txt`, from
`dev-tools/places.py`; arv does not need it), the smallest ring holding the point winning, so
Lesotho is not South Africa.

**The questions in the GUI (2026-10-17).** `--ask` reads a terminal; the GUI's server only runs
commands and returns what they print. So the questions became replayable: `describe --answers`
takes the answers given so far, runs the same questions with them (the flow is the same each time,
given the files and the answers), and stops at the first one without an answer, returning it as
JSON (its options, which are marked, why). A run is dry until no question is left: nothing learned is
kept (examples, rules, known names), and then it runs once more with every answer to keep them, so a
person who leaves halfway leaves nothing behind, and nothing is kept twice. Reading the clues again
each round costs a second or so; keeping a session in the server was the alternative, and the
server knows no features (2026-10-15). Names got a view of their own, split as a list of uses to
tick rather than one question each. A country too small for 1:110m (Singapore, Malta) has no outline: its
cities are trusted whatever outline holds them. Off every outline (a coast drawn coarsely, a small
island), the old rule stays: the country of a city within 400 km, said as "perhaps". A size-scaled
reach for cities (Tokyo is not a town) was left: population is a poor guess at extent, and real city
boundaries are megabytes. Positions given by a person keep four decimals (about 11 m); the
gazetteer's cities stay at two. Not yet: `arv find place:japan` finding what is tagged with places inside Japan; regions
(admin-1) as places; a larger list (GeoNames) as an optional download; offering likely duplicates
for merging; EAC-CPF export.

