# Digital archive metadata standards

A survey of existing standards for describing archived digital material. It
covers the institutional stack (national libraries, archives, research data)
and what a personal archive can realistically take from it, and ends with a
proposed profile for this project.

## 1. Metadata types

Almost every standard slices metadata the same way. That makes it easier to see
which standard covers what:

| Type | Question it answers | Example fields |
|---|---|---|
| **Descriptive** | What is it? Who, when, about what? | title, creator, date, subject, description |
| **Technical** | What exactly are the bits? | file format (PRONOM ID), size, codec, resolution, checksum |
| **Structural** | How do the parts fit together? | disc 2 of 5, folder = album, bundle = repo |
| **Preservation / provenance** | What has happened to it, and who did it? | created, fixity checked, copied, format-migrated |
| **Rights** | Who may use it? | licence, copyright holder, access restrictions |
| **Embedded** | Metadata stored *inside* the file itself | EXIF/XMP in photos, tags in audio/video, git history |

## 2. The frameworks (process, not file formats)

| Standard | What it is | Relevance |
|---|---|---|
| **OAIS**, ISO 14721 | Reference model for an archive: SIP (what is submitted) → AIP (what is stored) → DIP (what is delivered) | Vocabulary only. In our case each disc is an AIP. |
| **NDSA Levels of Digital Preservation** v2.0 (2019), v2.1 (2026) | Self-assessment matrix: 5 areas (Storage, Integrity, Control, Metadata, Content) × 4 levels | **Very useful for personal archives.** A practical checklist. |

NDSA Metadata row (paraphrased from v2.0; v2.1 lightly reworded levels 1–2):
1. Keep an **inventory** of content and where it is stored, and keep a copy of
   the inventory away from the content.
2. Store **enough metadata to know what the content is**.
3. **Choose metadata standards**, and find and fill gaps against them.
4. **Record preservation actions** and when they happened, and implement the
   chosen standards.

The design so far reaches level 2 (BagIt manifests + recfile catalogue).
Levels 3–4 are covered by picking the standards below and adding an event log.

## 3. Institutional standards

### Descriptive

| Standard | Maintainer | Notes |
|---|---|---|
| **Dublin Core** (DCMI Terms, ISO 15836) | DCMI | 15 core elements (Title, Creator, Subject, Description, Publisher, Contributor, Date, Type, Format, Identifier, Source, Language, Relation, Coverage, Rights) plus refined `dcterms:`. The common denominator that almost everything maps to. **Best fit for personal use.** |
| **MODS** | Library of Congress | Richer XML bibliographic description, a middle ground between Dublin Core and MARC |
| **MARC 21** / **BIBFRAME** | Library of Congress | Library catalogue records. BIBFRAME (linked data) is replacing MARC. Not relevant unless you catalogue books. |
| **EAD** (Encoded Archival Description) + **ISAD(G)** | SAA / ICA | Hierarchical *finding aids* (fonds → series → file → item). Standard in archives for collections. |
| **Records in Contexts** (RiC-CM / RiC-O) | ICA | Successor to ISAD(G)/EAD-style description as a graph (OWL ontology). RiC-O 1.0 released Dec 2023, 1.1 in May 2025. Heavy, but it is the direction archives are moving. |
| **PBCore** | US public media | Audiovisual collections |
| **schema.org** | W3C community | Web vocabulary. Used by RO-Crate. |

### Preservation, technical and structural

| Standard | Maintainer | Notes |
|---|---|---|
| **PREMIS** 3.0 (2015, still current) | Library of Congress | *The* preservation metadata standard. Entities: **Object, Event, Agent, Rights, Intellectual Entity**. Its **controlled vocabularies** at id.loc.gov (event types such as `fixity check`, `replication`, `migration`, `ingestion`) are easy to reuse even without PREMIS XML. |
| **METS** | Library of Congress | XML wrapper that ties descriptive, PREMIS, technical and structural metadata and a file inventory into one document. Used by Archivematica and E-ARK. Verbose. |
| **PRONOM** | UK National Archives | Registry of file formats with persistent IDs (e.g. `fmt/43` = JPEG 1.01). Identified with **DROID** or **Siegfried** (`sf`). Answers "what format is this, exactly?" decades later. |
| **DFXML** | Forensics community | Filesystem- and disk-image-level metadata (`fiwalk`) |
| **XMP** (ISO 16684) / **EXIF** / **IPTC** | Adobe/ISO, CIPA, IPTC | **Embedded** photo/video metadata. Preserve it (never strip it). XMP sidecars hold edits to RAW files. |

### Packaging and layout

