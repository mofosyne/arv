"""Set vocabulary: words arranged in a directed acyclic graph.

<home>/config/sets.rec (created from default_sets.rec) lists codes such as PHOTO,
TRIP, SCAN, TAXES, PROJ, CODE. Each may have several Parent codes (SCAN is
under both RECORDS and PHOTO), like SKOS "broader" in library thesauri.
Cycles are rejected when the vocabulary is loaded.

A disc has one Set (its id prefix, numbering and shelf place) and any number
of extra Category codes. It records every vocabulary path of each as Path
fields ("MEMORIES/PHOTO/TRIP"), so the disc stays self-describing without the
vocabulary file, and later vocabulary edits don't reclassify old discs.

Entries may also have (all optional, repeatable where it makes sense):
  Alias      other words for the same thing ("holiday", "travel" for TRIP), like
             SKOS altLabel or Hydrus tag siblings: typed or guessed words resolve
             to the code, so the vocabulary doesn't drift into near-duplicates.
  ScopeNote  what belongs here and what doesn't (SKOS scopeNote).
  Match      a glob that marks files as belonging here ("*.kicad_pcb" for ELEC),
             like Paperless-ngx matching rules: deterministic suggestions that
             need no model. A pattern without "/" is compared with each file and
             folder name, one with "/" with the whole path; case is ignored.
"""

import fnmatch
import os
import shutil

from . import discid, recfile

DEFAULT_SETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "default_sets.rec")


class VocabError(ValueError):
    pass


def word(text):
    """Comparison form of a code, alias or folder word: capitals and digits only."""
    return "".join(c for c in (text or "").upper() if c.isalnum())


def path_matches(pattern, path):
    """Does a Match glob apply to a relative file path? (see the module docstring)"""
    pattern, path = pattern.strip().lower(), path.lower()
    if "/" in pattern:
        return fnmatch.fnmatchcase(path, pattern.strip("/")) or fnmatch.fnmatchcase(path, pattern.strip("/") + "/*")
    return any(fnmatch.fnmatchcase(part, pattern) for part in path.split("/"))


class SetInfo:
    def __init__(self, code, name, description, parents, order, aliases=(), scope_note="", matches=()):
        self.code, self.name, self.description = code, name, description
        self.parents, self.order = parents, order
        self.aliases, self.scope_note, self.matches = list(aliases), scope_note, list(matches)


class Vocabulary:
    def __init__(self, entries, path=None):
        self.path = path
        self.entries = {e.code: e for e in entries}
        for e in entries:
            for p in e.parents:
                if p not in self.entries:
                    raise VocabError("%s: %s has unknown parent %s" % (path, e.code, p))
        self.aliases = {}
        for e in entries:
            for a in e.aliases:
                key = word(a)
                other = self.aliases.get(key)  # an alias may equal another code: codes win in resolve()
                if other and other != e.code:
                    raise VocabError("%s: alias %r of %s is already %s" % (path, a, e.code, other))
                self.aliases[key] = e.code
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

    def resolve(self, text):
        """Code for a code or an alias ("trips" -> TRIP), or None. Codes win over aliases."""
        key = word(text)
        for k in (key, key[:-1] if len(key) > 3 and key.endswith("S") else None):  # also "trips"
            if k in self.entries:
                return k
            if k in self.aliases:
                return self.aliases[k]
        return None

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
        """Best code for a folder-derived word ("Photos" -> PHOTO, "Holiday" -> TRIP), or None.

        Unlike resolve(), aliases come first: a folder called "Projects" is usually one
        project set (PROJ, which has the alias), not the whole PROJECTS group.
        """
        key = word(text).lstrip("0123456789") or word(text)  # "2019_Vacation" -> VACATION
        if not key:
            return None
        if key in self.aliases:
            return self.aliases[key]
        if key in self.entries:
            return key
        for code in sorted(self.entries, key=len, reverse=True):
            if key.startswith(code) or (len(key) >= 3 and code.startswith(key)):
                return code
        for alias in sorted(self.aliases, key=len, reverse=True):
            if len(alias) >= 4 and key.startswith(alias):
                return self.aliases[alias]
        singular = key.lower().rstrip("s")
        for e in self.entries.values():
            if any(w.rstrip("s") == singular for w in e.name.lower().replace(",", " ").split()):
                return e.code
        return None

    def match(self, paths):
        """{code: number of paths its Match rules claim}, for the codes that claim any."""
        rules = [(e.code, m) for e in self.entries.values() for m in e.matches]
        counts = {}
        for path in paths:
            for code in {c for c, m in rules if path_matches(m, path)}:
                counts[code] = counts.get(code, 0) + 1
        return counts

    def near(self, code):
        code = (code or "").upper()
        return [c for c in self.entries if c[:3] == code[:3] or c in code or code in c][:5]


def vocab_path(home):
    return os.path.join(home.config_dir, "sets.rec")


def load(home, path=None):
    """The vocabulary; creates <home>/config/sets.rec from the default on first use."""
    if not path:
        path = vocab_path(home)
        if not os.path.exists(path):
            os.makedirs(home.config_dir, exist_ok=True)
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
                               [p.strip().upper() for p in r.get_all("Parent") if p.strip()], order,
                               [a.strip() for a in r.get_all("Alias") if a.strip()], r.get("ScopeNote") or "",
                               [m.strip() for m in r.get_all("Match") if m.strip()]))
    return Vocabulary(entries, path)


def disc_codes(disc):
    """Every code a disc belongs to: its Set, Categories and all their recorded ancestors."""
    codes = {disc.get("Set")} | set(disc.get_all("Category"))
    for path in disc.get_all("Path"):
        codes.update(path.split("/"))
    return {c for c in codes if c}
