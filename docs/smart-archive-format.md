# Smart archive disc format (draft 0.3)

A disc (or any folder, image or drive) that **describes itself**: what it is,
what is on it, what each file is, how to verify it, and what other discs of the
same archive hold. A cataloguing program (for example
[Katalog](https://stephanecouturier.github.io/Katalog/)) can read it and fill
its database immediately, without scanning or hashing the payload, and a
person can read every file with a text editor.

This project (`arv make`) writes discs in this format. The format itself is
independent of the tool.

The layers and how a disc is made and read, as a picture: [architecture.md](architecture.md).

## Design rules

1. **Plain UTF-8 text**, LF line endings, no binary catalogue files. Every
   file can be read, grepped and diffed decades from now without this tool.
2. **Two shapes only:**
   - [GNU recutils](https://www.gnu.org/software/recutils/) *recfiles* for
     records (the disc, events, tag vocabulary): self-describing, with
     `%rec`/`%type`/`%mandatory` schema lines, multi-valued fields and
     multi-line values, validated by `recfix`.
   - **Tab-separated lists** for bulk per-file rows (a million files would be
     too slow as recfiles: measured 28 s and 1.85 GB RAM for `recsel` on 1M
     records, versus sub-second for a TSV scan).
3. **Standards underneath:** the disc is a [BagIt](https://www.rfc-editor.org/rfc/rfc8493)
   bag (checksums and completeness), field names follow Dublin Core, events use
   the PREMIS event-type vocabulary, formats use PRONOM IDs.
4. **Additive evolution:** readers ignore fields and files they don't know;
   new versions only add. A reader checks `Version` major/minor as usual.

## Why not reuse Katalog's own catalogue format

Katalog's files are a good internal cache, and they're text, but they were not
designed as an interchange or archival format:

| Katalog `.idx` / collection files | Why that matters on a disc |
|---|---|
| Absolute paths of the machine that catalogued (`/media/user/disc/...`) | Wrong as soon as the disc is mounted elsewhere; a disc needs paths relative to itself |
| Header lines like `<catalogFileCount>123` (tag-like, never closed) | Ad hoc; no schema, no types, no multi-line values |
| Dates as `yyyy/MM/dd hh:mm:ss` without a time zone | Ambiguous across machines and years |
| Integer IDs local to one collection (`device.csv` `ID`) | Collide between collections; a disc needs a global identity |
| Nowhere for descriptions, notes, provenance events or per-folder captions | These are the point of an archive disc |

So the disc uses recfiles and TSV with a published schema, and a Katalog
importer maps it (see the [mapping](#mapping-to-katalog)). Katalog's `ExternalID`
column in `device.csv` is a natural place for the disc's `Uuid`.

## Discovery

The filesystem is either a hybrid of ISO 9660 (Rock Ridge, Joliet) and UDF 1.02,
readable almost anywhere, or UDF 2.50 only (BD-ROM layout with a metadata
partition; the disc's [`Binding`](#binding-record-recfile) says which). Both show the same files. A UDF-only
disc has no ISO 9660 volume descriptor, so readers take the label from the UDF
logical volume identifier instead.

The volume label starts with the disc `Id`, optionally followed by a space and
the title as far as it fits, so the disc is recognisable in a file manager.
**The disc id is the label's first word.** Older discs have the id alone. The
UDF 2.50 primary volume identifier holds the id alone.

A reader recognises a smart-archive disc by **`catalog.rec` at the root whose
first `Archive` record has `Format: smart-archive`**:

```rec
%rec: Archive
%mandatory: Format Version Disc Uuid

Format: smart-archive
Version: 0.3
Disc: 2020-2025_PROJECTS_01
Uuid: 4f1c2a9e-7b3d-4c55-9e2a-1d0b6f8c3a71
Manifest: manifest-sha256.txt
Listing: catalog/volumes/2020-2025_PROJECTS_01/listing.tsv
Tags: catalog/volumes/2020-2025_PROJECTS_01/tags.tsv
Formats: catalog/volumes/2020-2025_PROJECTS_01/formats.csv
Snapshot: catalog/archive.rec
Viewer: index.html
Payload: data/
```

All paths are relative to the disc root. Pointer fields appear only when the
file exists. `bagit.txt` at the root additionally marks the disc as a BagIt bag.

## Files

| Path | Shape | Contents |
|---|---|---|
| `catalog.rec` | recfile | `Archive` entry record, then this disc's `Disc` and `Binding` records, the `Location` records it refers to, and its `Event` records |
| `bagit.txt`, `bag-info.txt` | BagIt | Bag declaration; `External-Identifier` = disc Id, `External-Description` = title and description, `Bag-Group-Identifier` = set and `Bag-Count` for multi-disc sets, `Payload-Oxum`, and `Bag-Software-Agent` = `arv@<commit> <https://github.com/mofosyne/arv>` |
| `manifest-sha256.txt`, `manifest-sha512.txt` | BagIt manifest | `<hash>  data/<path>`, one per payload file (`sha256sum -c` compatible) |
| `catalog/volumes/<id>/listing.tsv` | TSV | Size, modification time and path of every payload file |
| `catalog/volumes/<id>/tags.tsv` | TSV | Folder tags and optional image captions |
| `catalog/volumes/<id>/formats.csv` | CSV | PRONOM format identification per file (optional) |
| `catalog/volumes/<id>/manifest.sha256` | BagIt manifest | Copy of the disc's `manifest-sha256.txt` |
| `catalog/archive.rec` | recfile | Snapshot of the **whole archive** at burn time: every disc's `Disc`, `Binding`, `Location` and `Event` records (limited by [Access](#access)) |
| `catalog/volumes/<other-id>/` | as above | The same per-volume index files for the other discs in the snapshot: one folder per volume, as LTFS keeps one index per tape |
| `index.html` | HTML | Offline viewer (for people; readers can ignore) |
| `data/` | files | The payload, untouched |

Version 0.1 (samples only, never burned) kept these files by kind instead: `catalog/manifests/<id>.sha256`,
`catalog/listings/<id>.tsv`, `catalog/tags/<id>.tags`, `catalog/formats/<id>.csv`.

### `Disc` record (recfile)

| Field | Meaning | Notes |
|---|---|---|
| `Id` | Human disc id, e.g. `PHOTOS-07_2015-2024_Q` | The first word of the volume label; written on the disc. **Derived**, see [Disc ids](#disc-ids) |
| `Label` | The volume label, when it is more than the id | The id, a space, then as much of the title as fits: 32 bytes on hybrid discs, 126 characters on UDF 2.50, e.g. `TRIP-01_2019_4 Kyoto July 2019` |
| `IdScheme` | Which rule built `Id` | `set-seq-coverage/1`; absent for older discs |
| `Uuid` | Machine identity (UUID v4) of this image | Copies burned from one image share it |
| `Set`, `Sequence` | Set code and number within the set | `Sequence` is never reused within a set |
| `Category`* | Extra vocabulary codes | see [Set vocabulary](#set-vocabulary-a-word-hierarchy) |
| `Path`* | Vocabulary paths of the set and categories | e.g. `MEMORIES/PHOTO/TRIP`; recorded at burn time |
| `Title`, `Description`, `Creator`, `Subject`*, `Coverage`, `Rights` | Dublin Core description | `Subject` repeats; `Coverage` is [EDTF](#coverage-edtf) |
| `Date` | Date the image was made | `YYYY-MM-DD` |
| `Set`, `Part` | Set name and `n of N` for multi-disc sets | |
| `Location`* | Where the disc's copies are kept, one per place | A [`Location`](#location-records) code, or free text; updated later at home |
| `Access` | `public`, `private` (default) or `sealed` | What *other* discs' snapshots may show of this one, see [Access](#access) |
| `Withheld` | What was left out of this record | Only on the cut-down copy of a sealed disc in another disc's snapshot |
| `Note`* | Free-text notes; Q&A from the owner | Multi-line values continue with `+ ` |
| `Files`, `Bytes` | Payload totals | integers |
| `Copies`, `MediaId` | Burned copies and drive-reported media ids | home catalogue only |
| `Software` | Tool and commit that made the disc | |

### Disc ids

The id is a **readable label derived from record fields**, never the only copy
of that information: software regenerates it from `IdScheme`, `Set`,
`Sequence` and `Coverage` and can check it matches.

Scheme `set-seq-coverage/1`:

```
ID       = SET "-" SEQ "_" COVERAGE "_" CHECK
SET      = 2-8 capital letters or digits           PHOTOS
SEQ      = 2-3 digits, per set, never reused        07
COVERAGE = compact form of the EDTF Coverage        2015-2024 | 2019 | 201907-201908 | 199X
CHECK    = Luhn mod 36 check character              Q
```

Examples: `PHOTOS-07_2015-2024_Q`, `TAXES-01_2019_M`, `SCANS-02_199X_K`,
`TRIP-01_201907-201908_P`.

- **Set and number first:** Joliet keeps only 16 characters of the volume label
  (`PHOTOS-07_2015-2`), ISO 9660 and UDF keep 32, so the identifying part comes
  first. Sorting by id also groups a set.
- **Compact coverage:** EDTF intervals `a/b` become `a-b`, months drop their
  dash (`2019-07` → `201907`), qualifiers (`~ ? %`) are dropped. Open intervals
  cannot be used in an id.
- **Check character:** Luhn mod N (N = 36, alphabet `0-9A-Z`) over the letters
  and digits of `SET SEQ COVERAGE`, computed right to left with alternate
  doubling. It catches every single wrong character and every swap of two
  neighbouring characters, which are the usual mistakes when copying an id
  hand-written on a disc. Readers should suggest the closest known id.
- **Older ids** (`2020-2025_PROJECTS_01`, scheme `coverage-set-seq/0`, no
  `IdScheme` field) stay valid; ids written on physical discs never change.

### Set vocabulary (a word hierarchy)

Discs are classified with a controlled, extendable vocabulary of **words**, not
numbers: a code such as `TRIP` is readable on its own, on the disc and in the
id, while a number needs the vocabulary to mean anything. New codes are simply
added; a code is never reused for a different meaning.

The vocabulary is a **directed acyclic graph** (a polyhierarchy, like SKOS
`broader` in library thesauri): an entry may have several parents, and cycles
are rejected.

```
MEMORIES Memories
  PHOTO    Photos
    TRIP     Trips and holidays
    SCAN     Scans                [also under RECORDS]
  VIDEO    Home video
RECORDS  Records
  SCAN     Scans                  [also under PHOTO]
  FINANCE  Finance
    TAXES    Taxes
PROJECTS Projects
  PROJ     Personal projects
  CODE     Software
  ELEC     Electronics
BACKUPS  Backups and exports
  EMAIL    Email and messages
```

(abridged; the default is `src/arv/default_sets.rec`, a recfile with
`Code`, `Name`, `Description`, repeatable `Parent`, optional `Order`, and the
optional fields below.)

| Field | Like | Purpose |
|---|---|---|
| `Alias`* | SKOS `altLabel`, Hydrus tag siblings, Lightroom synonyms | Other words for the entry (`holidays`, `vacation` for `TRIP`). Typed or guessed words resolve to the code, so the vocabulary doesn't drift into near-duplicates. A typed code beats an alias; for folder names an alias wins, so a folder called "Projects" means `PROJ`, not the `PROJECTS` group. Two entries may not share an alias. |
| `ScopeNote` | SKOS `scopeNote` | What belongs here and what goes elsewhere |
| `Match`* | Paperless-ngx matching rules | File or folder name glob (`*.kicad_pcb`, `*.git`); a pattern with `/` is matched against the whole path, case ignored. Codes whose rules claim at least 10% of a folder's files are suggested as set and categories, with no model involved. |

These fields only steer the software that makes discs; readers need nothing
from the vocabulary file, because discs record their `Path`s.

A disc is classified by:

| Field | How many | Purpose |
|---|---|---|
| `Set` | exactly one | the id prefix, numbering and shelf place: `PROJ-03_2020-2025_K` |
| `Category` | any number | further codes the contents belong to, e.g. `CODE`, `ELEC` |
| `Path` | one per vocabulary path | every path from a top-level entry to the set and each category, recorded at burn time: `PROJECTS/PROJ`, `MEMORIES/PHOTO/SCAN`, `RECORDS/SCAN` |

The `Path` fields make the disc self-describing: a reader knows that a disc
belongs to `MEMORIES` without the vocabulary file, and later vocabulary edits
don't reclassify old discs. A disc belongs to a code if the code is its set, one
of its categories, or appears in any of its paths. Codes not in the vocabulary
are allowed (they simply have no `Path`).

Suggested mapping for catalogue software: vocabulary entries become nested
groups (in Katalog: virtual devices), and a disc appears under its set and each
category.

### Location records

Where discs are kept is its own record type in `archive.rec` and in each disc's
`catalog.rec`/snapshot (as in Katalog's storage table or ArchivesSpace's
locations): places form a tree, so moving a box is one edit, not one per disc.

```
%rec: Location
%key: Code

Code: HOME
Name: Home

Code: BOX3
Name: Box 3, blue lid
Parent: STUDY
```

A disc's `Location` fields (one per place its copies are kept, e.g. `BOX3` and
`OFFSITE`) name these codes; any other value is free text. Show a location as
its path of names: `Home / Study / Box 3, blue lid`. A disc carries the
`Location` records it refers to (and the places containing them), so it stays
self-describing; a full snapshot carries all of them.

### Collection records

Virtual folders that you make: named groups of whole discs, folders and files
from **different** discs, e.g. "Best of Kyoto" or "Tax documents 2015-2024".
They work like VVV's virtual folders or Lightroom's collections.

```
%rec: Collection
%key: Code

Code: KYOTO-BEST
Name: Best of Kyoto
Parent: TRAVEL
Description: favourite shots
Item: TRIP-01_2019_4:day2 Kinkaku-ji/
Item: TRIP-01_2019_4:day1 Fushimi Inari/IMG_0100.png
Item: TAXES-01_2019-2020_I
```

| Field | Meaning |
|---|---|
| `Code`, `Name` | key and readable name |
| `Parent` | the collection it is inside (collections form a tree) |
| `Item`* | `DISC-ID` (a whole disc), `DISC-ID:folder/` (a folder: trailing `/`) or `DISC-ID:folder/file`; paths relative to `data/`. Disc ids contain no `:` |

Collections live in `archive.rec` and travel in snapshots, limited by
[Access](#access):
- a snapshot keeps only items on discs it carries;
- items with paths on **sealed** discs are dropped (the whole-disc item stays);
- collections left empty are dropped.

When merging, readers **union** a collection's items and never remove any,
because a snapshot's copy may be filtered.

### Access

`Access` decides what other discs' catalogue snapshots may carry of a disc (the
disc itself always carries its own full record):

| Value | Full snapshot (your own discs) | Set snapshot (discs for other people) |
|---|---|---|
| `public` | full record and file lists | full record and file lists |
| `private` (default; also when absent) | full record and file lists | left out |
| `sealed` | identity only: `Id`, `Uuid`, `Set`, `Category`, `Path`, `Sequence`, `Coverage`, `Date`, `Part`, `Location`, `Copies`, `Access`; `Title` is `(sealed disc)`, plus a `Withheld` field; no events or file lists | left out |

A reader merging snapshots must never replace a full record with one that has
`Withheld`.

### Coverage (EDTF)

`Coverage` uses the Library of Congress
[Extended Date/Time Format](https://www.loc.gov/standards/datetime/) (ISO 8601-2),
which can express the uncertainty common in personal archives:

| Value | Meaning |
|---|---|
| `2019` | the year 2019 |
| `2015/2024` | 2015 to 2024 |
| `2019-07/2019-08` | July to August 2019 |
| `2019-07-14/2019-07-20` | 14 to 20 July 2019 (a trip) |
| `2019/..` | from 2019, end open |
| `199X` | some time in the 1990s |
| `1995~` | about 1995 |
| `[1998,1999]` | 1998 or 1999 |

Readers turn this into a date span (uncertain digits widen it: `199X` is
1990-01-01 to 1999-12-31) and can answer "which discs cover 15 July 2019?"
without reading any listings. The id carries at most year and month; the
record keeps full precision.

### `Event` records (recfile)

`Disc` (or, for a change to a place or a collection, `Object`: `location:CODE`,
`collection:CODE`), `Type` (PREMIS event type: `message digest calculation`, `creation`,
`fixity check`, `format identification`, `metadata modification`,
`replication`), `Date`, `Outcome` (`success` / `failure` / `warning`), `Agent`
(software, `human:LOGIN` for a hand edit, or `llm:<model> + owner review`), optional `Note`.

Events are appended, never edited. **Every change to the catalogue leaves one:** a note, an
access level, where a disc is kept, a place or a collection added, moved or renamed
(`metadata modification`, with what changed in `Note`, e.g. `Access: private -> public`).
Collection events give counts, never item paths, so they reveal nothing about sealed discs.
A disc's events follow its access level; events with an `Object` go only into full
snapshots, for the places and collections that snapshot carries.

### Listing TSV

```
# smart-archive listing 1	size (bytes)	modified (UTC, ISO 8601)	path (relative to data/)
4839201	2019-07-14T09:12:03Z	photos/2019 trip/IMG_0001.JPG
```

- The first line starts with `# smart-archive listing <version>` and names the columns.
- **The path is always the last column** and may contain tabs; split on the
  first N-1 tabs only. Paths never contain CR or LF (the writer refuses such
  names), use `/`, and are relative to `data/`.
- Checksums are in the BagIt manifest (join on `data/` + path).

### Tags TSV

```
# folder (relative to data/)	tags	caption (what sampled images show, if analysed)
photos/2019 trip	travel, japan	People at a temple gate in autumn.
```

Folder paths use `/`, relative to `data/`; `.` is the payload root. Tags are
comma-separated and lower case. The caption column is optional.

A tag is either a plain word or phrase (`travel`) or `namespace:value`
(`person:alice`, `place:kyoto`, `event:wedding-2019`, `source:pixel-7`,
`project:weather-station`), as in Hydrus. Namespaces keep facets apart: who,
where, which occasion, which device. Suggested namespaces are `person`,
`place`, `event`, `source` and `project`; any single word works. The
hierarchical keyword form used by XMP `lr:hierarchicalSubject` (Lightroom,
digiKam) replaces the colon with `|`: `place|kyoto`; set paths become
`MEMORIES|PHOTO|TRIP`.

## Reading a disc: suggested algorithm

1. Find `catalog.rec`; read its `Archive` record. Stop if `Format` is not `smart-archive`.
2. Look up the disc by `Uuid` in your database. Create it if new, or update it
   (this disc may be a copy, or re-read later).
3. Import the `Disc` record fields (title, description, notes, location...) and events.
4. Import the listing: path, size, modification time. Join the SHA-256 from the
   manifest if you store checksums. **No scan or hashing of `data/` needed.**
5. Import folder tags and captions; optionally PRONOM formats.
6. Optionally read `catalog/archive.rec` plus the other discs' listings, and
   create entries for discs that are **not** inserted (marked offline), so a
   single disc restores a whole archive's catalogue. Merge `Location` and
   `Collection` records too (union collection items).
7. Optionally verify: `Payload-Oxum` in `bag-info.txt` for a quick completeness
   check; the manifests for a full one.

## Building a virtual file system from the catalogue

A catalogue program can present the whole archive as a browsable or mountable
tree (for example with FUSE) without any disc inserted. Everything it needs is
in `catalog/archive.rec` and the listings of the newest disc. Useful trees:

| Tree | Built from | Example path |
|---|---|---|
| By disc | each disc's listing | `TRIP-01_2019_4/day1 Fushimi Inari/IMG_0100.png` |
| By kind | `Disc.Path` (vocabulary paths), then by disc | `MEMORIES/PHOTO/TRIP/TRIP-01_2019_4/...` |
| By place | `Disc.Location` + `Location` tree | `Home/Study/HOME-PUB-2020/TRIP-01_2019_4/...` |
| By date | `Disc.Coverage` (EDTF), or each file's modified time from the listing | `2019/07/TRIP-01_2019_4/...` |
| By collection | `Collection` tree and `Item`s | `Travel/Best of Kyoto/day2 Kinkaku-ji/...` |
| By tag | Tags TSV (`namespace:value`) | `place/kyoto/TRIP-01_2019_4/...` |

What each entry can show:
- **Files:** size and modified time (listing); SHA-256 (manifest); PRONOM format (formats CSV, optional).
- **Folders:** implied by the file paths. Empty folders are not listed.
- **Discs:** where their copies are (location path), so opening a file can say "insert TRIP-01_2019_4, kept in Home / Study / Public, made 2020".

A disc in several categories, places or collections appears in each tree under
each of them. That is the point of a DAG vocabulary.

## Mapping to Katalog

Based on Katalog's source (collection files `device.csv`, `storage.csv`,
`tags.csv`, one `.idx` per catalogue):

| Smart archive | Katalog |
|---|---|
| `Archive.Uuid` / `Disc.Uuid` | `device.csv` `ExternalID` |
| `Disc.Title` (or `Id`) | `device.csv` `Name` |
| `Disc.Set`, `Category`, `Path` | nested `Virtual` devices following the vocabulary paths |
| `Disc.Id` (volume label) | `storage.csv` `Label` |
| `Disc.Location` + `Location` records | `storage.csv` `Location` (the readable path, e.g. `Home / Study / Box 3`) |
| `Disc.Access` = `sealed` | import the identity only (or skip the disc) |
| `Binding.Filesystem`, `Media` | `storage.csv` `FileSystem`, `Type` / `Comment` |
| `Disc.Description`, `Note` | `storage.csv` `Comment` (or a new notes table) |
| Listing TSV row | `.idx` row: `<mount>/data/<path>`, size, date converted from UTC to `yyyy/MM/dd hh:mm:ss` |
| Manifest SHA-256 | Katalog's checksum column (catalogue with checksums enabled) |
| Tags TSV row | `tags.csv`: one tag per (tag, folder path) with type folder; `namespace:value` kept as the tag name |
| `catalog/archive.rec` other discs | additional devices marked as not connected |
| `Collection` records | virtual folders, if Katalog adds them (no direct equivalent today; folder tags named after the collection come closest) |

Up to version 0.2 the Disc record also held `Media`, `Filesystem` and `Ecc`; they are now in
the disc's `Binding`. Readers look in the Binding first, then in the Disc record.

### `Binding` record (recfile)

How one volume is stored on its medium. The files and the rest of the catalogue never depend
on the medium; the container and its protection do, so they are kept here, apart from the
`Disc` record (four layers: content, description, container, protection; plan.md). A new
medium needs a new kind of Binding, not a new format.

```rec
%rec: Binding
%key: Volume
%mandatory: Volume Container Protection

Volume: TRIP-01_2019_4
Container: udf-2.50
Protection: rs03
Media: M-DISC BD-R 25GB
Filesystem: UDF 2.50, BD-ROM layout with metadata partition and a real mirror (arv udfwrite)
Ecc: dvdisaster RS03 augmented image, BD-R 25GB (12219392 sectors), minimum 20% redundancy
MediumSectors: 12219392
```

| Field | Meaning |
|---|---|
| `Volume` | The disc `Id` |
| `Container` | How the volume is laid out: `iso9660+udf-1.02` (hybrid) or `udf-2.50`; later perhaps `ltfs`, `exfat`, `tar`, `afs` |
| `Protection` | Error correction around the container: `rs03` (dvdisaster augmented image) or `none` |
| `Media`, `Filesystem`, `Ecc` | The same, described for people |
| `MediumSectors` | The medium size RS03 was computed for, in 2048-byte sectors |

Planned: an optional `Extents` pointer to `catalog/volumes/<id>/extents.tsv` (path, start and
length of each file in the container's units). It is kept in the home catalogue and in later
discs' snapshots, not on the volume it describes (a file inside the image can only be found
through the tree it would replace; the disc's own map is the UDF metadata and its mirror), so a
volume damaged beyond repair still has its map on its siblings, as Piql's AFS table of contents
travels apart from the data.

Things Katalog would have no place for today (candidates for its developer):
events (provenance history), multi-line notes, image captions, PRONOM IDs,
and set/part numbering beyond a virtual-device parent.

## Versioning

- `Version: 0.3` is a draft; field names may still change before `1.0`. 0.2 grouped
  per-volume files by volume; 0.3 moved the medium's fields into `Binding`.
- Minor versions only add optional fields or files. A major version bump means
  a reader must not assume the old layout.
- Listing and tags files carry their own header version (`listing 1`).
