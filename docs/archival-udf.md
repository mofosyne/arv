# Archival UDF: the disc profile arv writes

**Draft.** The subset of UDF 2.50 that arv's own disc writer produces (issue #18), and how
RS03 error correction sits after it. The writer is [`src/udfwrite/`](../src/udfwrite/),
the only one arv makes discs with. NetBSD makefs (now `upstream/netbsd-makefs/`) wrote arv's first UDF discs; where
it differs from this profile, the difference is noted, since those discs stay readable.

The goal is a disc that is **written once, whole and closed**, readable by any UDF 2.50 reader
(Windows, macOS, Linux, 7-Zip), and laid out so that damage is survivable and the image is
**reproducible**: the same folder and record always give the same bytes.

Standards: ECMA-167 (3rd edition) and OSTA UDF 2.50. This profile only narrows them; anything
not mentioned follows UDF 2.50 as a read-only volume.

## Why a profile and a writer of our own

General UDF writers are built for media that keep changing (rewritable discs, hard disks), so
they carry what that needs and skip what only a finished archive can afford. An archive disc is
written once and never changed, which makes some robustness features cheap that are costly for
them. Example: UDF 2.50 lets the metadata mirror point at the same sectors as the metadata it
mirrors (with the *duplicate* flag clear), and NetBSD makefs does exactly that
(`/* XXX no support for metadata mirroring yet */`), because on rewritable media a real mirror
must be kept in sync on every change. For a finished image it is just a second copy of finished
bytes, so this profile makes a real mirror **mandatory**. Ours writes it, and the Linux kernel mounts through it when the main metadata is destroyed
(tested 2026-10-03).

