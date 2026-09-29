"""The archive catalogue: Disc and Event records in recfiles, plus per-disc manifests.

Home layout (the authoritative copy, default ~/.local/share/bluray-archive):

    archive.rec                 Disc / Event records for every disc
    manifests/<disc-id>.sha256  that disc's manifest-sha256.txt
    listings/<disc-id>.tsv      size, modification time and path of each file
    formats/<disc-id>.csv       PRONOM format of each file (when Siegfried is installed)
    tags/<disc-id>.tags         folder tags (optional)
    archive.sqlite              search index built by `archive index` (disposable)

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

# Per-disc files kept at home and in each disc's catalog/ snapshot: folder -> extension
DISC_FILE_KINDS = {
    "manifests": ".sha256",  # that disc's manifest-sha256.txt
    "listings": ".tsv",      # size, modified time, path
    "formats": ".csv",       # PRONOM format identification (optional)
    "tags": ".tags",         # folder <TAB> comma-separated tags (optional, e.g. from --llm)
}

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
        self.listing_dir = os.path.join(self.path, "listings")
        self.sqlite_path = os.path.join(self.path, "archive.sqlite")

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
        return self.disc_file("manifests", disc_id)

    def listing_path(self, disc_id):
        return self.disc_file("listings", disc_id)

    def disc_file(self, kind, disc_id):
        return os.path.join(self.path, kind, disc_id + DISC_FILE_KINDS[kind])

    def disc_files(self, disc_id):
        """{kind: path} of the per-disc files that exist at home."""
        out = {}
        for kind in DISC_FILE_KINDS:
            path = self.disc_file(kind, disc_id)
            if os.path.exists(path):
                out[kind] = path
        return out

    def store_disc_files(self, disc_id, files):
        """files: {kind: source path}"""
        for kind, src in files.items():
            if src and os.path.exists(src):
                dest = self.disc_file(kind, disc_id)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                shutil.copyfile(src, dest)


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


SNAPSHOT_DESCRIPTOR = recfile.Record("Snapshot", [
    ("%rec", "Snapshot"),
    ("%doc", "When this catalogue snapshot was taken and what it covers. The home\n"
             "archive.rec stays authoritative for anything recorded after this date."),
])


def write_snapshot(dest, catalog, disc_files, scope):
    """Write catalog/archive.rec plus manifests/, listings/ and formats/ into ``dest``.

    disc_files: {disc_id: {kind: path}}
    """
    info = recfile.Record("Snapshot", [
        ("Date", today()),
        ("Scope", scope),
        ("Discs", str(len(catalog.discs))),
    ])
    os.makedirs(dest, exist_ok=True)
    recfile.write(os.path.join(dest, "archive.rec"), [SNAPSHOT_DESCRIPTOR, info] + catalog.records())
    for kind in ("manifests", "listings"):
        os.makedirs(os.path.join(dest, kind), exist_ok=True)
    for disc_id, files in disc_files.items():
        for kind, src in files.items():
            if src and os.path.exists(src):
                os.makedirs(os.path.join(dest, kind), exist_ok=True)
                shutil.copyfile(src, os.path.join(dest, kind, disc_id + DISC_FILE_KINDS[kind]))


def _event_key(e):
    return tuple(e.fields)


def merge(home_catalog, other, prefer_other=False):
    """Merge ``other`` into ``home_catalog``. Returns (added disc ids, updated disc ids, added events)."""
    added, updated, events = [], [], 0
    for d in other.discs:
        existing = home_catalog.disc(d.get("Id"))
        if existing is None:
            home_catalog.discs.append(d)
            added.append(d.get("Id"))
        elif prefer_other and existing.fields != d.fields:
            existing.fields = list(d.fields)
            updated.append(d.get("Id"))
    known = {_event_key(e) for e in home_catalog.events}
    for e in other.events:
        if _event_key(e) not in known:
            home_catalog.events.append(e)
            known.add(_event_key(e))
            events += 1
    return added, updated, events


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


DISC_SEARCH_FIELDS = ("Id", "Title", "Description", "Subject", "Note", "Coverage")


def find_discs(catalog, pattern):
    match = _matcher(pattern)
    return [d for d in catalog.discs if any(match(v) for k, v in d.fields if k in DISC_SEARCH_FIELDS)]


def write_tags(path, folder_tags, captions=None):
    """folder <TAB> comma-separated tags [<TAB> caption from sampled images]"""
    captions = captions or {}
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# folder (relative to data/)\ttags\tcaption (what sampled images show, if analysed)\n")
        for folder in sorted(set(folder_tags) | set(captions)):
            line = "%s\t%s" % (folder, ", ".join(folder_tags.get(folder, [])))
            if captions.get(folder):
                line += "\t" + captions[folder].replace("\t", " ").replace("\n", " ")
            f.write(line + "\n")


def read_tag_info(path):
    """{folder: (tags, caption)}"""
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line and not line.startswith("#") and "\t" in line:
                folder, rest = line.split("\t", 1)
                tags, _, caption = rest.partition("\t")
                out[folder] = ([t.strip() for t in tags.split(",") if t.strip()], caption.strip())
    return out


def read_tags(path):
    return {folder: tags for folder, (tags, _) in read_tag_info(path).items()}


def find_tags(home, catalog, pattern):
    """[(Disc, folder, tags)] where a folder tag or image caption matches ``pattern``."""
    match = _matcher(pattern)
    hits = []
    for d in catalog.discs:
        path = home.disc_file("tags", d.get("Id"))
        if os.path.exists(path):
            for folder, (tags, caption) in read_tag_info(path).items():
                if any(match(t) for t in tags) or (caption and match(caption)):
                    hits.append((d, folder, tags + ([caption] if caption and match(caption) else [])))
    return hits


def find(home, catalog, pattern):
    """Scan every manifest. Returns (disc_hits, file_hits); file_hits: [(Disc, path)]."""
    match = _matcher(pattern)
    file_hits = []
    for d in catalog.discs:
        path = home.manifest_path(d.get("Id"))
        if not os.path.exists(path):
            continue
        for _, rel in iter_manifest(path):
            if match(rel):
                file_hits.append((d, rel))
    return find_discs(catalog, pattern), file_hits
