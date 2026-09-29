"""The archive catalogue: Disc and Event records in recfiles, plus per-disc manifests.

Home layout (the authoritative copy, default ~/.local/share/bluray-archive):

    archive.rec                 Disc / Event records for every disc
    manifests/<disc-id>.sha256  that disc's manifest-sha256.txt

Each disc carries a snapshot of this under catalog/.
"""

import datetime
import fnmatch
import os
import re
import shutil

from . import recfile

DESCRIPTORS = [
    recfile.Record(
        "Disc",
        [
            ("%rec", "Disc"),
            ("%doc", "One record per physical disc image (an OAIS AIP). Field names follow\n"
                     "Dublin Core terms where one fits: Title, Creator, Date, Description,\n"
                     "Subject, Coverage, Rights."),
            ("%key", "Id"),
            ("%mandatory", "Id Title Date"),
            ("%type", "Date date"),
            ("%type", "Files int"),
            ("%type", "Bytes int"),
        ],
    ),
    recfile.Record(
        "Event",
        [
            ("%rec", "Event"),
            ("%doc", "Preservation actions. Type uses the PREMIS eventType vocabulary:\n"
                     "https://id.loc.gov/vocabulary/preservation/eventType"),
            ("%mandatory", "Disc Type Date Outcome"),
            ("%type", "Outcome enum success failure warning"),
        ],
    ),
]

ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")


def default_home():
    env = os.environ.get("BLURAY_ARCHIVE_HOME")
    if env:
        return env
    base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.join(base, "bluray-archive")


def today():
    return datetime.date.today().isoformat()


def coverage_years(entries):
    """Year range of the payload's modification times, e.g. '2020-2025' or '2023'."""
    if not entries:
        return str(datetime.date.today().year)
    years = [datetime.date.fromtimestamp(e.mtime).year for e in entries]
    lo, hi = min(years), max(years)
    return str(lo) if lo == hi else "%d-%d" % (lo, hi)


def make_disc_id(coverage, set_name, number):
    return "%s_%s_%02d" % (coverage, set_name, number)


class Catalog:
    def __init__(self, records=None):
        self.discs = []
        self.events = []
        for r in records or []:
            if r.is_descriptor:
                continue
            if r.type == "Disc":
                self.discs.append(r)
            elif r.type == "Event":
                self.events.append(r)

    def records(self):
        """Records in file order: each type's descriptor is followed by its records."""
        disc_desc, event_desc = DESCRIPTORS
        return [disc_desc] + self.discs + [event_desc] + self.events

    def disc(self, disc_id):
        for d in self.discs:
            if d.get("Id") == disc_id:
                return d
        return None

    def events_for(self, disc_id):
        return [e for e in self.events if e.get("Disc") == disc_id]

    def next_number(self, set_name):
        numbers = [0]
        for d in self.discs:
            if d.get("Set") == set_name:
                m = re.search(r"_(\d+)$", d.get("Id", ""))
                if m:
                    numbers.append(int(m.group(1)))
        return max(numbers) + 1

    def subset(self, disc_ids):
        c = Catalog()
        c.discs = [d for d in self.discs if d.get("Id") in disc_ids]
        c.events = [e for e in self.events if e.get("Disc") in disc_ids]
        return c


class Home:
    def __init__(self, path=None):
        self.path = path or default_home()
        self.rec_path = os.path.join(self.path, "archive.rec")
        self.manifest_dir = os.path.join(self.path, "manifests")

    def load(self):
        if os.path.exists(self.rec_path):
            return Catalog(recfile.read(self.rec_path))
        return Catalog()

    def save(self, catalog):
        os.makedirs(self.manifest_dir, exist_ok=True)
        tmp = self.rec_path + ".tmp"
        recfile.write(tmp, catalog.records())
        os.replace(tmp, self.rec_path)

    def manifest_path(self, disc_id):
        return os.path.join(self.manifest_dir, disc_id + ".sha256")

    def store_manifest(self, disc_id, manifest_file):
        os.makedirs(self.manifest_dir, exist_ok=True)
        shutil.copyfile(manifest_file, self.manifest_path(disc_id))


def new_event(disc_id, type_, outcome, agent, note=None, date=None):
    r = recfile.Record("Event", [
        ("Disc", disc_id),
        ("Type", type_),
        ("Date", date or today()),
        ("Outcome", outcome),
        ("Agent", agent),
    ])
    if note:
        r.add("Note", note)
    return r


def write_snapshot(dest, catalog, manifest_sources, scope):
    """Write catalog/archive.rec and catalog/manifests/ into ``dest``."""
    os.makedirs(os.path.join(dest, "manifests"), exist_ok=True)
    header = recfile.Record("Snapshot", [
        ("%rec", "Snapshot"),
        ("%doc", "When this catalogue snapshot was taken and what it covers. The home\n"
                 "archive.rec stays authoritative for anything recorded after this date."),
    ])
    info = recfile.Record("Snapshot", [
        ("Date", today()),
        ("Scope", scope),
        ("Discs", str(len(catalog.discs))),
    ])
    recfile.write(os.path.join(dest, "archive.rec"), [header, info] + catalog.records())
    for disc_id, src in manifest_sources.items():
        shutil.copyfile(src, os.path.join(dest, "manifests", disc_id + ".sha256"))


def iter_manifest(path):
    """Yield (sha256, path) from a manifest file (paths are stored unencoded, see bag.encode_path)."""
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            digest, _, rel = line.partition("  ")
            yield digest, rel


def _matcher(pattern):
    pat = pattern.lower()
    if any(c in pat for c in "*?["):
        return lambda text: fnmatch.fnmatch(text.lower(), pat)
    return lambda text: pat in text.lower()


def find(home, catalog, pattern):
    """Return (disc_hits, file_hits). disc_hits: [Disc]; file_hits: [(Disc, path)]."""
    match = _matcher(pattern)
    disc_hits = [
        d for d in catalog.discs
        if any(match(v) for k, v in d.fields if k in ("Id", "Title", "Description", "Subject", "Note", "Coverage"))
    ]
    file_hits = []
    for d in catalog.discs:
        path = home.manifest_path(d.get("Id"))
        if not os.path.exists(path):
            continue
        for _, rel in iter_manifest(path):
            if match(rel):
                file_hits.append((d, rel))
    return disc_hits, file_hits
