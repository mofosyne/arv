# Organising an archive: lessons from other software

What personal archiving, cataloguing and photo software taught us about
categories and structure, and what this project adopted. The Katalog notes come
from reading its source; the rest is from the tools' documented behaviour.

## What the tools do

| Tool | Organising idea | Lesson |
|---|---|---|
| **Katalog** | Separates the physical medium (storage: location, label, filesystem) from logical grouping (devices, nested "virtual devices") and file lists (catalogues). Tags are attached to folder paths. | Keep "where it is" apart from "what it is". Folder tags are the only kind of tagging that stays cheap over thousands of discs. Its tags are free text with no vocabulary, so they drift. |
| **VVV** | Volumes plus physical/virtual folders; no tags. | Grouping by hand is laborious, and descriptions have nowhere to go. |
| **Johnny.Decimal** | At most 10 areas × 10 categories; each item has exactly one home. | Keep the top level small and the tree shallow. Numbers sort well but aren't readable decades later, so we use words plus an `Order` field. |
| **PARA** | Groups by how actionable things are; "Archive" is where things end up. | Group an archive by area of life, not by project status. |
| **digiKam / Lightroom** | Albums (one place) vs keywords (many, hierarchical, with synonyms and a "don't export" flag). Stored in XMP as `lr:hierarchicalSubject` (`Places\|Japan\|Kyoto`). | One home plus many tags. Export hierarchical keywords so photo tools can use them. Mark what must not be shared. |
| **Hydrus Network** | Namespaced tags (`person:`, `place:`), *siblings* (aliases that point to one tag), *parents* (implications). | This is the closest match to our design. Aliases and namespaces are what keep a large tag set consistent. |
| **Paperless-ngx** | Separate facets (correspondent, document type, tags, storage path) and assignment rules per tag (literal, regex, fuzzy, learned). | Keep facets in separate fields, not one overloaded tree. Deterministic rules handle the obvious cases without a model. |
| **TagSpaces** | Tags in sidecar files, no database. | Metadata that travels with the data survives the software. |
| **Calibre / Zotero** | Collections plus tags, custom columns. | The same one-home-plus-tags pattern. |
| **PhotoPrism / Immich** | Dates, places and people come from the files themselves (EXIF, reverse geocoding, faces). | Never ask a person for what the files already say. |
| **git-annex metadata** | `key=value` per file; "views" build folder trees from metadata. | A tree is one *view*. Store facets and derive trees from them. |
| **ArchivesSpace / Archivematica** | Grouping by provenance (*respect des fonds*), original order kept, containers and locations are separate records. | Never rearrange the payload to fit a taxonomy. Who or what produced the files is a strong search key. Locations are records, not text. |
| **SKOS, faceted classification** | `prefLabel`, `altLabel`, `broader`, `scopeNote`; facets beat Dewey's single enumerated tree. | Our set vocabulary is SKOS-shaped; it needed `altLabel` and `scopeNote`. |

## Patterns

1. **One home plus many tags.** Each disc has one `Set` and any number of `Category` codes.
2. **Separate facets.**
   - *What*: the set vocabulary.
   - *When*: EDTF `Coverage`.
   - *Who, where, which occasion, which device*: namespaced folder tags.
   - *Who may see it*: `Access`.
   - *Where it is kept*: `Location` records.
3. **Aliases stop drift.** Both vocabularies have `Alias`. It is used when typing, reviewing and guessing from folder names.
4. **Implication through parents.** The set vocabulary is a DAG, and discs record every path.
5. **Automate first, ask a person last.** The order is `Match` rules, then EXIF and file dates, then the embedding tagger, then a local LLM, then the owner's review.
6. **Physical location is a tree of records**, so moving a box is one edit.
7. **Original order is kept.** The payload is never rearranged; classification lives in metadata.
8. **Interoperate.** Hierarchical keywords export for XMP tools, and there is a Katalog mapping in [smart-archive-format.md](smart-archive-format.md).

## What was adopted

| | Feature | Where |
|---|---|---|
| A | `Alias` and `ScopeNote` in `sets.rec`. `--set holidays` means TRIP; a folder named "Projects" means PROJ. | `archivetool/sets.py`, `arv sets -v` |
| B | `Match` globs in both vocabularies. They suggest set and categories in `arv make`, and tags in `arv tag` (`--rules-only` needs no model). | `sets.Vocabulary.match`, `tagger.TagVocab.rule_tags` |
| C | `Access`: `public` / `private` (default) / `sealed`. It controls what other discs' snapshots carry. | `catalog.access`, `catalog.sealed_view`, `arv access` |
| D | Namespaced tags (`person:`, `place:`, `event:`, `source:`, `project:`) and tag aliases, normalised in review and in drafts. | `catalog.normalise_tag`, `arv tags` |
| E | `Location` records (a tree), one disc `Location` per place copies are kept, `burned --location`, `list --at`. | `catalog.Catalog.locations`, `arv location` |
| F | Hierarchical keywords (`MEMORIES\|PHOTO\|TRIP`, `place\|kyoto`) as TSV or as an exiftool argument file. | `arv keywords` |

## Not adopted (yet)

- Per-tag privacy (Lightroom's "don't export" per keyword): `Access` is per disc.
- Metadata-driven views as folders (git-annex views). `arv list --in/--at/--covers` and search cover most of it.
- Auto-derived place and people facets from EXIF GPS and faces. This needs libraries beyond the standard library.
