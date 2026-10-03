# Philosophy

What this project is for, and what it deliberately is not. The how is in
[workflow.md](workflow.md); the decisions and their reasons are in [plan.md](../research/plan.md).

## In one sentence

**Keep what matters readable, by the people it matters to, for decades, without
depending on us, this tool, or any company.**

Everything else follows from six principles:

1. **Curate, don't hoard.** Durable media is small and costly in effort, so it
   holds a chosen selection, not everything. What goes on a disc, and who may see
   it, is decided on purpose. Everyday storage keeps the rest.
2. **Every disc stands alone.** A disc is ordinary files plus everything needed to
   understand, check and repair it, and a catalogue of every disc before it.
   Any one disc, read with any computer, is enough to start from.
3. **Durability comes from copies and simplicity**, not from clever layers:
   identical copies in different places, error correction on every disc, and as
   few moving parts as possible.
4. **The format is the product; the tool is replaceable.** Open standards, plain
   text and written specifications outlive programs. Every disc carries the
   source of the tools that made it, and nothing on a disc requires them. **Open on
   both ends:** anyone can make a volume as well as read one, with ordinary
   equipment and open tools, unlike archives whose reader is open but whose writer
   belongs to one vendor (research-notes.md, Piql).
   **The archive is independent of its medium:** files and the catalogue never
   depend on a filesystem or medium; a volume's *binding* (UDF on Blu-ray today)
   is recorded separately, so new media need a new binding, not a new format.
5. **People decide; tools propose.** Automation (sorting, tagging, choosing what
   to archive) only makes suggestions, runs locally, and is never needed to read
   a disc.
6. **Describe, don't own.** Version control and distributed file systems own your
   files: they move them into their own stores, replace them with links, and must
   be running to give them back. An archive does not. It records, checks and
   copies files, but leaves them as ordinary files under the owner's control,
   wherever they already are.

The sections below say what each principle means in practice.

## Not everything goes on Blu-ray (principle 1)

A Blu-ray holds 25 GB. A household's data does not fit, and most of it does not
need to last fifty years. So there are two tiers:

| Tier | Holds | Lifetime | Robustness |
|---|---|---|---|
| **Everyday storage**: NAS, external drives, the hard drives lying around | everything | years; replaced as drives fail | good enough, not trusted for decades |
| **Archive discs** (M-DISC BD-R) | what matters most | decades | self-describing, error-corrected, copies in different places |

The discs are not a backup of the NAS. They are a **curated selection**: the
things worth the extra effort of a durable copy. Choosing is part of the work.
How the everyday tier can be set up: [workflow.md](workflow.md), "Before the discs".

## Plain files, not a backup engine (principle 2)

Backup programs like restic/rustic (used by LCSAS) store as much as possible in
as little space as possible: files cut into chunks, deduplicated, compressed,
encrypted, spread over many volumes. That suits bulk backup. It is the wrong
trade for a small, curated archive meant to outlive its tools, because every
file then depends on the program, the key and every volume holding its chunks.

Here, a file on a disc is the file:
- **any one disc can be read on its own**, with any operating system, without
  this tool;
- **the disc describes itself**: what is on it, which other discs exist and
  where they are kept, and how to check and repair it (README.txt);
- **space is not optimised.** No compression, no deduplication. Curating keeps
  the volume small enough that this does not matter.

## Importance decides what is archived, and for whom (principles 1 and 5)

Data matters differently to different people: some to you, some to family,
some to whoever inherits the discs, some to anyone. The tool already records
some of this:
- **access levels** (`public`, `private`, `sealed`) decide who may see a disc and
  what other discs' catalogues say about it;
- **sets, categories and tags** say what something is;
- **collections** gather what belongs together across discs.

**Direction: an archive organiser.** From the everyday storage, propose what is
worth a disc: rank folders by importance and audience (rules, tags, and
optionally a local model), leave out what is already archived (by hash), and
fill discs with the most important material first. People decide; the tool
proposes.

## Durability in practice (principle 3)

- **Identical copies in different places**, burned from the same image. Two
  damaged copies can rebuild each other (tested; research-notes.md section 8).
- **RS03 error correction on every disc**, covering the filesystem too.
- **Copy health is watched, duplicates are only reported.** The tool can say which discs have
  too few copies or only one site, and which files are already on another disc (by SHA-256).
  It never removes a duplicate itself: each disc must still stand alone.
- **No extra layers that need their own repair.** No PAR2; if encryption comes,
  it sits on top of RS03, and the simplest option wins.
- **Long bets only:** plain text (recfiles, TSV), BagIt, UDF, and tools that run
  on Python 3 or as WebAssembly. Every disc carries the source of the tools that
  made it.

## Two orders: the shelf and the catalogue (principle 2)

Physically, discs are kept by access level and the year they were made, so new
discs only ever go at the end ([shelving.md](shelving.md)). Virtually, the
catalogue on every disc shows them by kind, by year covered, by place, by
collection and by tag. The shelf only has to be stable; the catalogue gives
every other view, and catalogue software such as Katalog can present it.

