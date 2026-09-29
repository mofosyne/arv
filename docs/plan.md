# Plan

Working plan for turning the scripts into an `archive` tool. Background is in
`research-notes.md` and `metadata-standards.md`.

## Principles

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
index.html   search.html   README.txt          ← open these first
bagit.txt  bag-info.txt  manifest-*.txt  tagmanifest-*.txt
catalog.rec                                    this disc: Disc/Copy/Event records
catalog/                                       snapshot of the whole archive at burn time
  archive.rec
  manifests/<disc-id>.sha256
  web/<disc-id>.js                             lazy-loaded data for search.html
  archive.sqlite                               optional convenience copy
tools/
  bluray-archival-workflow/                    uncompressed copy of this repo
  bluray-archival-workflow.bundle              git bundle (full history)
  dvdisaster/                                  source tarball + static Linux/Windows binaries
  bagit.py
data/                                          payload (embedded EXIF/XMP untouched)
[dvdisaster RS03 ECC after the filesystem]
```

All non-`data/` files are BagIt tag files, covered by the tagmanifests.

## On-disc copy of this repo

- Uncompressed tree (browsable with no tools) + `git bundle` (history,
  `git bundle verify`). No tar.gz: a stream is fragile after the first bad byte.
- The commit hash is recorded in `catalog.rec` as the agent of the creation event.
- Keep the CLI **Python standard library only** (vendor `bagit.py`) so the
  on-disc copy runs without `pip`.

## On-disc HTML viewer

- `index.html`: static, no JavaScript. Disc description, notes, location, a
  folder tree with relative links into `data/`, and sizes and checksums.
- `search.html`: vanilla JS with no dependencies, working from `file://`. Data
  is loaded with `<script src>` (not `fetch`, which `file://` blocks). It
  searches across the snapshot, loading `catalog/web/<disc-id>.js` per disc on
  demand.
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
- [x] `archive make <folder>`: bag → prompt for Title/Description/Notes/Subject →
      `catalog.rec` → `tools/` → `index.html` → image → RS03 → `dvdisaster -t`
- [x] Home catalogue `archive.rec` (Disc/Copy/Event), short disc IDs, `Location`
- [x] `archive find <pattern>` (plain scan of manifests to start)
- [x] `archive note <disc-id> <text>` (+ `archive locate`)
- [x] Tests (`tests/test_archivetool.py`)

### Phase 2: whole-archive retrieval
- [x] `catalog/` snapshot with `--snapshot full|set|none`
- [ ] Snapshot hash chain (`Previous-Snapshot-Sha256`)
- [ ] `search.html` across the snapshot
- [ ] Generated `archive.sqlite`; `archive find` uses it when present
- [ ] `archive check <disc-id>`: `dvdisaster -t` on an inserted disc → `fixity check` Event
- [ ] `archive rebuild <disc>`: recreate the home catalogue from the newest disc
- [ ] Import from VVV exports

### Phase 3: scale and standards
- [ ] Split a source folder across N discs (`Bag-Count: n of N`, `Bag-Group-Identifier`)
- [ ] Medium sizing (BD 25/50/100), `--no-bdr-defect-management`
- [ ] Siegfried/PRONOM `formats.yaml`, RO-Crate export

### Phase 4: extras
- [ ] GUI over the CLI
- [ ] Optional UDF 2.50 metadata mirror through NetBSD `makefs -t udf`

## Decisions (2026-09-29)

- CLI: **Python, standard library only**.
- Disc ids: **year range of the files + set + number**, e.g. `2020-2025_PROJECTS_01`.
- Discs for other people or external parties: **minimal catalogue**
  (`--snapshot set`, only this set). Own off-site copies can take `full`.
- Encryption: optional later, not implemented now.
- Media: **M-DISC BD-R** as standard.
- Redundancy: **RS03 inside every image + whole extra copies** rather than
  off-disc parity files. Burn copies from the **same ISO** so they are
  sector-identical: dvdisaster can then fill an image's unreadable sectors by
  reading another copy into the same image file before applying RS03. Off-disc
  RS01 `.ecc` files are only worth it for old discs burned without ECC.
- Manifest paths are written unencoded (bagit-python and `sha256sum -c`
  compatible). Names with CR/LF or a literal `%0A`/`%0D`/`%25` are rejected.

## Separate track: standalone RS03 library

Neither dvdisaster nor the speed47 fork has a library or API; it is one GPLv3 C
program. The RS03 on-disc format is effectively frozen, so a library is
practical: extract the RS03 encoder/decoder from speed47's `src/`, keep
bit-for-bit compatibility (tested against the dvdisaster CLI), and expose
create/verify/repair. The result stays GPLv3, which constrains the licence
of anything linking to it (this repo's licence is still undecided).

## Open decisions

- Licence for this repo (GPLv3 fits if the RS03 library happens)
- Number of copies per disc by default, and a `Copy` record type to track them
- GUI toolkit (Tk from the stdlib vs. a local web UI)