So the profile narrows UDF to what an archive needs, and the writer spends what it saves on
robustness and checkability: a real mirror, contiguous files in a fixed order, and reproducible
images. The same idea is offered back upstream: a real mirror for makefs's read-only images
(issue #7).

## Layout

Sector numbers are 2048-byte sectors from the start of the image. `N` is the last sector of the
UDF volume, `M` the last sector of the medium.

| Sectors | What | Notes |
|---|---|---|
| 0–15 | system area | zeros |
| 16–18 | volume recognition sequence | `BEA01`, `NSR03`, `TEA01` |
| 256 | anchor | points at the main and reserve volume descriptor sequences |
| 288… | main volume descriptor sequence | primary VD, partition, logical volume (with the metadata partition map), unallocated space (empty), implementation use, terminator |
| 320… | logical volume integrity sequence | `closed` |
| partition start | **metadata file** | file set descriptor, every directory and file entry |
| … | **file data** | every file in one contiguous run, in a fixed order |
| … | **metadata mirror file** | a real copy of the metadata file, at the far end of the partition |
| before `N−256` | reserve volume descriptor sequence | copy of the main sequence |
| `N−256`, `N` | anchors | |
| `N+1` … `M` | **RS03**: padding, header, CRC layer, error correction | written by dvdisaster's code, sized to fill the medium (at least 20% redundancy) |

## The rules

**A closed, read-only volume.** Partition access type *read-only*; integrity *closed*; no free
space (the unallocated space descriptor has no entries; no space bitmaps or tables); one session;
no sparing, VAT or anything for rewritable or appendable media.

**Two partition maps, as UDF 2.50 requires for the BD layout:** a type 1 map for the physical
partition and a type 2 *Metadata Partition* map. Allocation and alignment unit: 32 sectors.
**The metadata mirror is mandatory.** UDF 2.50 allows a mirror that points at the same sectors
as the metadata (duplicate flag clear); this profile does not. The mirror must be **a real copy
in separate sectors**, byte for byte the same as the metadata, with the *duplicate metadata* flag
set, placed at the far end of the partition so that one scratch cannot take both. A volume
without one does not conform to this profile, whatever else it gets right.
*udfmake today does not conform: the flag is clear and the mirror file points at the same blocks
(issue #7).*

**Only regular files and folders.** No symbolic or hard links, devices, sockets, extended
attributes, named streams or ACLs. (arv copies or notes the source's links before the writer
sees them: docs/smart-archive-format.md, "Links".) Permissions: everything readable by all and writable by none;
folders searchable by all; a file whose source had any execute bit is executable by all (as
`genisoimage -r` and git do: only "executable or not" survives), so a script with `chmod +x`
runs straight from the mounted disc. "All" matters: on a mounted disc the owner is unknown (−1),
so readers are "other". Owner and group are unknown (−1).

**Each file in one contiguous run of sectors**, starting on a sector boundary, split only where a
UDF extent reaches its maximum length (just under 1 GiB). No file data embedded in file entries,
so every file's bytes are in the data area and its location is a single (start, length) pair.

**A fixed order.** Folders and files are placed by their full path, compared as UTF-8 bytes,
depth first. Directory entries are written in the same order.

**Entries.** Extended File Entries (tag 266), one block each, in the metadata partition. A
folder's identifiers are in their own metadata blocks (a `short_ad`; udfmake embeds small folders
in the entry instead), a file's data is described with `long_ad`s into the physical partition.
A File Identifier Descriptor's CRC covers its padding too: 7-Zip rejects the image otherwise.
Unique ids: 0 for the root, 16 and up in path order.

**Names.** OSTA compressed Unicode: 8-bit when every character is at most U+00FF, otherwise
16-bit; at most 255 bytes. Names are stored exactly as given; a name that doesn't fit is refused
before anything is written (`arv names` checks this).

**Reproducible.** Nothing depends on the machine or the moment of writing:
- file and folder timestamps are the files' modification times (UTC, as in `listing.tsv`);
- volume and descriptor timestamps are the disc's creation time from its record;
- the volume set identifier comes from the disc's UUID; the volume identifier is the disc id and
  the logical volume identifier its label;
- the implementation identifier names arv and its version.

**What the writer records off the disc.** Because it plans the whole layout before writing,
the writer knows each file's start sector and records them as Binding data
(`catalog/volumes/<id>/extents.tsv`) in the home catalogue and on later discs, not on this disc.
This disc's own map is its metadata and mirror, protected by RS03.

## Known issue: the end anchors and RS03

UDF readers look for anchors at sector 256 and at the end of the volume (`N−256`, `N`). On a
plain image all three are where readers expect them (checked with `udfinfo` on a udfmake image).
RS03 then appends its data after the filesystem, so on the finished disc the last sectors hold
error correction, and the two end anchors sit mid-disc where no reader looks. `udfinfo` on a
finished image reports: *"Second and third Anchor Volume Descriptor Pointer not found"*.

The disc still reads, because readers find the anchor at 256, and sector 256 is inside the area
RS03 protects. (Before RS03 is added, with sector 256 destroyed, the Linux kernel and udftools
find the volume through the end anchors; 7-Zip does not. And with the main metadata destroyed,
the Linux kernel mounts through the mirror: tested 2026-10-03, see `src/udfwrite/README.md`.) But the anchor itself loses its redundancy. Moving the end anchors to the end of
the medium would put them inside RS03's area, which would break byte compatibility with
dvdisaster, so that is not an option. To do: test readers (Linux, Windows, macOS) with sector 256
damaged and RS03 not yet applied, to know what the anchor at 256 alone is worth.

## Checking a disc against this profile

A disc conforms only if **all** of these hold; the mirror check is not optional.

`make -C src/udfwrite check` runs the automatic part of this list and leaves test images (clean,
metadata destroyed, anchor 256 destroyed) for the readers that need a person.


- `udfinfo` reports revision 2.50, integrity closed, access type read-only.
- **Mandatory mirror:** `udfdump -b 2048 -S IMAGE` shows "Metadata is duplicated on disc", the
  mirror file's extent differs from the metadata file's, and the two extents hold identical bytes
  (`make -C src/udfwrite check` tests all three).
- The Linux kernel mounts it read-only, and still mounts it with the main metadata deliberately
  overwritten (falls back to the mirror).
- 7-Zip lists and extracts it; every file matches `manifest-sha256.txt`.
- Two runs over the same folder and record give byte-identical images.
- Windows and macOS open it, from an image and from a burned disc.
