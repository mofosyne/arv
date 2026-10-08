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
- [ ] Per-copy tracking via the BD-R BCA serial (deferred, low priority)
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
- **A name clash**: today's `Collection` records are virtual folders across discs ("Best of
  Kyoto"). They become `Selection` (format 0.5; readers accept both), so "collection" means
  the thing in a workflow folder.
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