## Small tools, run in order (principle 4)

The work is a chain of small programs (plan.md, "a chain of small programs"):
scan, describe, bag, image, protect, verify; and on the reading side, read,
combine copies, repair, check, search. Each one does one step on plain files
and has a written format behind it, so any of them can be replaced, and each
can travel on the disc.

## Plain text first, with a way into institutional systems (principle 4)

The catalogue is plain, line-oriented text in the Unix tradition: recfiles for
records, TSV for file lists, BagIt's text manifests. `cat`, `grep`, `diff` and
`sort` work on all of it, damage loses lines rather than whole documents, and
history is kept by appending records, not by editing them. The heavy
institutional formats (METS XML, PREMIS XML, E-ARK, OCFL) solve problems a
personal archive does not have, so they are not the source of truth.

**Personal usefulness comes first; a hand-off to an archive comes second, and is never
a rescue.** The owner never has to do an archivist's work to use their own discs. But what an
archivist would need is recorded whenever it is known cheaply and automatically, so that one
day the archive can be given to a library, a national archive or a software archive without
anyone reconstructing what is in it.

Part of that is **telling an archivist what kind of objects are inside**, not just which file
formats. Siegfried already names each file's format (a WARC web archive, a PDF/A, a disk
image). But some objects are whole folders that no per-file tool sees: a git repository, a
mail folder, a photo library, a website mirror, a bag inside the bag, a build tree. Each
needs its own handling and its own expert, and an archivist should learn from the catalogue
that a disc holds one (planned: research/plan.md, "Notable objects").

Code deserves a word of its own. The people likely to care about arv keep their own code,
so a personal archive is also a small **software heritage** archive: for code, faithful means
the tree's shape (executable bits, links: the listing records them as git does, see
"Links" in the format) and its history, and useful later means it can be rebuilt. What
Software Heritage already holds (public code) can be referenced by its identifier; what only
the owner has (private repositories, and the notes, photos and reasons around them) is what
arv is for.

So moving the archive into such a system one day (a library, a national
archive, a company records system) should be routine, not a rescue. So:
- **the package is already one they take:** every disc is a BagIt bag, which
  Archivematica and similar systems ingest as a transfer;
- **field meanings come from their standards:** Dublin Core names for
  description, PREMIS event types and preservation levels, PRONOM ids for file
  formats, ISO 8601/EDTF dates. The crosswalk is in
  [metadata-standards.md](../research/metadata-standards.md), section 6;
- **identifiers are stable and paths are plain:** disc ids plus UUIDs, file paths
  relative to `data/`, SHA-256 for every file;
- **exports, not rewrites:** an RO-Crate description already exists
  (`--ro-crate`); a METS/PREMIS export can be added the same way when someone
  needs it, generated from the recfiles rather than replacing them;
- **a path to Piql:** an organisation that wants film storage later should not
  have to start over. Piql's ingest (PiqlConnect) is built on Archivematica,
  which takes BagIt bags as they are; and our per-file records are kept mappable
  to the AFS table of contents, so an AFS export (or an `afs` binding) stays a
  small step.

Anything we invent that has no standard counterpart (importance per audience,
cascading appraisals, the catalogue snapshot on every disc) is documented in
[smart-archive-format.md](smart-archive-format.md), so an exporter knows what
to carry over and where.

## Describe, don't own (principle 6)

| | Owns the files | Describes the files |
|---|---|---|
| Examples | git, git-annex, restic/Borg repositories, sync and distributed file systems | this archive: the discs, and `.arv/` beside the everyday tree |
| Where the content is | in the tool's store (objects, packs, chunks), the tree holding links or managed copies | where it already was, as ordinary files |
| To get a file back | the tool, often its exact version and settings | copy it |
| If the tool breaks or is removed | the files can be stranded | nothing happens to the files |

In practice:
- **The tool never moves, renames, links or rewrites the owner's files.** It reads
  them, hashes them and copies them onto discs.
- **Its own data sits beside the files, not in their place:** the catalogue in a
  `.arv/` folder at the root of the tree, and on every disc. Deleting that
  folder leaves every file exactly as it was.
- **Everything it writes must survive being copied anywhere:** between Windows,
  macOS and Linux, onto exFAT or NTFS drives and NAS shares. So no symlinks or hard
  links (the owner's links are noted in the listing, as git notes them in its tree, and the
  files they point to copied: see "Links" in the format), plain UTF-8 text read with any line endings, portable file names, nothing
  that depends on permissions, and indexes that can always be rebuilt. (Lesson
  from git-annex, whose symlink trees break when moved between operating systems.)
- **Storage managers stay welcome on the everyday tier** (ZFS, restic, even
  git-annex in unlocked mode): they manage the storage; the archive only
  describes and curates.

## Help is optional and local (principle 5)

Language models can suggest descriptions and tags, but only running locally, and
only as suggestions. Nothing about reading, checking or repairing a disc depends
on them.
