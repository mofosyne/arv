"""Disposable SQLite search index built from the plain-text catalogue.

recutils and plain scans get slow past ~100k files; this makes `arv find`
fast at millions. Delete archive.sqlite at any time; `arv index` rebuilds it.
"""

import os
import sqlite3

from . import catalog as catalog_mod
from .listing import read_listing

SCHEMA = """
CREATE TABLE files (disc TEXT, path TEXT, lpath TEXT, size INTEGER);
CREATE INDEX files_disc ON files(disc);
"""


def _sources(home, disc_id):
    listing = home.listing_path(disc_id)
    if os.path.exists(listing):
        return listing, None
    manifest = home.manifest_path(disc_id)
    return None, (manifest if os.path.exists(manifest) else None)


def build(home, cat):
    home.ensure(home.cache_dir)
    tmp = home.sqlite_path + ".tmp"
    if os.path.exists(tmp):
        os.remove(tmp)
    db = sqlite3.connect(tmp)
    db.executescript(SCHEMA)
    for d in cat.discs:
        disc_id = d.get("Id")
        listing, manifest = _sources(home, disc_id)
        if listing:
            rows = ((disc_id, "data/" + rel, ("data/" + rel).lower(), int(size))
                    for size, _mtime, rel in read_listing(listing))
        elif manifest:
            rows = ((disc_id, rel, rel.lower(), None) for _, rel in catalog_mod.iter_manifest(manifest))
        else:
            continue
        db.executemany("INSERT INTO files VALUES (?,?,?,?)", rows)
    db.commit()
    db.close()
    os.replace(tmp, home.sqlite_path)


def is_fresh(home):
    """True when archive.sqlite is newer than every file it was built from."""
    if not os.path.exists(home.sqlite_path):
        return False
    built = os.path.getmtime(home.sqlite_path)
    sources = [home.rec_path]
    if os.path.isdir(home.volumes_dir):
        for disc_id in os.listdir(home.volumes_dir):
            sources += [home.manifest_path(disc_id), home.listing_path(disc_id)]
    return all(os.path.getmtime(p) <= built for p in sources if os.path.exists(p))


def find(home, cat, pattern):
    """Same results as catalog.find, answered from the index."""
    pat = pattern.lower()
    if any(c in pat for c in "*?["):
        sql, arg = "SELECT disc, path FROM files WHERE lpath GLOB ? ORDER BY rowid", pat
    else:
        sql, arg = "SELECT disc, path FROM files WHERE INSTR(lpath, ?) > 0 ORDER BY rowid", pat
    db = sqlite3.connect(home.sqlite_path)
    try:
        rows = db.execute(sql, (arg,)).fetchall()
    finally:
        db.close()
    by_id = {d.get("Id"): d for d in cat.discs}
    return catalog_mod.find_discs(cat, pattern), [(by_id[i], p) for i, p in rows if i in by_id]
