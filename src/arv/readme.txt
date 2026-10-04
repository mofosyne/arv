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
  spec: tools/{repo}/docs/spec/smart-archive-format.md).
  From the root of the mounted disc, with the reader (see RESTORE):
    ./arvc find PATTERN                (every disc in this disc's catalogue)
    ./arvc list
  Or plain text tools: grep -ri PATTERN catalog/volumes/*/listing.tsv

VERIFY (detect damage)
  From the root of the mounted disc, either of:
    sha256sum -c manifest-sha256.txt   (any Unix-like system; nothing else)
    ./arvc verify .                    (arvc: see RESTORE; checks it as a BagIt
                                        bag: every manifest, every file, no
                                        file too many or missing)

RESTORE (copy the files back)
  Copying data/ anywhere is enough for most files. Symbolic links in the
  original folder, execute bits and dates are in the listing,
  catalog/volumes/*/listing.tsv; the reader in tools/ brings them back too,
  checking every file as it copies. {ape_use}Build it with any C compiler:
    cc -O2 -pthread -o arvc tools/{repo}/src/arvc/*.c tools/{repo}/src/bagit/bagit.c \
      tools/{repo}/src/udfwrite/udfwrite.c tools/{repo}/src/rs03/rs03.c
    ./arvc verify .                    (or: ./arvc restore . ~/restored)
  Without a compiler, after copying data/ to DEST, recreate the links with:
    awk -F'\t' '$3 ~ /^link (recorded|broken)/ {{print $4 "\t" $5}}' \
      catalog/volumes/*/listing.tsv | while IFS="$(printf '\t')" read -r t p
      do ln -s "$t" "DEST/$p"; done

REPAIR (fix damage)
  1. Read the disc into an image, even if parts are unreadable, with either
     of (unread sectors are left as zeros, which the repair finds):
       ddrescue -b 2048 /dev/sr0 disc.iso disc.map       (GNU ddrescue)
       dvdisaster -d /dev/sr0 -r -i disc.iso             (dvdisaster Light,
         https://github.com/teaching-droid/dvdisaster-light: add --rescue)
{size_check}  2. Repair, then check, with the reader in tools/ (arvc or arv.com: see
     RESTORE); it needs nothing else:
       ./arvc check --image disc.iso --repair
     or with dvdisaster: dvdisaster -i disc.iso -f{dvdisaster_n}
     If arv cannot repair it, it prints the dvdisaster Light commands to
     paste: dvdisaster looks harder for the error correction's layout{n_note}.
  3. Still damaged? Every copy of this disc is identical. Put in another copy
     and read it into the same image; only the missing sectors are read:
       ddrescue -b 2048 /dev/sr0 disc.iso disc.map       (the same map file)
       dvdisaster -d /dev/sr0 -r -j 1 -i disc.iso
     then repair as in step 2.
  Then burn or mount disc.iso and verify as above.

CATALOGUE
  catalog.rec             this disc's record (GNU recutils format, plain text)
{catalog_lines}
TOOLS
  tools/{repo}/           the program that made this disc; its docs/spec/
                          folder describes the disc format, the UDF profile
                          and the error correction, so other programs can be
                          written to read and repair it
{bundle_line}{ape_line}