"""Set vocabulary: a Dewey-like classification of discs.

<home>/sets.rec (created from default_sets.rec) lists sets with a readable
Code used in disc ids (PHOTO, TAXES, SCAN, TRIP) and a numeric Class that gives
shelf order and hierarchy (111 TRIP under 110 PHOTO under 100 Memories).

A disc records both its Set code and SetClass at burn time, so its place in
the classification survives later edits to the vocabulary.
"""

import os
import shutil

from . import discid, recfile

DEFAULT_SETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "default_sets.rec")


class SetInfo:
    def __init__(self, cls, code, name, description):
        self.cls, self.code, self.name, self.description = cls, code, name, description

    @property
    def label(self):
        return "%03d %s" % (self.cls, self.code or self.name)


def vocab_path(home):
    return os.path.join(home.path, "sets.rec")


def load(home, path=None):
    """[SetInfo] sorted by class; creates <home>/sets.rec from the default on first use."""
    if not path:
        path = vocab_path(home)
        if not os.path.exists(path):
            os.makedirs(home.path, exist_ok=True)
            shutil.copyfile(DEFAULT_SETS, path)
    out = []
    for r in recfile.read(path):
        if r.is_descriptor or not r.get("Class"):
            continue
        code = (r.get("Code") or "").strip().upper() or None
        if code and not discid.SET_RE.match(code):
            raise ValueError("%s: set code %r must be 2-8 capital letters or digits" % (path, code))
        out.append(SetInfo(int(r.get("Class")), code, r.get("Name") or code, r.get("Description") or ""))
    return sorted(out, key=lambda s: s.cls)


def by_code(vocab):
    return {s.code: s for s in vocab if s.code}


def lookup(vocab, code):
    return by_code(vocab).get((code or "").upper())


def ancestors(vocab, info):
    """Division and parent sets of a set, top first: 111 TRIP -> [100 Memories, 110 PHOTO]."""
    classes = {s.cls: s for s in vocab}
    out = []
    for parent in (info.cls // 100 * 100, info.cls // 10 * 10):
        if parent != info.cls and parent in classes and classes[parent] not in out:
            out.append(classes[parent])
    return out


def path_name(vocab, info):
    """'Memories > Photos > Trips and holidays'"""
    return " > ".join([a.name for a in ancestors(vocab, info)] + [info.name])


def guess(vocab, text):
    """Best set code for a folder-derived word ("Photos" -> PHOTO, "Tax_2019" -> TAXES), or None."""
    word = "".join(c for c in (text or "").upper() if c.isalnum())
    if not word:
        return None
    codes = by_code(vocab)
    if word in codes:
        return word
    for code in sorted(codes, key=len, reverse=True):
        if word.startswith(code) or (len(word) >= 3 and code.startswith(word)):
            return code
    singular = word.lower().rstrip("s")
    for info in codes.values():
        if any(w.rstrip("s") == singular for w in info.name.lower().replace(",", " ").split()):
            return info.code
    return None


def near(vocab, code):
    """Known codes that look like ``code`` (shared prefix), for 'did you mean' hints."""
    code = (code or "").upper()
    return [c for c in by_code(vocab) if c[:3] == code[:3] or c in code or code in c][:5]
