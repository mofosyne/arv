"""The archive catalogue: Disc and Event records in recfiles, plus each volume's index files.

Home layout (the working copy: a .arv folder, found as described in homes.py):

    config/         what you set up: sets.rec (vocabulary), tags.rec (tag vocabulary)
    catalog/        the catalogue, laid out exactly like catalog/ on every disc:
      archive.rec                       Disc / Binding / Location / Selection / Collection / Revision / Object / Event / Appraisal records
      volumes/<disc-id>/manifest.sha256 that disc's manifest-sha256.txt
      volumes/<disc-id>/listing.tsv     size, modification time and path of each file
      volumes/<disc-id>/formats.csv     PRONOM format of each file (when Siegfried is installed)
      volumes/<disc-id>/tags.tsv        folder tags (optional)
    drafts/         work in progress before `make`
    cache/          rebuildable: models/, runtime/ (marked with CACHEDIR.TAG,
                    so Borg, restic and GNU tar skip it; .gitignore "*" keeps it out of git)

One folder per volume, as LTFS keeps one index per tape. A disc's catalog/ (read-only) can be
used directly as a catalogue (--home catalog). A home in the older flat layout is moved into
this one on first use; only the tool's own files are moved.
"""

import datetime
import fnmatch
import os
import re

from . import homes, recfile

# The record descriptors (the catalogue's TYPES, in the order written, then the Archive and Snapshot
# records on discs) live in arv's data/descriptors.rec (src/arv/data), shared with arv.
TYPES = ("Home", "Disc", "Binding", "Location", "Selection", "Collection", "Revision", "Object", "Event",
         "Appraisal")
_ALL_DESCRIPTORS = recfile.read(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "arv", "data",
                                             "descriptors.rec"))
DESCRIPTORS = _ALL_DESCRIPTORS[:len(TYPES)]
assert tuple(d.type for d in DESCRIPTORS) == TYPES

# Per-disc files kept at home and in each disc's catalog/ snapshot: folder -> extension
DISC_FILE_KINDS = {
    "manifests": "manifest.sha256",  # that disc's manifest-sha256.txt
    "listings": "listing.tsv",       # size, modified time, path
    "formats": "formats.csv",        # PRONOM format identification (optional)
    "tags": "tags.tsv",              # folder <TAB> comma-separated tags (optional, e.g. from --llm)
    "extents": "extents.tsv",        # where each file starts in the image (Binding data; never on
                                     # the volume it describes, see docs/spec/archival-udf.md)
}
# the flat layout of format 0.1: <kind>/<disc-id><extension>
_OLD_EXTENSIONS = {"manifests": ".sha256", "listings": ".tsv", "formats": ".csv", "tags": ".tags"}


def volume_file(catalog_dir, kind, disc_id):
    """Path of one of a volume's index files inside a catalog/ folder (home or disc)."""
    return os.path.join(catalog_dir, "volumes", disc_id, DISC_FILE_KINDS[kind])

# No dots: in file names everything before the first dot is the id (TRIP-01_2019_4.noecc.iso)
ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]*$")
LOCATION_RE = re.compile(r"^[A-Z0-9][A-Z0-9_-]{0,23}$")
SELECTION_RE = re.compile(r"^[A-Z0-9][A-Z0-9_-]{0,31}$")


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


def default_home():
    """The home used when none is given: see homes.find (a .arv folder above, the machine config...)."""
    return homes.find()[0]


def today_date():
    """Today; or the day of $SOURCE_DATE_EPOCH (UTC) when set, for reproducible discs and tests."""
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch and epoch.strip().isdigit():
        return datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc).date()
    return datetime.date.today()


def today():
    return today_date().isoformat()


