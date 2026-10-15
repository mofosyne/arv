#!/usr/bin/env python3
"""Make a large synthetic home for measuring arv (issue #37): N discs, 3 copies each (two discs
on shelves and an image on a NAS), a fixity check of every copy, a move of some copies, one data
object a disc, and each disc's manifest and listing at home. Ids are made as arv makes them (disc
ids with their check character, event ids from their content), so arv audit finds nothing wrong.

    python3 dev-tools/bigcat.py HOME [DISCS] [FILES-A-DISC]
    arv --home HOME objects ; arv --home HOME todo ; arv --home HOME audit
"""
import hashlib
import os
import sys
import uuid

ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def check_char(payload):                    # src/arv/discid.c: discid_check
    total, factor = 0, 2
    for c in reversed(payload.upper()):
        if c not in ALPHABET:
            continue
        addend = factor * ALPHABET.index(c)
        total += addend // 36 + addend % 36
        factor = 1 if factor == 2 else 2
    return ALPHABET[(36 - total % 36) % 36]


def record(fields):
    return "\n".join(f"{k}: {v}" for k, v in fields)


def event(fields):                          # src/arv/archive.c: event_content_id
    eid = hashlib.sha256(record(fields).encode()).hexdigest()[:32]
    return eid, record([("EventId", eid)] + fields)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    home = sys.argv[1]
    discs = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
    files = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    rng = uuid.UUID(int=37)                  # reproducible: Uuids from a counter, not random
    counter = [rng.int]

    def next_uuid():
        counter[0] += 1
        return str(uuid.UUID(int=counter[0] & ((1 << 128) - 1), version=4))

    cat = os.path.join(home, "catalog")
    os.makedirs(os.path.join(home, "config"), exist_ok=True)
    os.makedirs(os.path.join(cat, "volumes"), exist_ok=True)
    disc_recs, events, objects = [], [], []
    places = [("HOME", "Home", None), ("SHELF", "Study shelf", "HOME"), ("OFFSITE", "A friend's house", None),
              ("NAS", "The NAS", "HOME")]
    for n in range(1, discs + 1):
        year = 2000 + n % 25
        body = f"PHOTO-{n:02d}_{year}"
        did = f"{body}_{check_char(body)}"
        date = f"20{20 + n % 6}-0{1 + n % 9}-1{n % 9}"
        disc_recs.append(record([("Id", did), ("Uuid", next_uuid()), ("IdScheme", "set-seq-coverage/1"),
                                 ("Title", f"Photos {year}, part {n}"), ("Set", "PHOTO"), ("Path", "MEMORIES/PHOTO"),
                                 ("Sequence", n), ("Coverage", year), ("Date", date), ("Files", files),
                                 ("Bytes", files * 3_000_000), ("Access", "private")]))
        common = [("Disc", did)]
        _, e = event(common + [("Type", "creation"), ("Date", date), ("Outcome", "success"), ("Authorship", "automatic"),
                               ("Agent", "arv@bigcat"), ("Note", f"image {did}.iso")])
        events.append(e)
        reps = []
        for letter, form, where, temp, extra in (
                ("A", "disc", "SHELF", "cold", [("Identifier", f"barcode:ARV{n:05d}")]),
                ("B", "disc", "OFFSITE", "cold", [("Identifier", f"bca:{n:016X}")]),
                ("C", "iso", "NAS", "warm", [("Path", f"/nas/arv/{did}.iso")])):
            fields = common + [("Type", "replication"), ("Date", date), ("Outcome", "success"), ("Authorship", "human"),
                               ("Agent", "human:archivist"), ("Note", f"copy {letter}"), ("Copy", letter),
                               ("Form", form), ("Location", where), ("Temperature", temp)] + extra + [("ReadBack", "identical")]
            rid, e = event(fields)
            events.append(e)
            reps.append((letter, rid))
        for letter, rid in reps:              # a check of every copy
            _, e = event(common + [("Type", "fixity check"), ("Date", f"2026-0{1 + n % 9}-01"), ("Outcome", "success"),
                                   ("Authorship", "automatic"), ("Agent", "arv@bigcat"), ("Note", f"copy {letter} read back"),
                                   ("Copy", letter), ("Replication", rid)])
            events.append(e)
        if n % 3 == 0:                        # some copies moved since
            letter, rid = reps[1]
            _, e = event(common + [("Type", "metadata modification"), ("Date", "2026-05-01"), ("Outcome", "success"),
                                   ("Authorship", "human"), ("Agent", "human:archivist"),
                                   ("Note", f"copy {letter} Location: OFFSITE -> HOME"), ("Copy", letter),
                                   ("Replication", rid), ("Location", "HOME"), ("Sets", f"Location:{rid}")])
            events.append(e)
        lines, listing = [], ["# arv listing 2\tsize (bytes)\tmodified (UTC, ISO 8601)\tkind\tlink target\tpath (relative to data/)"]
        for f in range(files):
            p = f"photos/{year}/IMG_{n:05d}_{f:04d}.JPG"
            lines.append(f"{hashlib.sha256(p.encode()).hexdigest()}  data/{p}")
            listing.append(f"3000000\t{year}-07-01T00:00:00Z\tfile\t-\t{p}")
        manifest = "\n".join(lines) + "\n"
        vol = os.path.join(cat, "volumes", did)
        os.makedirs(vol, exist_ok=True)
        with open(os.path.join(vol, "manifest.sha256"), "w") as out:
            out.write(manifest)
        with open(os.path.join(vol, "listing.tsv"), "w") as out:
            out.write("\n".join(listing) + "\n")
        tree_text = "".join(f"{l[:64]}  {l[66 + len('data/photos/'):]}\n" for l in lines)
        objects.append(record([("Uuid", next_uuid()), ("Version", 1), ("Name", str(year)), ("Kind", "folder"),
                               ("Tree", hashlib.sha256(tree_text.encode()).hexdigest()), ("Disc", did), ("Path", "photos"),
                               ("Files", files), ("Bytes", files * 3_000_000), ("Date", date),
                               ("Source", f"photos/{year}-{n}")]))
    loc_recs = [record([("Code", c), ("Name", name)] + ([("Parent", p)] if p else [])) for c, name, p in places]
    parts = ["%rec: Disc", *disc_recs, "%rec: Location", *loc_recs, "%rec: Object", *objects, "%rec: Event", *events]
    with open(os.path.join(cat, "archive.rec"), "w") as out:
        out.write("\n\n".join(parts) + "\n")
    print(f"{home}: {discs} discs, {len(events)} events, {discs * files} files")


main()
