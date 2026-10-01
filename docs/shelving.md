# Organising the physical discs

The tool doesn't know where a disc is; it only knows what you record with
`archive locate`, `archive burned --location` and `archive location`. This guide
recommends a physical arrangement that matches the catalogue, so the shelf and
the records stay easy to keep in step and anyone can find a disc from its id
alone.

## The rule: shelf order is id order

Disc ids sort the way the archive is organised: set code, then sequence number
(`PHOTO-03_…`, `PHOTO-04_…`, `TRIP-01_…`). So:

1. **One section per top-level group** of the vocabulary, in its `Order`
   (`archive sets` shows it): MEMORIES, RECORDS, CAREER, PROJECTS, COLLECT,
   BACKUPS, MISC. A section is a shelf, a drawer or a run of boxes.
2. **Within a section, by set code,** in the vocabulary's order or
   alphabetically. Pick one and keep to it.
3. **Within a set, by sequence number.** Split sets (`Bag-Count: n of N`) have
   consecutive numbers, so they stay together.

A disc with several categories still has exactly **one** home: its Set, the
first part of its id. Categories, tags and collections are for finding it.
Don't make physical copies to file a disc under each category.

Leave room at the end of each set for future discs, because sequence numbers
only grow. A box per set (or per small group of sets) makes that easy.

## Location codes: down to the box, not the slot

Record locations down to the **box** (or case, or binder), not the position
inside it. The id order inside the box already tells you where a disc is, and
not tracking slots means re-shelving never makes the records wrong.

```sh
archive location add HOME "Home"
archive location add HOME-STUDY "Study" --in HOME
archive location add HOME-B01 "Box 1: MEMORIES (PHOTO, TRIP)" --in HOME-STUDY
archive location add HOME-B02 "Box 2: RECORDS" --in HOME-STUDY
archive location add HOME-SAFE "Fire safe" --in HOME
archive location add PARENTS "Parents' house"
archive location add PARENTS-B01 "Box 1 (copies)" --in PARENTS
```

- **Codes:** short, written large on the box itself, site first (`HOME-B01`,
  `PARENTS-B01`), so the code says which site it belongs to even after a box
  moves. Codes allow 1–24 capital letters, digits, `-` and `_`.
- **Names:** say what's inside. They show up in `archive list`, `search.html` and
  every disc's catalogue ("stored at Home / Study / Box 1: MEMORIES …").
- **Moving a box:** `archive location move HOME-B01 --in PARENTS` moves every
  disc in it.

## Copies: same image, different places

Burn every copy from the **same `.iso`**, so the copies are sector-identical and
can repair each other (see [workflow.md](workflow.md#6-recover)). Keep at least
two copies, in two places: the usual 3-2-1 idea, with an off-site copy.

```sh
archive burned TRIP-01_2019_4 --copies 1 --location HOME-B01
archive burned TRIP-01_2019_4 --copies 1 --location PARENTS-B01
archive list --at PARENTS          # what the off-site box holds
```

Discs you made `sealed` (taxes, identity documents) belong in the safe:
`--location HOME-SAFE`.

The off-site box doesn't need to follow the home box's order exactly. Because
everything is located by box, it only needs the same ids.

## Labels

| Where | What | How |
|---|---|---|
| On the disc | the **full id**, e.g. `TRIP-01_2019_4` | a solvent-free (water-based) marker meant for discs, on the clear inner hub ring only. No paper labels. |
| Case spine | the volume label: the id, then the title, e.g. `TRIP-01_2019_4 Kyoto July 2019` | printed or written spine insert |
| Box | its location code, large, and its contents list | print `archive list --at HOME-B01` and keep it in the box lid |

The id's last character is a check character, so a smudged or misread id is
caught: `archive id` says whether an id is valid, and suggests the likely disc.

## Care

Handling and storage, following the usual advice for optical media (for
example NIST SP 500-252, *Care and Handling of CDs and DVDs: A Guide for
Librarians and Archivists*):

- Store discs **upright in their cases**, like books, not stacked flat or loose.
- Keep them **cool, dry and dark**, with stable temperature and humidity. Avoid
  attics, garages, outside walls, sunny shelves and cars.
- Handle them **by the edges**, and don't flex them.
- Write only on the hub, with a solvent-free marker. Adhesive labels can peel or
  unbalance a disc.

M-DISC media resist ageing better than ordinary BD-R, but cases, storage
conditions and copies in more than one place still matter.

## Checking over time

- Every few years, **read one copy of every disc fully**:
  `archive check --device /dev/sr0` logs a fixity-check event.
- Alternate which copy (home or off-site) you check.
- A disc that needed repair is a warning sign: burn a fresh copy from a good
  image, record it with `archive burned`, and retire the weak one.
- `archive list` shows copies and locations, so discs with only one copy, or
  with all copies at one site, are easy to spot.

## What this buys you

With the shelf in id order and locations recorded per box, every index points
to a physical place:
- `archive find`;
- `search.html` on any disc;
- a virtual tree in catalogue software, "by place", for example
  `Home/Study/Box 1/TRIP-01_2019_4/…` (see
  [the format spec](smart-archive-format.md#building-a-virtual-file-system-from-the-catalogue)).

Inside the box, the disc's place follows from its id.
