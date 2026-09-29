"""Set vocabulary: words arranged in a directed acyclic graph.

<home>/sets.rec (created from default_sets.rec) lists codes such as PHOTO,
TRIP, SCAN, TAXES, PROJ, CODE. Each may have several Parent codes (SCAN is
under both RECORDS and PHOTO), like SKOS "broader" in library thesauri.
Cycles are rejected when the vocabulary is loaded.

A disc has one Set (its id prefix, numbering and shelf place) and any number
of extra Category codes. It records every vocabulary path of each as Path
fields ("MEMORIES/PHOTO/TRIP"), so the disc stays self-describing without the
vocabulary file, and later vocabulary edits don't reclassify old discs.
"""

import os
import shutil

from . import discid, recfile

DEFAULT_SETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "default_sets.rec")


class VocabError(ValueError):
    pass


class SetInfo:
    def __init__(self, code, name, description, parents, order):
        self.code, self.name, self.description = code, name, description
        self.parents, self.order = parents, order


class Vocabulary:
    def __init__(self, entries, path=None):
        self.path = path
        self.entries = {e.code: e for e in entries}
        for e in entries:
            for p in e.parents:
                if p not in self.entries:
                    raise VocabError("%s: %s has unknown parent %s" % (path, e.code, p))
        self._check_acyclic()

    def _check_acyclic(self):
        state = {}

        def visit(code, trail):
            if state.get(code) == "done":
                return
            if state.get(code) == "active":
                cycle = trail[trail.index(code):] + [code]
                raise VocabError("%s: cycle in parents: %s" % (self.path, " -> ".join(cycle)))
            state[code] = "active"
            for p in self.entries[code].parents:
                visit(p, trail + [code])
            state[code] = "done"

        for code in self.entries:
            visit(code, [])

    def get(self, code):
        return self.entries.get((code or "").upper())

    def children(self, code):
        kids = [e for e in self.entries.values() if code in e.parents]
        return sorted(kids, key=lambda e: (e.order, e.code))

    def roots(self):
        return sorted((e for e in self.entries.values() if not e.parents), key=lambda e: (e.order, e.code))

    def paths(self, code):
        """Every path from a top-level entry down to ``code``: ['MEMORIES/PHOTO/SCAN', 'RECORDS/SCAN']."""
        e = self.get(code)
        if not e:
            return []
        if not e.parents:
            return [e.code]
        return sorted({"%s/%s" % (p, e.code) for parent in e.parents for p in self.paths(parent)})

    def ancestors(self, code):
        """All codes above ``code`` (any path)."""
        out = set()
        for path in self.paths(code):
            out.update(path.split("/")[:-1])
        return out

    def guess(self, text):
        """Best code for a folder-derived word ("Photos" -> PHOTO, "Holiday" -> TRIP), or None."""
        word = "".join(c for c in (text or "").upper() if c.isalnum())
        if not word:
            return None
        if word in self.entries:
            return word
        for code in sorted(self.entries, key=len, reverse=True):
            if word.startswith(code) or (len(word) >= 3 and code.startswith(word)):
                return code
        singular = word.lower().rstrip("s")
        for e in self.entries.values():
            if any(w.rstrip("s") == singular for w in e.name.lower().replace(",", " ").split()):
                return e.code
        return None

    def near(self, code):
        code = (code or "").upper()
        return [c for c in self.entries if c[:3] == code[:3] or c in code or code in c][:5]


def vocab_path(home):
    return os.path.join(home.path, "sets.rec")


def load(home, path=None):
    """The vocabulary; creates <home>/sets.rec from the default on first use."""
    if not path:
        path = vocab_path(home)
        if not os.path.exists(path):
            os.makedirs(home.path, exist_ok=True)
            shutil.copyfile(DEFAULT_SETS, path)
    entries = []
    for r in recfile.read(path):
        if r.is_descriptor or not r.get("Code"):
            continue
        code = r.get("Code").strip().upper()
        if not discid.SET_RE.match(code):
            raise VocabError("%s: code %r must be 2-8 capital letters or digits" % (path, code))
        order = int(r.get("Order")) if (r.get("Order") or "").strip().isdigit() else 50
        entries.append(SetInfo(code, r.get("Name") or code, r.get("Description") or "",
                               [p.strip().upper() for p in r.get_all("Parent") if p.strip()], order))
    return Vocabulary(entries, path)


def disc_codes(disc):
    """Every code a disc belongs to: its Set, Categories and all their recorded ancestors."""
    codes = {disc.get("Set")} | set(disc.get_all("Category"))
    for path in disc.get_all("Path"):
        codes.update(path.split("/"))
    return {c for c in codes if c}
