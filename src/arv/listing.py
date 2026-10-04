"""File listings on each disc: catalog/volumes/<id>/listing.tsv, one line per file with size and date.

The manifests only have checksums; these add what a person (or Katalog) wants to see, and what
the manifests cannot hold: which files were executable, and every symbolic link in the source
folder with what was done with it (docs/spec/smart-archive-format.md, "Listing TSV" and "Links").

Version 2 columns: size, modified, kind, link target, path. Kind is words:
  file [executable]                 a file, in data/
  link copied [executable]          a link to a file: the file's bytes are in data/ under the link's name
  link copied folder                a link to a folder whose files were copied (--links copy)
  link recorded folder              a link to a folder inside the source, noted here only
  link recorded external            a link to something outside the source, noted here only
  link broken                       a link to nothing, noted here only
Rows that are only noted have "-" as size; they are not in data/ or the manifests.
"""

import datetime

HEADER = "# arv listing 2\tsize (bytes)\tmodified (UTC, ISO 8601)\tkind\tlink target\tpath (relative to data/)\n"


def write_listing(path, entries, links=()):
    """Listing of `entries` (the files in data/) and `links` (links that are only noted), by path."""
    rows = sorted([(e, False) for e in entries] + [(e, True) for e in links],
                  key=lambda r: r[0].path.encode("utf-8"))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        # Versioned header so readers know the columns; the path is always the last column
        f.write(HEADER)
        for e, noted_only in rows:
            mtime = datetime.datetime.fromtimestamp(e.mtime, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            size = "-" if noted_only else str(e.size)
            f.write("%s\t%s\t%s\t%s\t%s\n" % (size, mtime, getattr(e, "kind", "file"), getattr(e, "link", "") or "-", e.path))


def read_rows(path):
    """Every row as a dict: size (int or None), modified, kind, link, path. Reads versions 1 and 2."""
    version = 1
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if line.startswith("#"):
                words = line[1:].split("\t", 1)[0].split()
                if len(words) == 3 and words[1] == "listing" and words[2].isdigit():
                    version = int(words[2])
                continue
            if not line:
                continue
            if version == 1:
                size, mtime, rel = line.split("\t", 2)
                kind, link = "file", ""
            else:
                size, mtime, kind, link, rel = line.split("\t", 4)
                link = "" if link == "-" else link
            yield {"size": None if size == "-" else int(size), "modified": mtime, "kind": kind,
                   "link": link, "path": rel}


def read_listing(path):
    """(size, modified, path) of each file in data/; links that are only noted are skipped."""
    for row in read_rows(path):
        if row["size"] is not None:
            yield str(row["size"]), row["modified"], row["path"]