| Standard | Notes |
|---|---|
| **BagIt** (RFC 8493) | Fixity + minimal key/value info. Transfer/packaging, not description. |
| **E-ARK CSIP / SIP / AIP / DIP** (v2.2, 2024) | EU profile of *METS + PREMIS in a fixed folder layout*. The most complete "standard archival package" spec. Validator: Commons-IP. Heavy for personal use. |
| **OCFL** 1.1 | Versioned object layout (`inventory.json`, `v1/`, `v2/`...) for storage on disk. Aimed at mutable storage, so a poor fit for write-once discs. |
| **RO-Crate** 1.2 (June 2025) | Folder + one `ro-crate-metadata.json` (JSON-LD, schema.org) + optional human-readable `ro-crate-preview.html`. Lightweight, designed to **combine with BagIt**, and understood by research-data repositories (Zenodo, WorkflowHub, Galaxy). |
| **WARC** (ISO 28500) | Container for captured websites. It is a payload type, not a description format. |

## 4. What institutions actually run

- **Libraries and archives:** OAIS model, stored as **METS + PREMIS** (often
  E-ARK or an Archivematica AIP), wrapped in **BagIt** for transfer and
  storage, with **Dublin Core/MODS/EAD** description and **PRONOM** format IDs.
- **Research data:** BagIt + **RO-Crate** or DataCite metadata.
- **Common pattern:** a thin, well-specified package (BagIt) plus a rich
  description in a separate standard file, with the searchable catalogue in a
  database (usually generated from those files). That is the same split this
  project already uses.

## 5. Existing disc-cataloguing software

A separate category of desktop "disc catalogers" exists. They scan a mounted
disc and store file names, sizes, dates (and sometimes thumbnails or EXIF) in
the app's **own database**, so you can search without inserting the disc.

| Tool | Platform | Licence | Catalogue storage |
|---|---|---|---|
| **VVV** (Virtual Volumes View) 1.5 | Linux / Windows / macOS | Open source | Embedded database file; catalogues from 1.5 need a one-time conversion for newer builds |
| **cdcat** | Linux / Windows / macOS (Qt) | Open source | Its own catalogue file |
| **NeoFinder** (formerly CDFinder) | macOS / Windows | Commercial | Proprietary; strong photo/video/XMP support |
| **DiskCatalogMaker** | macOS | Commercial | Proprietary |
| **Cathy**, **WinCatalog** | Windows | Freeware / commercial | Proprietary |
| **Snap2HTML** | Windows | Open source | Self-contained HTML file listing |

Institutional equivalents: **Archivematica** (ingest → METS/PREMIS AIPs),
**DROID** / **Siegfried** (format identification) and **Brunnhilde**
(Siegfried-based collection reports).

Implications for this project:
- **All of these index by scanning the mounted filesystem**, so any disc made
  here is compatible with all of them automatically. None of them read BagIt,
  recfiles or RO-Crate.
- **Their catalogues are app-specific files that live off-disc.** If the app
  dies, so does the index. That is the gap the on-disc plain-text manifests and
  `catalog.rec` fill: any future tool can rebuild an index from the disc itself.
- They are still worth using as a **convenience front end**. Rebuilding a
  VVV/NeoFinder catalogue from the discs is always possible.
- The `archive.sqlite` planned here is the same idea as these apps' databases,
  but it is generated from open, on-disc sources rather than being the only copy.

## 6. What a personal archive should take from this

Adopt the **vocabularies** (field names, event types, format IDs) and not the
**XML machinery** (METS, EAD, E-ARK). The vocabularies are what make the data
interpretable later. The machinery exists to exchange records between
institutions.

| Need | Adopt | Skip |
|---|---|---|
| Inventory + fixity (NDSA L1) | BagIt manifests | — |
| Description (NDSA L2) | Dublin Core term names | MARC, MODS, EAD, RiC |
| Format identification | PRONOM IDs via Siegfried | — |
| Preservation events (NDSA L4) | PREMIS event-type vocabulary | PREMIS XML, METS |
| Embedded metadata | Keep EXIF/XMP intact | — |
| Machine-readable description | Optional generated **RO-Crate** | E-ARK |
| Versioning | Not needed on write-once media | OCFL |

## 7. Proposed profile for this project

### Source of truth: recfiles, with field names borrowed from standards

```rec
%rec: Disc
%doc: One per physical disc (= OAIS AIP). Fields follow dcterms where possible.
%key: Id
%mandatory: Id Title Date
%type: Date date

Id: 2025-01-13_Projects_2020_-_2025
Title: Projects 2020 - 2025
Creator: A. Person
Date: 2025-01-13
Description: Source code and design files for personal projects
Subject: electronics
Subject: firmware
Rights: All rights reserved
IsPartOf: PROJECTS-2025
Index: 1
Count: 3

%rec: Binding
%doc: How the volume is stored on its medium (container, protection), kept apart from the Disc.

Volume: 2025-01-13_Projects_2020_-_2025
Container: iso9660+udf-1.02
Protection: rs03
Media: BD-R 25GB

%rec: Event
%doc: Preservation actions. Type uses the PREMIS eventType vocabulary.
%type: Date date
%type: Outcome enum success failure warning
%mandatory: Disc Type Date Outcome

Disc: 2025-01-13_Projects_2020_-_2025
Type: fixity check
Date: 2026-09-29
Agent: dvdisaster 0.79.10
Outcome: success
Note: 0 unreadable sectors, ECC blocks pass
```