class Catalog:
    def __init__(self, records=None):
        self.groups = {t: [] for t in TYPES}
        for r in records or []:
            if r.is_descriptor:
                continue
            if r.type in self.groups:
                self.groups[r.type].append(r)
        (self.homes, self.discs, self.bindings, self.locations, self.selections, self.collections, self.revisions,
         self.objects, self.events, self.appraisals) = (self.groups[t] for t in TYPES)

    def records(self):
        """Records in file order: each type's descriptor is followed by its records (Disc and
        Event always, the rest when there are any)."""
        out = []
        for desc, kind in zip(DESCRIPTORS, TYPES):
            if self.groups[kind] or kind in ("Disc", "Event"):
                out += [desc] + self.groups[kind]
        return out

    # ------------------------------------------------------------ bindings


    # ------------------------------------------------------------ selections


    # ------------------------------------------------------------ locations

    def binding(self, disc_id):
        return next((b for b in self.bindings if b.get("Volume") == disc_id), None)

    def with_binding(self, disc):
        """The disc's record with its Binding's fields added after it (for showing to people).
        Older records kept Media, Filesystem and Ecc in the Disc record itself; they show as they are."""
        b = self.binding(disc.get("Id"))
        r = recfile.Record("Disc", list(disc.fields))
        if b is not None:
            r.fields += [(k, v) for k, v in b.fields if k != "Volume" and r.get(k) is None]
        return r

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


    def disc(self, disc_id):
        for d in self.discs:
            if d.get("Id") == disc_id:
                return d
        return None

    def events_for(self, disc_id):
        return [e for e in self.events if e.get("Disc") == disc_id]


CACHEDIR_TAG = ("Signature: 8a477f597d28d172789f06886806bc55\n"
                "# This folder holds caches made by arv (Archive, Record, Verify).\n"
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
            os.remove(old_index)  # the retired SQLite index (find scans the plain-text lists)

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


AUTHORSHIP = ("automatic", "suggested", "accepted", "edited", "human")
MODEL_AGENTS = ("llm:", "embeddings:", "vision:")


def is_model(agent):
    """A model's judgement (not software following rules): its agent names llm:, embeddings:..."""
    return any(p in (agent or "") for p in MODEL_AGENTS)


def default_authorship(agents):
    if any(is_model(a) for a in agents):
        return "suggested"    # never claim a review that was not recorded
    if any((a or "").startswith("human:") for a in agents):
        return "human"
    return "automatic"


def authorship(record):
    """A record's Authorship; for events written before it existed, read from the Agent text."""
    value = record.get("Authorship")
    if value in AUTHORSHIP:
        return value
    agents = record.get_all("Agent")
    text = " ".join(agents)
    if "(unreviewed)" in text:
        return "suggested"
    if "+ owner review" in text:
        return "accepted"     # format 0.3: reviewed, whether it was changed was not recorded
    return default_authorship(agents)


def reviewed_agents(agent, how):
    """Agents of work a model suggested and a person then saw (accepted / edited): both of them."""
    agents = [agent] if isinstance(agent, str) else list(agent)
    if how in ("accepted", "edited") and not any(a.startswith("human:") for a in agents):
        agents.append(person())
    return agents


def new_event(disc_id, type_, outcome, agent, note=None, date=None, obj=None, authorship=None):
    """agent: one name or a list (e.g. a model and the person who reviewed its suggestion)."""
    agents = [agent] if isinstance(agent, str) else list(agent)
    r = recfile.Record("Event", [("Disc", disc_id)] if disc_id else [("Object", obj)])
    r.fields += [
        ("Type", type_),
        ("Date", date or today()),
        ("Outcome", outcome),
        ("Authorship", authorship or default_authorship(agents)),
    ]
    r.fields += [("Agent", a) for a in agents]
    if note:
        r.add("Note", note)
    return r


def person():
    """The Agent of a change made by hand: human:LOGIN."""
    try:
        import getpass
        return "human:" + getpass.getuser()
    except Exception:  # no login name (e.g. some containers)
        return "human:unknown"


FORMAT_NAME = "smart-archive"
FORMAT_VERSION = "0.4"  # 0.2: per-volume index files in catalog/volumes/<disc-id>/; 0.3: Binding records;
                        # 0.4: Authorship, Appraisal records, listing 2

ARCHIVE_DESCRIPTOR = _ALL_DESCRIPTORS[6]

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


SNAPSHOT_DESCRIPTOR = _ALL_DESCRIPTORS[7]


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
