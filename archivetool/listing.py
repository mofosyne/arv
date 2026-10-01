"""File listings on each disc: catalog/listings/<id>.tsv, one line per file with size and date.

The manifests only have checksums; these add what a person (or Katalog) wants to see.
"""


def write_listing(path, entries):
    """Plain-text listing with sizes and dates (the manifests only have checksums)."""
    import datetime
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        # Versioned header so readers know the columns; the path is always the last column
        f.write("# smart-archive listing 1\tsize (bytes)\tmodified (UTC, ISO 8601)\tpath (relative to data/)\n")
        for e in entries:
            mtime = datetime.datetime.fromtimestamp(e.mtime, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            f.write("%d\t%s\t%s\n" % (e.size, mtime, e.path))


def read_listing(path):
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            size, mtime, rel = line.split("\t", 2)
            yield size, mtime, rel
