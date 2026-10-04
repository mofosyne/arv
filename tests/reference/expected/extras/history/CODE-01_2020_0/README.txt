Hist-src
========

Disc id:  CODE-01_2020_0
Set:      CODE
Burned:   2026-01-01
Contents: 1 files, 2 bytes (in data/)
Made by:  SW

This is an archive disc by archivist, made on 2026-01-01: Hist-src. Its
files are ordinary files in the data/ folder, and any computer can open
them.

The rest of this page is for checking the disc and, if it is ever damaged,
repairing it. Anyone comfortable with a command line can follow it.

This disc is a BagIt bag (RFC 8493) with dvdisaster RS03 error correction
data stored after the filesystem.

BROWSE
  Open index.html in any web browser. It lists every file on this disc
  and lists the discs made before it (full catalogue).

SEARCH
  Catalogue software that reads this format can search every disc (the
  spec: tools/arv/docs/smart-archive-format.md).
  From the root of the mounted disc, with the reader (see RESTORE):
    ./arvc find PATTERN                (every disc in this disc's catalogue)
    ./arvc list
  Or plain text tools: grep -ri PATTERN catalog/volumes/*/listing.tsv

VERIFY (detect damage)
  From the root of the mounted disc, any of (arvc: see RESTORE):
    sha256sum -c manifest-sha256.txt
    python3 tools/bagit.py --validate .
    ./arvc verify .

RESTORE (copy the files back)
  Copying data/ anywhere is enough for most files. Symbolic links in the
  original folder, execute bits and dates are in the listing,
  catalog/volumes/*/listing.tsv; the reader in tools/ brings them back too,
  checking every file as it copies. It is also ready to run, as
  tools/arv.com: one file for Linux, macOS, Windows and the BSDs, on x86-64
  and ARM64 (Cosmopolitan). Copy it off the disc (on Windows as arv.exe):
    ~/arv.com verify .                 (or: ~/arv.com restore . ~/restored)
  If a Linux shell will not start it: sh ~/arv.com verify .
  Build it with any C compiler:
    cc -O2 -pthread -o arvc tools/arv/src/arvc/*.c \
      tools/arv/src/udfwrite/udfwrite.c tools/arv/src/rs03/rs03.c
    ./arvc verify .                    (or: ./arvc restore . ~/restored)
  Without a compiler, after copying data/ to DEST, recreate the links with:
    awk -F'\t' '$3 ~ /^link (recorded|broken)/ {print $4 "\t" $5}' \
      catalog/volumes/*/listing.tsv | while IFS="$(printf '\t')" read -r t p
      do ln -s "$t" "DEST/$p"; done

REPAIR (fix damage)
  1. Read the disc into an image, even if parts are unreadable, with either
     of (unread sectors are left as zeros, which the repair finds):
       ddrescue -b 2048 /dev/sr0 disc.iso disc.map       (GNU ddrescue)
       dvdisaster -d /dev/sr0 -r -i disc.iso             (dvdisaster Light,
         https://github.com/teaching-droid/dvdisaster-light: add --rescue)
     The image is larger than the filesystem. If dvdisaster does not mention
     RS03 error correction while reading, read again with --ignore-iso-size.
  2. Repair, then check, with the reader in tools/ (arvc or arv.com: see
     RESTORE); it needs nothing else:
       ./arvc check --image disc.iso --repair
     or with dvdisaster: dvdisaster -i disc.iso -f
     If arv cannot repair it, it prints the dvdisaster Light commands to
     paste: dvdisaster looks harder for the error correction's layout.
  3. Still damaged? Every copy of this disc is identical. Put in another copy
     and read it into the same image; only the missing sectors are read:
       ddrescue -b 2048 /dev/sr0 disc.iso disc.map       (the same map file)
       dvdisaster -d /dev/sr0 -r -j 1 -i disc.iso
     then repair as in step 2.
  Then burn or mount disc.iso and verify as above.

CATALOGUE
  catalog.rec             this disc's record (GNU recutils format, plain text)
  catalog/archive.rec     all discs in the archive as of the burn date
  catalog/volumes/<id>/   per disc: manifest.sha256, listing.tsv, formats.csv

TOOLS
  tools/arv/           the program that made this disc; its docs/ folder
                          describes the disc format (smart-archive-format.md)
                          and the error correction (rs03-format.md), so other
                          programs can be written to read and repair it
  tools/arv.bundle     the same with full history: git clone <bundle>
  tools/arv.com           the reader, ready to run (Linux, macOS, Windows, BSD)
  tools/bagit.py          BagIt validator (public domain)
