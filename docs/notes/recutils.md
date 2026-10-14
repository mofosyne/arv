# Application note: querying the catalogue with GNU recutils

arv's catalogue is a set of recfiles: plain text, one record per thing, typed by the record
descriptors at the head of each file. GNU recutils (`recsel`, `recinf`, `recfix`, `recfmt`,
`rec2csv`) can query them without arv. Use it for a question arv has no command for, or when
arv is not at hand. Every example below was run against a real catalogue.

## The files

| File | What |
|---|---|
| `.arv/catalog/archive.rec` | the home catalogue: every disc, place, selection, collection and event, as of now |
| `catalog.rec` on a disc | that disc's own record: `Archive`, `Disc`, `Binding`, the places it names, its events |
| `catalog/archive.rec` on a disc | the whole archive as of the day the disc was made |
| `catalog/config/*.rec` | the set codes and tag words |

What each record type and field means is described in the spec
([smart-archive-format.md](../spec/smart-archive-format.md)), and briefly in the `%doc` of each
descriptor.

**Read with recutils; change with arv.** Events are never edited, and each one's `EventId` is a
hash of its fields. An event changed by hand no longer matches its id, and `arv audit` reports
it. `recfix --check` only reads, so it is safe.

## Examples

```sh
cd .arv/catalog    # or the root of a disc, using catalog/archive.rec
recinf archive.rec                        # how many of each record
recfix --check archive.rec && echo ok     # well formed, mandatory fields there, types right
```

Places belong to physical copies, not `Disc` records. A copy's `replication` event has its
initial `Location`; later `metadata modification` events name that copy by its `Replication` id
and `Copy`. Use arv for the current disc-level view and filters:

```sh
arv objects PHOTO-01_2026_3       # current place of each copy
arv list --at BOX                  # discs with a copy at BOX or inside it
```

Every copy (a copy is a `replication` event), with the title of its disc:

```sh
recsel -t Event -e 'Type = "replication"' -j Disc -p Disc_Id,Copy,Disc_Title,Location,Identifier archive.rec
```
```
Disc_Id: DOC-01_2026_G
Copy: A
Disc_Title: Letters
Location: BOX
Identifier: barcode:ARV00001
```

Which disc and copy a barcode belongs to:

```sh
recsel -t Event -e 'Identifier = "barcode:ARV00001"' -p Disc,Copy archive.rec
```

One disc's history, oldest first:

```sh
recsel -t Event -e 'Disc = "DOC-01_2026_G"' -p Date,Type,Note archive.rec
```

Search every field, ignoring case (`-q`), or one field by pattern (`~`):

```sh
recsel -t Disc -i -q 1950s -C -P Id archive.rec
recsel -t Disc -i -e 'Note ~ "letters"' -C -P Id archive.rec
```

Count, and compare dates (`>>` means after):

```sh
recsel -t Disc -c archive.rec
recsel -t Event -e 'Type = "replication" && Date >> "2026-10-01"' -c archive.rec
recsel -t Disc -e '#Retired = 0' -C -P Id archive.rec   # discs not retired
```

The place tree (`Parent` is joined like `Location`):

```sh
recsel -t Location -j Parent -p Code,Name,Parent_Name archive.rec
```

Into a spreadsheet, or a line of your own:

```sh
recsel -t Event -e 'Type = "replication"' -j Disc -p Disc_Id,Copy,Disc_Title,Location archive.rec | rec2csv > copies.csv
recsel -t Disc archive.rec | recfmt '{{Id}}  {{Title}}
'
```

## Things to know

- **A join keeps only records that match.** A copy whose `Location` is free text rather than a
  place's code, and a place with no `Parent`, drop out of a joined query. Leave out `-j` to see
  every record. A disc with two copies appears once per replication event; arv combines copies
  when showing unique places for the disc.
- **recsel shows the record; arv draws the conclusions.** Two replication events can be one
  physical disc (they share a `bca:`, `barcode:` or `serial:` identifier). Two copies on one tape
  count as one. A copy's letter is the newest one given. A field changed twice apart is a
  conflict. `arv objects`, `arv list` and `arv todo` take all of that into account. For their
  conclusions in a script, read `arv objects --json` or `arv todo --tsv`, not recsel.
- **Listings are TSV, not recfiles.** For files, use `grep`, `awk` or `cut` on
  `catalog/volumes/*/listing.tsv` (size, modified, kind, link target, path), or `arv find`.

recutils is packaged by most distributions (Debian's is `recutils`) and by Homebrew. Emacs's
`rec-mode` browses and edits recfiles, but here use it to read only.
