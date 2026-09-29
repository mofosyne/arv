# Smart archive disc format (draft 0.1)

A disc (or any folder, image or drive) that **describes itself**: what it is,
what is on it, what each file is, how to verify it, and what other discs of the
same archive hold. A cataloguing program (for example
[Katalog](https://stephanecouturier.github.io/Katalog/)) can read it and fill
its database immediately, without scanning or hashing the payload, and a
person can read every file with a text editor.

This project (`archive make`) writes discs in this format. The format itself is
independent of the tool.

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

A reader recognises a smart-archive disc by **`catalog.rec` at the root whose
first `Archive` record has `Format: smart-archive`**:

```rec
%rec: Archive
%mandatory: Format Version Disc Uuid

Format: smart-archive
Version: 0.1
Disc: 2020-2025_PROJECTS_01
Uuid: 4f1c2a9e-7b3d-4c55-9e2a-1d0b6f8c3a71
Manifest: manifest-sha256.txt
Listing: catalog/listings/2020-2025_PROJECTS_01.tsv
Tags: catalog/tags/2020-2025_PROJECTS_01.tags
Formats: catalog/formats/2020-2025_PROJECTS_01.csv
Snapshot: catalog/archive.rec
Viewer: index.html
Search: search.html
Payload: data/
```

All paths are relative to the disc root. Pointer fields appear only when the
file exists. `bagit.txt` at the root additionally marks the disc as a BagIt bag.

## Files

| Path | Shape | Contents |
|---|---|---|
| `catalog.rec` | recfile | `Archive` entry record, then this disc's `Disc` record and its `Event` records |
| `bagit.txt`, `bag-info.txt` | BagIt | Bag declaration; `External-Identifier` = disc Id, `Bag-Group-Identifier` / `Bag-Count` for multi-disc sets |
| `manifest-sha256.txt`, `manifest-sha512.txt` | BagIt manifest | `<hash>  data/<path>`, one per payload file (`sha256sum -c` compatible) |
| `catalog/listings/<id>.tsv` | TSV | Size, modification time and path of every payload file |
| `catalog/tags/<id>.tags` | TSV | Folder tags and optional image captions |
| `catalog/formats/<id>.csv` | CSV | PRONOM format identification per file (optional) |
| `catalog/archive.rec` | recfile | Snapshot of the **whole archive** at burn time: every disc's `Disc` and `Event` records |
| `catalog/{manifests,listings,tags,formats}/<other-id>.*` | as above | The same per-file data for the other discs in the snapshot |
| `index.html`, `search.html` | HTML | Offline viewer and search (for people; readers can ignore) |
| `data/` | files | The payload, untouched |

### `Disc` record (recfile)

| Field | Meaning | Notes |
|---|---|---|
| `Id` | Human disc id, e.g. `PHOTOS-07_2015-2024_Q` | Also the volume label; written on the disc. **Derived**, see [Disc ids](#disc-ids) |
| `IdScheme` | Which rule built `Id` | `set-seq-coverage/1`; absent for older discs |
| `Uuid` | Machine identity (UUID v4) of this image | Copies burned from one image share it |
| `Set`, `Sequence` | Set code and number within the set | `Sequence` is never reused within a set |
| `Title`, `Description`, `Creator`, `Subject`*, `Coverage`, `Rights` | Dublin Core description | `Subject` repeats; `Coverage` is [EDTF](#coverage-edtf) |
| `Date` | Date the image was made | `YYYY-MM-DD` |
| `Set`, `Part` | Set name and `n of N` for multi-disc sets | |
| `Location` | Where the disc is kept | Free text; may be updated later at home |
| `Note`* | Free-text notes; Q&A from the owner | Multi-line values continue with `+ ` |
| `Media`, `Filesystem`, `Ecc` | Physical and technical description | e.g. `M-DISC BD-R 25GB`, RS03 details |
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

### Coverage (EDTF)

`Coverage` uses the Library of Congress
[Extended Date/Time Format](https://www.loc.gov/standards/datetime/) (ISO 8601-2),
which can express the uncertainty common in personal archives:

| Value | Meaning |
|---|---|
| `2019` | the year 2019 |
| `2015/2024` | 2015 to 2024 |
| `2019-07/2019-08` | July to August 2019 |
| `199X` | some time in the 1990s |
| `1995~` | about 1995 |
| `[1998,1999]` | 1998 or 1999 |

Readers can turn this into a year range and answer "which discs cover 2019?"
without reading any listings.

### `Event` records (recfile)

`Disc`, `Type` (PREMIS event type: `message digest calculation`, `creation`,
`fixity check`, `format identification`, `metadata modification`,
`replication`), `Date`, `Outcome` (`success` / `failure` / `warning`), `Agent`
(software or `llm:<model> + owner review`), optional `Note`.

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
   single disc restores a whole archive's catalogue.
7. Optionally verify: `Payload-Oxum` in `bag-info.txt` for a quick completeness
   check; the manifests for a full one.

## Mapping to Katalog

Based on Katalog's source (collection files `device.csv`, `storage.csv`,
`tags.csv`, one `.idx` per catalogue):

| Smart archive | Katalog |
|---|---|
| `Archive.Uuid` / `Disc.Uuid` | `device.csv` `ExternalID` |
| `Disc.Title` (or `Id`) | `device.csv` `Name` |
| `Disc.Set` | a parent `Virtual` device grouping the set's discs |
| `Disc.Id` (volume label) | `storage.csv` `Label` |
| `Disc.Location` | `storage.csv` `Location` |
| `Disc.Filesystem`, `Media` | `storage.csv` `FileSystem`, `Type` / `Comment` |
| `Disc.Description`, `Note` | `storage.csv` `Comment` (or a new notes table) |
| Listing TSV row | `.idx` row: `<mount>/data/<path>`, size, date converted from UTC to `yyyy/MM/dd hh:mm:ss` |
| Manifest SHA-256 | Katalog's checksum column (catalogue with checksums enabled) |
| Tags TSV row | `tags.csv`: one tag per (tag, folder path) with type folder |
| `catalog/archive.rec` other discs | additional devices marked as not connected |

Things Katalog would have no place for today (candidates for its developer):
events (provenance history), multi-line notes, image captions, PRONOM IDs,
and set/part numbering beyond a virtual-device parent.

## Versioning

- `Version: 0.1` is a draft; field names may still change before `1.0`.
- Minor versions only add optional fields or files. A major version bump means
  a reader must not assume the old layout.
- Listing and tags files carry their own header version (`listing 1`).
