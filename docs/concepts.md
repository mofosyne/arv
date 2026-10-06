# Concepts: what arv calls things

The model behind the commands, on one page. The steps are in [workflow.md](workflow.md); why it
works this way is in [philosophy.md](philosophy.md); what a disc holds, field by field, is in
[spec/smart-archive-format.md](spec/smart-archive-format.md).

## Two axes, and a registry that only points

arv keeps two things apart, and a third that only points at them:

| | What it is | Where it lives | Made by |
|---|---|---|---|
| **collection** | something you keep over time: one workflow folder, one code, one history | a `.arv` marker in the folder (identity only) | `arv collection init` |
| **archive** (home) | the record of one privacy sphere: every disc, copy, place, collection, revision and event | a `.arv` folder (the catalogue) | `arv init` |
| **copies** | the physical or stored copies of a disc: burned, an iso, or a folder | recorded in the archive | `arv burned`, `arv stored` |
| **machine registry** | which archives exist on this computer, and the default | `~/.config/arv/homes.rec` | `arv init --name`, `arv where` |

- A **collection** is *what you keep*. It is a folder you sort, with a small `.arv` marker carrying
  its identity (a UUID) and nothing else. The marker never goes on a disc.
- An **archive** is *the record*. It is a `.arv` folder holding the catalogue: `catalog/` (laid out
  exactly like `catalog/` on a disc), `config/` (your vocabularies), `drafts/` and `cache/`. Each
  archive is its own **privacy sphere**: its discs carry its catalogue only.
- **Copies** are the discs and images, each with a **place** and a **temperature** (hot, warm,
  cold).
- The **machine registry** is not an archive: it is a list of names and paths, and which is the
  default. It never goes on a disc, and deleting it loses nothing.

`.arv` is one name for three things, told apart by what it is:

| `.arv` | What it is |
|---|---|
| a **folder** | a home: the archive's catalogue |
| a **pointer file** (`Home: PATH`) | "this tree belongs to the archive whose catalogue is there" |
| a **pointer file with `Collection: UUID`** | the same, and "this folder is that collection's workflow folder" |

The archive is found the way git finds `.git`: `arv` walks up from the folder it works on, then
falls back to the default registered on this machine. `arv where` says which archive is used, and
why.

## Objects and folders: implicit and explicit

A disc is plain files: the folder tree is preserved as it is, under `data/`. Containment is
therefore **implicit in the paths** — a repository at `data/projects/foo/` is inside `projects/`
because the path says so.

The format does not turn folders into objects. There is no `Object` record; a folder is a path
prefix. Objecthood is asserted in only two places:

- the **collection** a disc is an edition of (`Disc.Collection`);
- **git repositories** (`git.tsv`: roots, heads, shallow boundary, every commit).

Everything else is observation, not assertion. The planned **Notable objects**
([plan.md](../research/plan.md)) adds records for the folders and files an archivist must treat
specially — a git repository, a mailbox, a nested bag, a database, a disc image — with a path and a
kind, in the catalogue. That is where object nesting becomes **explicit**, and only where it adds
something the paths cannot:

- a **submodule**, whose files are at a path inside the repo but whose history is elsewhere;
- a **selection**, which groups files from different discs and has no path containment at all;
- a **photo library**, which may reference files outside its own folder.

> **The folder tree is the implicit physical truth. Object relations are explicit in the
> catalogue, only where they add something the paths do not.**

So a found disc with no catalogue is still navigable: the folders are there, plain, and
`listing.tsv` and `README.txt` explain them.

## There is no temporary workspace

There is no separate folder to assemble a disc in, and the design avoids one. The **workflow folder
is the collection**: it is where you sort, and it stays. Deleting it does not lose the archive's
record of the collection (that is in the catalogue, and `arv restore` writes the marker back), but
the folder is the thing you keep — not a scratch area to throw away.

What is transient is the **output**, not the folder:

| Transient | What it is | Safe to delete? |
|---|---|---|
| the `.iso` images | `arv make`'s output (`--output-dir`, default: the current folder) | yes, once every copy is burned or stored and recorded |
| `drafts/` | work in progress: the JSON `arv describe` and `arv tag` save, taken by `arv make --draft` | yes, once applied |
| `cache/` | rebuildable indexes (marked `CACHEDIR.TAG`) | yes, always |

## The archive is a collective of copies

No single place holds everything. The PC or NAS holds the working objects (hot) and maybe some
disc images (warm); the shelf holds the burned discs (cold). Each is partial:

- the **workflow folder** may hold only the objects being worked on;
- a **disc** holds the part of the collection put on it, and a copy of the catalogue;
- an **iso** is a warm mirror of a disc.

The full data object collection is the **union** of all of them: the PC/NAS and the cold discs
together. The catalogue is the map — it records where every copy is, so the union can be found,
and `arv status` on a folder says which of its files are on which discs and which are on none.

Completeness is a property of the union, not of any one place. `arv todo` watches the union's
health: discs with no copy, copies never read back, discs with no cold copy, discs in fewer than
two places, checks overdue.

### What an edition covers

An edition is made from what is in the workflow folder when `arv make` runs: its discs together
hold that, and no more. The folder does not have to hold everything the collection ever had; what
left it is still on earlier discs, and the catalogue still says where.

Today a newer safe edition **replaces** the earlier ones (unless they are kept), so a file that left
the folder before the newer edition was made is then on the older discs only. `arv retire` lists
those files and records nothing until `--yes`; keep an edition (`arv collection keep CODE N`)
whose discs hold something you still want. arv chooses which files go on which disc of an edition
(`--split` fills discs in order); choosing that by hand, across folders, is not built yet.

## The catalogue is spread across every disc

There is no central catalogue the discs depend on. Every disc carries a **snapshot** of the whole
archive's catalogue as of its making — `catalog/archive.rec` and `catalog/volumes/<other-id>/` —
limited by `--snapshot` (`full`, `set`, `disc`) and by each disc's access level. So:

- **the newest disc is always a copy of the catalogue**;
- a lost archive is recovered from any disc (`arv rebuild`);
- a disc of another archive is refused, because archives are separate privacy spheres
  (`--any-archive` to merge).

## The words

| Word | Meaning |
|---|---|
| **collection** | something kept over time: one workflow folder, one code, one history. *What you keep.* |
| **set** | a vocabulary classification (`PHOTO`, `TRIP`), not a thing you keep; the id prefix |
| **revision** | one recorded state of a collection: a **checkpoint** (hashes only) or an **edition** |
| **edition** | a set of discs made together from what a collection's workflow folder holds at the time; each disc holds part of it and a copy of the catalogue; numbered; replaced by a newer safe edition unless kept |
| **volume (disc)** | one bag, one image; belongs to exactly one edition |
| **copy** | one physical or stored copy of a disc: burned, an iso, or a folder; with a place and a temperature |
| **archive** (home) | one privacy sphere's catalogue: a `.arv` folder |

A **collection** and a **set** are different things: a collection is what you keep, a set is how it
is classified. A collection has one set; a set has many collections.
