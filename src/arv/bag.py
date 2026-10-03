"""BagIt (RFC 8493) writing for a payload that is *not* moved.

The payload folder stays untouched: manifests are computed from it with
``data/`` prefixed paths, and the image builder grafts it in as ``data/``.
Only the tag files are written to a staging directory.
"""

import hashlib
import os
import re
import stat
import sys
from dataclasses import dataclass, field

ALGORITHMS = ("sha256", "sha512")
CHUNK = 1024 * 1024


@dataclass
class Entry:
    path: str  # relative to the payload root, POSIX separators
    size: int
    mtime: float
    hashes: dict = field(default_factory=dict)
    kind: str = "file"   # words, see listing.py: "file", "file executable", "link copied", ...
    link: str = ""       # a link's target, as written in the link
    source: str = None   # where the bytes are read from, when not <payload>/<path> (copied links)


AMBIGUOUS_RE = re.compile(r"[\r\n]|%(0A|0D|25)", re.IGNORECASE)


def encode_path(path):
    """Manifest paths are written as-is.

    RFC 8493 also asks for ``%`` to be encoded as ``%25``, but bagit-python (the
    Library of Congress reference implementation) only decodes %0A/%0D, and
    ``sha256sum -c`` decodes nothing. Writing paths raw keeps all three
    agreeing; names that would be ambiguous are rejected in scan_payload().
    """
    return path


def hash_file(path):
    digests = {alg: hashlib.new(alg) for alg in ALGORITHMS}
    with open(path, "rb") as f:
        while True:
            chunk = f.read(CHUNK)
            if not chunk:
                break
            for d in digests.values():
                d.update(chunk)
    return {alg: d.hexdigest() for alg, d in digests.items()}


LINK_POLICIES = ("default", "record", "copy")


class Payload(list):
    """The files that go into data/ (Entries, in path order), plus the links that are only
    listed (``links``: Entries whose ``kind`` is "link recorded ...", "link broken", or
    "link copied folder" for a folder link whose files were copied)."""

    def __init__(self, entries=(), links=(), policy="default"):
        super().__init__(entries)
        self.links = list(links)
        self.policy = policy

    def link_summary(self):
        """'links: 2 copied, 1 recorded, 1 broken (policy: default)', or None without links."""
        counts = {}
        for e in list(self) + self.links:
            if e.kind.startswith("link"):
                word = e.kind.split()[1]
                counts[word] = counts.get(word, 0) + 1
        if not counts:
            return None
        order = ("copied", "recorded", "broken")
        return "links: %s (policy: %s)" % (
            ", ".join("%d %s" % (counts[w], w) for w in order if w in counts), self.policy)


def _inside(path, root):
    return path == root or path.startswith(root.rstrip(os.sep) + os.sep)


