"""The archive catalogue: Disc and Event records in recfiles, plus each volume's index files.

Home layout (the working copy: a .arv folder, found as described in homes.py):

    config/         what you set up: sets.rec (vocabulary), tags.rec (tag vocabulary)
    catalog/        the catalogue, laid out exactly like catalog/ on every disc:
      archive.rec                       Disc / Event / Location / Collection records
      volumes/<disc-id>/manifest.sha256 that disc's manifest-sha256.txt
      volumes/<disc-id>/listing.tsv     size, modification time and path of each file
      volumes/<disc-id>/formats.csv     PRONOM format of each file (when Siegfried is installed)
      volumes/<disc-id>/tags.tsv        folder tags (optional)
    drafts/         work in progress before `make`
    cache/          rebuildable: archive.sqlite, models/, runtime/ (marked with CACHEDIR.TAG,
                    so Borg, restic and GNU tar skip it; .gitignore "*" keeps it out of git)

One folder per volume, as LTFS keeps one index per tape. A disc's catalog/ (read-only) can be
used directly as a catalogue (--home catalog). A home in the older flat layout is moved into
this one on first use; only the tool's own files are moved.
"""

import datetime
import fnmatch
import os
import re
import shutil

from . import homes, recfile

DESCRIPTORS = [
    recfile.Record(
        "Disc",
        [
            ("%rec", "Disc"),
            ("%doc", "One record per physical disc image (an OAIS AIP). Field names follow\n"
                     "Dublin Core terms where one fits: Title, Creator, Date, Description,\n"
                     "Subject, Coverage, Rights. Uuid is the machine identity of the image\n"
                     "(copies burned from one image share it); Id is for people and is\n"
                     "derived from Set, Sequence and Coverage (EDTF) under IdScheme, so it\n"
                     "can always be regenerated and checked (see docs/smart-archive-format.md)."),
            ("%key", "Id"),
            ("%mandatory", "Id Title Date"),
            ("%type", "Uuid uuid"),
            ("%type", "Sequence int"),
            ("%type", "Date date"),
            ("%type", "Files int"),
            ("%type", "Bytes int"),
            ("%type", "Access enum public private sealed"),
        ],
    ),
    recfile.Record(
        "Location",
        [
            ("%rec", "Location"),
            ("%doc", "Places where discs are kept: site, room, shelf, box. Parent makes a tree,\n"
                     "so moving a box moves every disc in it. A Disc's Location field (one per\n"
                     "place its copies are kept) names a Code here, or is free text."),
            ("%key", "Code"),
            ("%mandatory", "Code Name"),
        ],
    ),
    recfile.Record(
        "Collection",
        [
            ("%rec", "Collection"),
            ("%doc", "Virtual folders across discs. Item is DISC-ID (a whole disc), DISC-ID:folder/\n"
                     "or DISC-ID:folder/file (paths relative to the disc's data/). Parent nests\n"
                     "collections. Snapshots on other discs carry only items they may show."),
            ("%key", "Code"),
            ("%mandatory", "Code Name"),
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
    "manifests": "manifest.sha256",  # that disc's manifest-sha256.txt
    "listings": "listing.tsv",       # size, modified time, path
    "formats": "formats.csv",        # PRONOM format identification (optional)
    "tags": "tags.tsv",              # folder <TAB> comma-separated tags (optional, e.g. from --llm)
}
# the flat layout of format 0.1: <kind>/<disc-id><extension>
_OLD_EXTENSIONS = {"manifests": ".sha256", "listings": ".tsv", "formats": ".csv", "tags": ".tags"}


def volume_file(catalog_dir, kind, disc_id):
    """Path of one of a volume's index files inside a catalog/ folder (home or disc)."""
    return os.path.join(catalog_dir, "volumes", disc_id, DISC_FILE_KINDS[kind])

ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
LOCATION_RE = re.compile(r"^[A-Z0-9][A-Z0-9_-]{0,23}$")
COLLECTION_RE = re.compile(r"^[A-Z0-9][A-Z0-9_-]{0,31}$")


def parse_item(item):
    """A collection Item -> (disc id, path relative to data/, is_folder). Whole disc: path ''."""
    disc_id, _, path = item.strip().partition(":")
    path = path.strip().lstrip("/")
    return disc_id.strip(), path, path == "" or path.endswith("/")

# Access: who may see a disc's description and file list on *other* discs' catalogue snapshots.
#   public   anywhere, including discs given to other people (--snapshot set)
#   private  your own full snapshots only (the default)
#   sealed   other discs carry only its identity and location, never its titles, notes or files
ACCESS_LEVELS = ("public", "private", "sealed")
DEFAULT_ACCESS = "private"
SEALED_FIELDS = ("Id", "Uuid", "IdScheme", "Set", "Category", "Path", "Sequence", "Coverage", "Date", "Part",
                 "Location", "Copies", "MediaId", "Access")
WITHHELD = "title, description, notes, subjects and file lists (Access: sealed)"


def access(disc):
    value = (disc.get("Access") or DEFAULT_ACCESS).strip().lower()
    return value if value in ACCESS_LEVELS else DEFAULT_ACCESS


def sealed_view(disc):
    """What other discs may carry about a sealed disc."""
    r = recfile.Record("Disc", [(k, v) for k, v in disc.fields if k in SEALED_FIELDS])
    r.fields.insert(1, ("Title", "(sealed disc)"))
    r.add("Withheld", WITHHELD)
    return r


def default_home():
    """The home used when none is given: see homes.find (a .arv folder above, the machine config...)."""
    return homes.find()[0]


def today():
    return datetime.date.today().isoformat()


def coverage_years(entries):
    """Year range of the payload's modification times as EDTF: '2020/2025' or '2023'."""
    if not entries:
        return str(datetime.date.today().year)
    years = [datetime.date.fromtimestamp(e.mtime).year for e in entries]
    lo, hi = min(years), max(years)
    return str(lo) if lo == hi else "%d/%d" % (lo, hi)


class Catalog:
    def __init__(self, records=None):
        self.discs = []
        self.locations = []
        self.collections = []
        self.events = []
        for r in records or []:
            if r.is_descriptor:
                continue
            if r.type == "Disc":
                self.discs.append(r)
            elif r.type == "Location":
                self.locations.append(r)
            elif r.type == "Collection":
                self.collections.append(r)
            elif r.type == "Event":
                self.events.append(r)

    def records(self):
        """Records in file order: each type's descriptor is followed by its records."""
        disc_desc, location_desc, collection_desc, event_desc = DESCRIPTORS
        out = [disc_desc] + self.discs
        if self.locations:
            out += [location_desc] + self.locations
        if self.collections:
            out += [collection_desc] + self.collections
        return out + [event_desc] + self.events

    # ------------------------------------------------------------ collections

    def collection(self, code):
        code = (code or "").strip().upper()
        return next((c for c in self.collections if c.get("Code") == code), None)

    def collection_chain(self, code):
        """[Collection, its parent, ...] from ``code`` up to the top (cycles are cut)."""
        chain, seen = [], set()
        col = self.collection(code)
        while col is not None and col.get("Code") not in seen:
            seen.add(col.get("Code"))
            chain.append(col)
            col = self.collection(col.get("Parent"))
        return chain

    def collection_path(self, code):
        chain = self.collection_chain(code)
        return " / ".join(c.get("Name") or c.get("Code") for c in reversed(chain)) if chain else code

    def collections_for(self, disc_ids, sealed=()):
        """Collections as a snapshot may carry them: items only for ``disc_ids``, and no paths
        on ``sealed`` discs (only the whole-disc item). Collections left empty are dropped,
        unless a kept collection is inside them."""
        kept = {}
        for col in self.collections:
            items = []
            for item in col.get_all("Item"):
                disc_id, path, _ = parse_item(item)
                if disc_id in disc_ids and not (path and disc_id in sealed):
                    items.append(item)
            if items:
                kept[col.get("Code")] = items
        codes = set()
        for code in kept:
            codes.update(c.get("Code") for c in self.collection_chain(code))
        out = []
        for col in self.collections:
            if col.get("Code") in codes:
                out.append(recfile.Record("Collection", [(k, v) for k, v in col.fields if k != "Item"]
                                          + [("Item", i) for i in kept.get(col.get("Code"), [])]))
        return out

    # ------------------------------------------------------------ locations

    def location(self, code):
        code = (code or "").strip().upper()
        return next((l for l in self.locations if l.get("Code") == code), None)

    def location_chain(self, code):
        """[Location, its parent, ...] from ``code`` up to the top (cycles are cut)."""
        chain, seen = [], set()
        loc = self.location(code)
        while loc is not None and loc.get("Code") not in seen:
            seen.add(loc.get("Code"))
            chain.append(loc)
            loc = self.location(loc.get("Parent"))
        return chain

    def location_path(self, code):
        """'Home / Study / Box 3' for a Location code; free text is returned as it is."""
        chain = self.location_chain(code)
        if not chain:
            return code
        return " / ".join(l.get("Name") or l.get("Code") for l in reversed(chain))

    def where(self, disc):
        """Every place a disc's copies are kept, as readable paths ('' when not recorded)."""
        return "; ".join(self.location_path(l) for l in disc.get_all("Location"))

    def locations_under(self, code):
        """Codes of ``code`` and every location inside it."""
        code = code.strip().upper()
        return {l.get("Code") for l in self.locations
                if code in [c.get("Code") for c in self.location_chain(l.get("Code"))]} | {code}

    def locations_for(self, discs):
        """The Location records that ``discs`` refer to, with the places containing them."""
        codes = set()
        for d in discs:
            for value in d.get_all("Location"):
                codes.update(l.get("Code") for l in self.location_chain(value))
        return [l for l in self.locations if l.get("Code") in codes]

    def disc(self, disc_id):
        for d in self.discs:
            if d.get("Id") == disc_id:
                return d
        return None

    def events_for(self, disc_id):
        return [e for e in self.events if e.get("Disc") == disc_id]

    def next_number(self, set_name):
        """Next unused sequence number in a set (numbers are never reused)."""
        from . import discid
        numbers = [0]
        for d in self.discs:
            if d.get("Set") == set_name:
                if (d.get("Sequence") or "").isdigit():
                    numbers.append(int(d.get("Sequence")))
                else:
                    parsed = discid.parse(d.get("Id", ""))
                    if parsed:
                        numbers.append(parsed["sequence"])
        return max(numbers) + 1

    def subset(self, disc_ids):
        c = Catalog()
        c.discs = [d for d in self.discs if d.get("Id") in disc_ids]
        c.events = [e for e in self.events if e.get("Disc") in disc_ids]
        c.locations = self.locations_for(c.discs)
        c.collections = self.collections_for(disc_ids, self.sealed_ids())
        return c

    def sealed_ids(self):
        return {d.get("Id") for d in self.discs if access(d) == "sealed"}

    def shared_view(self):
        """This catalogue as other discs may carry it: sealed discs cut down to their identity."""
        c = Catalog()
        sealed = {d.get("Id") for d in self.discs if access(d) == "sealed"}
        c.discs = [sealed_view(d) if d.get("Id") in sealed else d for d in self.discs]
        c.events = [e for e in self.events if e.get("Disc") not in sealed]
        c.locations = list(self.locations)
        c.collections = self.collections_for({d.get("Id") for d in self.discs}, sealed)
        return c


CACHEDIR_TAG = ("Signature: 8a477f597d28d172789f06886806bc55\n"
                "# This folder holds caches made by the archive tool (bluray-archival-workflow).\n"
                "# Everything here can be rebuilt; backup tools may skip it.\n"
                "# See https://bford.info/cachedir/\n")


class Home:
    """The working catalogue folder (see the module docstring for its layout)."""

    def __init__(self, path=None):
        self.path = path or default_home()
        if (os.path.isdir(os.path.join(self.path, "volumes"))
                and os.path.exists(os.path.join(self.path, "archive.rec"))):
            # a bare catalog/ folder, e.g. on a disc: read it in place
            self.catalog_dir = self.path
            self.config_dir = os.path.join(self.path, "config")
            self.drafts_dir = os.path.join(self.path, "drafts")
            self.cache_dir = os.path.join(self.path, "cache")
        else:
            self.catalog_dir = os.path.join(self.path, "catalog")
            self.config_dir = os.path.join(self.path, "config")
            self.drafts_dir = os.path.join(self.path, "drafts")
            self.cache_dir = os.path.join(self.path, "cache")
            if os.path.exists(os.path.join(self.path, "archive.rec")):
                self._migrate_flat_layout()
        self.rec_path = os.path.join(self.catalog_dir, "archive.rec")
        self.volumes_dir = os.path.join(self.catalog_dir, "volumes")
        self.sqlite_path = os.path.join(self.cache_dir, "archive.sqlite")

    def _migrate_flat_layout(self):
        """Move a format-0.1 home (archive.rec, manifests/, sets.rec ... at the top) into this layout."""
        top = self.path
        os.makedirs(self.catalog_dir, exist_ok=True)
        os.replace(os.path.join(top, "archive.rec"), os.path.join(self.catalog_dir, "archive.rec"))
        for kind, ext in _OLD_EXTENSIONS.items():
            old = os.path.join(top, kind)
            if not os.path.isdir(old):
                continue
            for name in os.listdir(old):
                if name.endswith(ext):
                    dest = volume_file(self.catalog_dir, kind, name[:-len(ext)])
                    os.makedirs(os.path.dirname(dest), exist_ok=True)
                    os.replace(os.path.join(old, name), dest)
            if not os.listdir(old):
                os.rmdir(old)
        for name, folder in (("sets.rec", self.config_dir), ("tags.rec", self.config_dir),
                             ("drafts", self.path), ("models", self.cache_dir), ("runtime", self.cache_dir)):
            old = os.path.join(top, name)
            if os.path.exists(old) and folder != self.path:
                self.ensure(folder)
                os.replace(old, os.path.join(folder, name))
        old_index = os.path.join(top, "archive.sqlite")
        if os.path.exists(old_index):
            os.remove(old_index)  # rebuildable; `arv index` makes a new one in cache/

    def ensure(self, folder):
        """Create one of the home's folders; cache/ gets its CACHEDIR.TAG and .gitignore."""
        os.makedirs(folder, exist_ok=True)
        if os.path.abspath(folder) == os.path.abspath(self.cache_dir):
            for name, text in (("CACHEDIR.TAG", CACHEDIR_TAG), (".gitignore", "*\n")):
                marker = os.path.join(folder, name)
                if not os.path.exists(marker):
                    with open(marker, "w", encoding="utf-8", newline="\n") as f:
                        f.write(text)
        return folder

    def load(self):
        if os.path.exists(self.rec_path):
            return Catalog(recfile.read(self.rec_path))
        return Catalog()

    def save(self, catalog):
        os.makedirs(self.catalog_dir, exist_ok=True)
        tmp = self.rec_path + ".tmp"
        recfile.write(tmp, catalog.records())
        os.replace(tmp, self.rec_path)

    def manifest_path(self, disc_id):
        return self.disc_file("manifests", disc_id)

    def listing_path(self, disc_id):
        return self.disc_file("listings", disc_id)

    def disc_file(self, kind, disc_id):
        return volume_file(self.catalog_dir, kind, disc_id)

    def disc_files(self, disc_id):
        """{kind: path} of the volume's index files that exist at home."""
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


FORMAT_NAME = "smart-archive"
FORMAT_VERSION = "0.2"  # 0.2: per-volume index files in catalog/volumes/<disc-id>/

ARCHIVE_DESCRIPTOR = recfile.Record("Archive", [
    ("%rec", "Archive"),
    ("%doc", "Entry point of a smart-archive disc: which format and version this is, which disc,\n"
             "and where its other catalogue files are (paths relative to the disc root).\n"
             "Specification: tools/bluray-archival-workflow/docs/smart-archive-format.md"),
    ("%mandatory", "Format Version Disc Uuid"),
])

# (Archive field, path pattern on disc); a field is written only when the file exists
ARCHIVE_POINTERS = [
    ("Manifest", "manifest-sha256.txt"),
    ("Listing", "catalog/volumes/{id}/listing.tsv"),
    ("Tags", "catalog/volumes/{id}/tags.tsv"),
    ("Formats", "catalog/volumes/{id}/formats.csv"),
    ("Snapshot", "catalog/archive.rec"),
    ("Viewer", "index.html"),
    ("Payload", "data/"),
]


def archive_records(disc, root):
    """[descriptor, record] for the Archive entry record at the top of a disc's catalog.rec."""
    r = recfile.Record("Archive", [("Format", FORMAT_NAME), ("Version", FORMAT_VERSION),
                                   ("Disc", disc.get("Id")), ("Uuid", disc.get("Uuid") or "")])
    for field, pattern in ARCHIVE_POINTERS:
        rel = pattern.format(id=disc.get("Id"))
        if field == "Payload" or os.path.exists(os.path.join(root, rel)):
            r.add(field, rel)
    return [ARCHIVE_DESCRIPTOR, r]


SNAPSHOT_DESCRIPTOR = recfile.Record("Snapshot", [
    ("%rec", "Snapshot"),
    ("%doc", "When this catalogue snapshot was taken and what it covers. The home\n"
             "archive.rec stays authoritative for anything recorded after this date."),
])


def write_snapshot(dest, catalog, disc_files, scope):
    """Write catalog/archive.rec plus each volume's index files (volumes/<disc-id>/) into ``dest``.

    disc_files: {disc_id: {kind: path}}
    """
    info = recfile.Record("Snapshot", [
        ("Date", today()),
        ("Scope", scope),
        ("Discs", str(len(catalog.discs))),
    ])
    os.makedirs(dest, exist_ok=True)
    recfile.write(os.path.join(dest, "archive.rec"), [SNAPSHOT_DESCRIPTOR, info] + catalog.records())
    os.makedirs(os.path.join(dest, "volumes"), exist_ok=True)
    for disc_id, files in disc_files.items():
        for kind, src in files.items():
            if src and os.path.exists(src):
                target = volume_file(dest, kind, disc_id)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copyfile(src, target)


def _event_key(e):
    return tuple(e.fields)


def merge(home_catalog, other, prefer_other=False):
    """Merge ``other`` into ``home_catalog``. Returns (added disc ids, updated disc ids, added events).

    A sealed disc's cut-down record (it has Withheld) never replaces a full one.
    """
    added, updated, events = [], [], 0
    for d in other.discs:
        existing = home_catalog.disc(d.get("Id"))
        if existing is None:
            home_catalog.discs.append(d)
            added.append(d.get("Id"))
        elif prefer_other and existing.fields != d.fields and not (d.get("Withheld") and not existing.get("Withheld")):
            existing.fields = list(d.fields)
            updated.append(d.get("Id"))
    for col in other.collections:   # items are unioned: a filtered copy never removes any
        existing = home_catalog.collection(col.get("Code"))
        if existing is None:
            home_catalog.collections.append(col)
            continue
        if prefer_other:
            keep = [(k, v) for k, v in col.fields if k != "Item"]
            existing.fields = keep + [(k, v) for k, v in existing.fields if k == "Item"]
        have = set(existing.get_all("Item"))
        existing.fields += [("Item", i) for i in col.get_all("Item") if i not in have]
    for loc in other.locations:
        existing = home_catalog.location(loc.get("Code"))
        if existing is None:
            home_catalog.locations.append(loc)
        elif prefer_other:
            existing.fields = list(loc.fields)
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


DISC_SEARCH_FIELDS = ("Id", "Title", "Description", "Subject", "Note", "Coverage", "Category", "Path", "Location")


def find_discs(catalog, pattern):
    match = _matcher(pattern)
    return [d for d in catalog.discs if any(match(v) for k, v in d.fields if k in DISC_SEARCH_FIELDS)]


TAG_NAMESPACES = ("person", "place", "event", "source", "project")  # suggested; any word works


def normalise_tag(tag):
    """'Place : Kyoto ' -> 'place:kyoto'. A tag is a plain word or phrase, or namespace:value."""
    tag = " ".join((tag or "").replace(",", " ").split()).lower()
    ns, sep, value = tag.partition(":")
    if sep and ns.strip() and " " not in ns.strip():
        return "%s:%s" % (ns.strip(), value.strip())
    return tag


def split_tag(tag):
    """('place', 'kyoto') for 'place:kyoto'; ('', 'travel') for a plain tag."""
    ns, sep, value = tag.partition(":")
    if sep and ns and " " not in ns:
        return ns, value
    return "", tag


def hierarchical(tag):
    """XMP lr:hierarchicalSubject form (as Lightroom and digiKam write it): 'place|kyoto'."""
    ns, value = split_tag(tag)
    return "%s|%s" % (ns, value) if ns else value


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
