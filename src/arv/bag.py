"""BagIt (RFC 8493) writing for a payload that is *not* moved.

The payload folder stays untouched: manifests are computed from it with
``data/`` prefixed paths, and the image builder grafts it in as ``data/``.
Only the tag files are written to a staging directory.
"""

import hashlib
import re
import os
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


def scan_payload(src, progress=True):
    """Walk ``src`` and hash every regular file. Symlinks are rejected."""
    files, symlinks = [], []
    for root, dirs, names in os.walk(src):
        dirs.sort()
        for name in dirs:
            if os.path.islink(os.path.join(root, name)):
                symlinks.append(os.path.join(root, name))
        for name in sorted(names):
            full = os.path.join(root, name)
            if os.path.islink(full):
                symlinks.append(full)
            elif os.path.isfile(full):
                files.append(full)
    ambiguous = [f for f in files if AMBIGUOUS_RE.search(os.path.relpath(f, src))]
    if ambiguous:
        raise ValueError(
            "file names containing line breaks or %0A / %0D / %25 cannot be listed\n"
            "unambiguously in BagIt manifests; please rename:\n  " + "\n  ".join(ambiguous[:20])
        )
    if symlinks:
        raise ValueError(
            "symlinks are not supported in the payload (resolve or remove them):\n  "
            + "\n  ".join(symlinks[:20])
        )

    total = sum(os.path.getsize(f) for f in files)
    done = 0
    entries = []
    for n, full in enumerate(files, 1):
        st = os.stat(full)
        rel = os.path.relpath(full, src).replace(os.sep, "/")
        entries.append(Entry(rel, st.st_size, st.st_mtime, hash_file(full)))
        done += st.st_size
        if progress and sys.stderr.isatty():
            pct = 100 * done / total if total else 100
            print("\rHashing %d/%d files (%.0f%%)" % (n, len(files), pct), end="", file=sys.stderr)
    if progress and sys.stderr.isatty():
        print(file=sys.stderr)
    return entries


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