def scan_payload(src, progress=True, links="default"):
    """Walk ``src`` and hash every regular file; returns a Payload.

    Symbolic links follow the links policy (docs/smart-archive-format.md, "Links"), and every
    link is noted in the listing whatever happens to it:
    - a link to a file inside ``src`` is copied: the target's bytes under the link's name;
    - a link to a folder inside ``src`` is recorded only (``copy``: its files are copied too);
    - a link to anything outside ``src`` is refused (``record``: recorded; ``copy``: copied);
    - a broken link is recorded and warned about; copying a link that loops is an error.
    Devices, sockets and pipes are refused.
    """
    if links not in LINK_POLICIES:
        raise ValueError("unknown links policy %r" % links)
    root = os.path.realpath(src)
    files, recorded, refused, special, loops = [], [], [], [], []

    def link_entry(rel, full, kind, target):
        st = os.lstat(full)
        return Entry(rel, 0, st.st_mtime, kind=kind, link=target)

    def walk(disk, rel, ancestors, via_link):
        """disk: the folder to read (may be reached through links); rel: its path in data/."""
        try:
            names = sorted(os.listdir(disk))
        except OSError as err:
            raise ValueError("cannot read %s: %s" % (disk, err))
        for name in names:
            full = os.path.join(disk, name)
            r = rel + "/" + name if rel else name
            if not os.path.islink(full):
                st = os.lstat(full)
                if stat.S_ISDIR(st.st_mode):
                    walk(full, r, ancestors | {os.path.realpath(full)}, via_link)
                elif stat.S_ISREG(st.st_mode):
                    files.append((r, full, via_link))
                else:
                    special.append(full)
                continue
            target = os.readlink(full)
            real = os.path.realpath(full)
            if not os.path.exists(full):
                recorded.append(link_entry(r, full, "link broken", target))
                print("Warning: broken link %s -> %s (recorded in the listing, not archived)"
                      % (r, target), file=sys.stderr)
                continue
            inside = _inside(real, root)
            if not inside and links == "default":
                refused.append("%s -> %s" % (r, target))
                continue
            if os.path.isdir(real):
                if links == "copy" and any(_inside(a, real) for a in ancestors):
                    loops.append("%s -> %s" % (r, target))   # following it would never end
                elif links == "copy":
                    recorded.append(link_entry(r, full, "link copied folder", target))
                    walk(full, r, ancestors | {real}, True)
                else:
                    recorded.append(link_entry(r, full, "link recorded folder" if inside
                                               else "link recorded external", target))
            elif os.path.isfile(real):
                if not inside and links == "record":
                    recorded.append(link_entry(r, full, "link recorded external", target))
                else:
                    files.append((r, full, target))
            else:
                special.append(full)

    walk(root, "", {root}, None)   # via: None, a file link's target, or True inside a copied folder
    rels = [r for r, _, _ in files] + [e.path for e in recorded]
    ambiguous = [r for r in rels if AMBIGUOUS_RE.search(r)]
    if ambiguous:
        raise ValueError(
            "file names containing line breaks or %0A / %0D / %25 cannot be listed\n"
            "unambiguously in BagIt manifests; please rename:\n  " + "\n  ".join(ambiguous[:20])
        )
    if special:
        raise ValueError("only files, folders and links can be archived; please remove:\n  "
                         + "\n  ".join(special[:20]))
    if loops:
        raise ValueError("links that loop back to a folder containing them:\n  " + "\n  ".join(loops[:20]))
    if refused:
        raise ValueError(
            "links pointing outside the folder (use --links record to note them in the listing,\n"
            "or --links copy to archive what they point to):\n  " + "\n  ".join(refused[:20]))

    total = sum(os.path.getsize(full) for _, full, _ in files)
    done = 0
    entries = []
    for n, (rel, full, via) in enumerate(files, 1):
        st = os.stat(full)
        kind = "file"
        if isinstance(via, str):
            kind = "link copied"
        if st.st_mode & 0o111:
            kind += " executable"
        e = Entry(rel, st.st_size, st.st_mtime, hash_file(full), kind=kind)
        if via is not None:   # a file link, or a file inside a copied folder link
            e.source = os.path.realpath(full)
            e.link = via if isinstance(via, str) else ""
        entries.append(e)
        done += st.st_size
        if progress and sys.stderr.isatty():
            pct = 100 * done / total if total else 100
            print("\rHashing %d/%d files (%.0f%%)" % (n, len(files), pct), end="", file=sys.stderr)
    if progress and sys.stderr.isatty():
        print(file=sys.stderr)
    entries.sort(key=lambda e: e.path.encode("utf-8"))
    return Payload(entries, sorted(recorded, key=lambda e: e.path.encode("utf-8")), links)


def payload_oxum(entries):
    return "%d.%d" % (sum(e.size for e in entries), len(entries))


def write_manifest(path, lines):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for digest, rel in lines:
            f.write("%s  %s\n" % (digest, encode_path(rel)))


def write_bag_tags(stage, entries, bag_info):
    """Write bagit.txt, bag-info.txt and payload manifests into ``stage``."""
    with open(os.path.join(stage, "bagit.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("BagIt-Version: 1.0\nTag-File-Character-Encoding: UTF-8\n")
    with open(os.path.join(stage, "bag-info.txt"), "w", encoding="utf-8", newline="\n") as f:
        for label, value in bag_info:
            # Long or multi-line values continue on lines starting with whitespace
            f.write("%s: %s\n" % (label, str(value).replace("\n", "\n  ")))
    for alg in ALGORITHMS:
        write_manifest(
            os.path.join(stage, "manifest-%s.txt" % alg),
            [(e.hashes[alg], "data/" + e.path) for e in entries],
        )


def write_tagmanifests(stage):
    """Checksum every file in ``stage`` except the tagmanifests themselves. Call last."""
    tag_files = []
    for root, dirs, names in os.walk(stage):
        dirs.sort()
        for name in sorted(names):
            rel = os.path.relpath(os.path.join(root, name), stage).replace(os.sep, "/")
            if not rel.startswith("tagmanifest-"):
                tag_files.append(rel)
    hashed = {rel: hash_file(os.path.join(stage, rel)) for rel in tag_files}
    for alg in ALGORITHMS:
        write_manifest(
            os.path.join(stage, "tagmanifest-%s.txt" % alg),
            [(hashed[rel][alg], rel) for rel in tag_files],
        )
