# Philosophy

What this project is for, and what it deliberately is not. The how is in
[workflow.md](workflow.md); the decisions and their reasons are in [plan.md](plan.md).

## Not everything goes on Blu-ray

A Blu-ray holds 25 GB. A household's data does not fit, and most of it does not
need to last fifty years. So there are two tiers:

| Tier | Holds | Lifetime | Robustness |
|---|---|---|---|
| **Everyday storage**: NAS, external drives, the hard drives lying around | everything | years; replaced as drives fail | good enough, not trusted for decades |
| **Archive discs** (M-DISC BD-R) | what matters most | decades | self-describing, error-corrected, copies in different places |

The discs are not a backup of the NAS. They are a **curated selection**: the
things worth the extra effort of a durable copy. Choosing is part of the work.

## Plain files, not a backup engine

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

## Importance decides what is archived, and for whom

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

## Durability comes from copies and simplicity

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

## Two orders: the shelf and the catalogue

Physically, discs are kept by access level and the year they were made, so new
discs only ever go at the end ([shelving.md](shelving.md)). Virtually, the
catalogue on every disc shows them by kind, by year covered, by place, by
collection and by tag. The shelf only has to be stable; the catalogue gives
every other view, and catalogue software such as Katalog can present it.

## Small tools, run in order

The work is a chain of small programs (plan.md, "a chain of small programs"):
scan, describe, bag, image, protect, verify; and on the reading side, read,
combine copies, repair, check, search. Each one does one step on plain files
and has a written format behind it, so any of them can be replaced, and each
can travel on the disc.

## Help is optional and local

Language models can suggest descriptions and tags, but only running locally, and
only as suggestions. Nothing about reading, checking or repairing a disc depends
on them.