Event types to start with (all from the PREMIS vocabulary): `creation`,
`ingestion`, `message digest calculation`, `fixity check`, `replication`
(burned another copy), `migration` (copied to new media or converted format),
`deaccession` (disc destroyed or discarded), and `metadata modification` for every hand edit
of the catalogue (Agent `human:LOGIN`; see smart-archive-format.md, Event records).

### Per-file data: plain text on the disc, search in SQLite

- Per-file records stay in the BagIt manifests (and optionally a Siegfried
  report `formats.yaml` with PRONOM IDs) as **tag files** on each disc.
- recutils does not scale to per-file records. Measured: `recsel` needs 28 s
  and 1.85 GB RAM for 1M records, and `recfix` with `%key` is roughly quadratic
  (5k records: 4 s, 20k: 53 s).
- A home-side `archive.sqlite` is **generated** from the recfiles + manifests
  for fast "which disc holds X" queries. It is deletable and rebuildable.

### Optional export: RO-Crate for autodetection and indexing

Generate `ro-crate-metadata.json` (+ `ro-crate-preview.html`) at the bag root
from the `Disc` record. This covers the original goal of "autodetected and
indexed by archival software" without making JSON-LD the thing you edit by
hand. RO-Crate 1.2 documents how to combine it with BagIt.

### Resulting disc root

```
bagit.txt  bag-info.txt  manifest-*.txt  tagmanifest-*.txt   BagIt: fixity
catalog.rec                                                   Dublin Core-style description + events at burn time
formats.yaml                                                  Siegfried/PRONOM format IDs (optional)
ro-crate-metadata.json  ro-crate-preview.html                 generated, machine-readable (optional)
catalog/                                                      cumulative catalogue snapshot of all earlier discs
README.txt                                                    plain-English recovery instructions
data/                                                         payload (embedded EXIF/XMP untouched)
[dvdisaster RS03 ECC after the filesystem]
```

### Cumulative catalogue snapshot on every disc

Each disc also carries a snapshot of the **whole** catalogue as it stood at
burn time, so the newest disc indexes every earlier disc. Losing the home
machine then only means inserting the latest disc, with no re-scanning. Tape
backup systems do the same with their catalogues (e.g. Bacula, Amanda).

```
catalog/                         BagIt tag directory (covered by tagmanifest)
  archive.rec                    all Disc / Copy / Event records at burn time
  volumes/<disc-id>/             each earlier disc's index: manifest, listing, formats, tags
```

- Header fields `Snapshot-Date`, `Includes-Up-To: <disc-id>` and
  `Previous-Snapshot-Sha256` (a hash chain that detects a missing or altered disc).
- Rebuild: take the newest disc's snapshot, add the own `catalog.rec` of any
  discs burned after it, and let the newest date win for notes and events.
- Size is negligible (~150 MB of text per million files).
- The snapshot is frozen at burn time. The home `archive.rec` stays
  authoritative for notes and checks added later.
- Privacy: each disc reveals the listing of everything before it. Add a
  per-disc option to omit or limit the snapshot for discs given to other people.

Events that happen *after* burning (later fixity checks, re-burns) can only
live in the master `archive.rec` at home, because the disc is write-once.

## Sources

- [NDSA Levels of Digital Preservation](https://www.ndsa.org/publications/levels-of-digital-preservation/), [v2.1 announcement](https://www.ndsa.org/2026/03/23/announcing-version-2_1-of-the-ndsa-levels-of-digital-preservation.html), [DPC: introducing v2.0](https://www.dpconline.org/blog/introducing-the-new-ndsa-levels-of-preservation)
- [PREMIS Data Dictionary 3.0](https://www.loc.gov/standards/premis/v3/)
- [E-ARK CSIP](https://earkcsip.dilcis.eu/), [E-ARK SIP](https://earksip.dilcis.eu/), [Commons-IP validator](https://keeps.github.io/commons-ip/)
- [RO-Crate 1.2](https://www.researchobject.org/ro-crate/specification/1.2/index.html), [release announcement](https://esciencelab.org.uk/ro-crate/announcements/2025/06/04/ro-crate-1.2-released/)
- [OCFL 1.1](https://ocfl.io/1.1/spec/)
- [ICA Records in Contexts](https://www.ica.org/ica-network/expert-groups/egad/records-in-contexts-ric/), [RiC-O 1.1](https://www.ica.org/standards/RiC/RiC-O_1-1.html)
- [Preservation metadata (Wikipedia)](https://en.wikipedia.org/wiki/Preservation_metadata)
