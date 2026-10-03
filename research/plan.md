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
- [x] Generated `archive.sqlite` (`arv index`); `find` uses it when fresh
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
- [x] NetBSD `makefs -t udf` builds on Linux as a C library: `src/udfmake/` (UDF 2.50,
      metadata partition, two upstream bugs fixed).
- [ ] Confirm `src/udfmake/upstream/BUG-REPORT.md` by hand, then send it to NetBSD.
- [ ] Metadata mirror duplication in udfmake (upstream lacks it).
- [x] `arv make --filesystem udf250` using it (the default since 2026-10-03).
- [ ] Test the images with a Linux kernel mount, Windows and macOS.
- [ ] Standalone RS03 library (see below). Needs the licence decision first.

## Decisions (2026-09-29)

- CLI: **Python, standard library only**.
- Disc ids: **year range of the files + set + number**, e.g. `2020-2025_PROJECTS_01`.
- Discs for other people or external parties: **minimal catalogue**
  (`--snapshot set`, only this set). Own off-site copies can take `full`.
- Encryption: optional later, not implemented now (direction: see Decisions 2026-10-01).
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
  - the formats are the contract: `docs/smart-archive-format.md` is the spec, not the Python code;
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
- **The profile is written down** in `docs/archival-udf.md` (layout, rules, how to check a disc).
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
- **Encryption, when it comes (not now): sealed discs only, opt-in.** The disc stays a normal
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
