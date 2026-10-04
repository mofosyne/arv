{title}
{underline}

Disc id:  {id}{part}
Set:      {set}
Burned:   {date}
Contents: {files} files, {bytes} bytes (in data/)
Made by:  {software}

{plain}

The rest of this page is for checking the disc and, if it is ever damaged,
repairing it. Anyone comfortable with a command line can follow it.

This disc is a BagIt bag (RFC 8493) with dvdisaster RS03 error correction
data stored after the filesystem.

BROWSE
  Open index.html in any web browser. It lists every file on this disc
  and{other_discs}.

SEARCH
  Catalogue software that reads this format can search every disc (the
  spec: tools/{repo}/docs/smart-archive-format.md).
  From the root of the mounted disc, with Python 3:
    python3 tools/{repo}/arv --home catalog find PATTERN
    python3 tools/{repo}/arv --home catalog list
  or with arvc (see RESTORE): ./arvc find PATTERN, ./arvc list
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
  checking every file as it copies. {ape_use}Build it with any C compiler:
    cc -O2 -o arvc tools/{repo}/src/arvc/*.c tools/{repo}/src/udfwrite/udfwrite.c
    ./arvc verify .                    (or: ./arvc restore . ~/restored)
  Without a compiler, after copying data/ to DEST, recreate the links with:
    awk -F'\t' '$3 ~ /^link (recorded|broken)/ {{print $4 "\t" $5}}' \
      catalog/volumes/*/listing.tsv | while IFS="$(printf '\t')" read -r t p
      do ln -s "$t" "DEST/$p"; done

REPAIR (fix damage)
  Use dvdisaster: https://github.com/teaching-droid/dvdisaster-light or
  https://github.com/speed47/dvdisaster (a copy may be in tools/extra/, but
  keep one off-disc too).
  1. Read the disc into an image, even if parts are unreadable:
       dvdisaster -d /dev/sr0 -r -i disc.iso
     (dvdisaster Light can read, repair and re-read in one go: add --rescue.)
{size_check}  2. Repair, then check:
       dvdisaster -i disc.iso -f
       dvdisaster -i disc.iso -t
  3. Still damaged? Every copy of this disc is identical. Put in another copy
     and read it into the same image; only the missing sectors are read:
       dvdisaster -d /dev/sr0 -r -j 1 -i disc.iso
     then repair as in step 2.
  Then burn or mount disc.iso and verify as above.

CATALOGUE
  catalog.rec             this disc's record (GNU recutils format, plain text)
{catalog_lines}
TOOLS
  tools/{repo}/           the program that made this disc
{bundle_line}{ape_line}  tools/bagit.py          BagIt validator (public domain)
