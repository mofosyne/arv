"""Structured, human-readable disc ids that are derived from metadata fields.

The Disc record stores the parts (Set, Sequence, Coverage, IdScheme); the Id is
regenerated from them and must match, so the id written on a disc is never the
only copy of that information.

Scheme "set-seq-coverage/1":

    ID       = SET "-" SEQ "_" COVERAGE "_" CHECK
    SET      = 2-8 capital letters or digits              PHOTOS
    SEQ      = 2-3 digits, per set, never reused           07
    COVERAGE = compact form of the EDTF Coverage field     2015-2024, 2019, 201907-201908, 199X
    CHECK    = Luhn mod 36 check character over SET, SEQ and COVERAGE (letters and digits only)

    PHOTOS-07_2015-2024_K

Set and number come first so the part that identifies the disc survives the
16-character Joliet volume label ("PHOTOS-07_2015-2") and sorts sets together.
The whole id fits the 32-character ISO 9660 / UDF volume label.

Coverage is stored in the Disc record as EDTF (ISO 8601-2, Library of Congress
Extended Date/Time Format), which can say "1995~" (circa), "199X" (some year
in the 1990s), "2019-07/2019-08" or "[1998,1999]". The id uses a compact form
without qualifiers.

Older ids ("2020-2025_PROJECTS_01", scheme "coverage-set-seq/0") remain valid.
"""

import re

SCHEME = "set-seq-coverage/1"
LEGACY_SCHEME = "coverage-set-seq/0"
ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
MAX_LABEL = 32

SET_RE = re.compile(r"^[A-Z0-9]{2,8}$")
ID_RE = re.compile(r"^(?P<set>[A-Z0-9]{2,8})-(?P<seq>\d{2,3})_(?P<coverage>[0-9X]{4,6}(?:-[0-9X]{4,6})?)_(?P<check>[0-9A-Z])$")
LEGACY_RE = re.compile(r"^(?P<coverage>\d{4}(?:-\d{4})?)_(?P<set>[A-Z0-9-]+)_(?P<seq>\d+)$")


class IdError(ValueError):
    pass


# ---------------------------------------------------------------- check character

def check_char(payload):
    """Luhn mod 36 over the letters and digits of ``payload``; catches any single wrong
    character and most swaps of neighbouring characters."""
    chars = [c for c in payload.upper() if c in ALPHABET]
    total, factor = 0, 2
    for c in reversed(chars):
        addend = factor * ALPHABET.index(c)
        total += addend // 36 + addend % 36
        factor = 1 if factor == 2 else 2
    return ALPHABET[(36 - total % 36) % 36]


# ---------------------------------------------------------------- coverage (EDTF subset)

EDTF_DATE = re.compile(r"^(?P<year>\d{3}[\dX]|\d{2}XX)(?:-(?P<month>\d{2}|XX)(?:-(?P<day>\d{2}|XX))?)?$")


def _strip(value):
    return value.strip().strip("~?%").replace("~", "").replace("?", "").replace("%", "")


def _date_range(value):
    """(first year, last year) of one EDTF date such as 2019, 2019-07, 199X, 19XX."""
    m = EDTF_DATE.match(_strip(value))
    if not m:
        raise IdError("not an EDTF date: %r" % value)
    y = m.group("year")
    return int(y.replace("X", "0")), int(y.replace("X", "9"))


def coverage_range(coverage):
    """(first year, last year) covered, or None if unknown. Accepts EDTF and legacy 'YYYY-YYYY'."""
    if not coverage:
        return None
    c = coverage.strip()
    if re.fullmatch(r"\d{4}-\d{4}", c) and int(c[5:]) > 12:  # legacy year range
        return int(c[:4]), int(c[5:])
    if c.startswith("[") or c.startswith("{"):
        years = [_date_range(v) for v in c.strip("[]{}").split(",") if v.strip() and ".." not in v]
        return (min(a for a, _ in years), max(b for _, b in years)) if years else None
    if "/" in c:
        start, end = c.split("/", 1)
        lo = _date_range(start)[0] if start not in ("", "..") else None
        hi = _date_range(end)[1] if end not in ("", "..") else None
        if lo is None and hi is None:
            return None
        return (lo if lo is not None else hi, hi if hi is not None else lo)
    return _date_range(c)


def to_edtf(coverage):
    """Normalise user input: legacy '2015-2024' becomes the EDTF interval '2015/2024'."""
    c = coverage.strip()
    if re.fullmatch(r"\d{4}-\d{4}", c) and int(c[5:]) > 12:
        return c[:4] + "/" + c[5:]
    coverage_range(c)  # validates
    return c


def compact(coverage):
    """EDTF coverage -> the compact form used in ids: 2015/2024 -> 2015-2024, 2019-07 -> 201907."""
    c = to_edtf(coverage)
    if c.startswith("[") or c.startswith("{"):
        lo, hi = coverage_range(c)
        return str(lo) if lo == hi else "%d-%d" % (lo, hi)

    def one(v):
        m = EDTF_DATE.match(_strip(v))
        if not m:
            raise IdError("cannot use %r in an id" % v)
        return m.group("year") + (m.group("month") or "")

    if "/" in c:
        start, end = c.split("/", 1)
        if start in ("", "..") or end in ("", ".."):
            raise IdError("open-ended coverage %r cannot be used in an id" % coverage)
        a, b = one(start), one(end)
        return a if a == b else "%s-%s" % (a, b)
    return one(c)


# ---------------------------------------------------------------- compose / parse

def normalise_set(name):
    s = re.sub(r"[^A-Za-z0-9]", "", name or "").upper()
    if len(s) < 2:
        raise IdError("set name %r needs at least 2 letters or digits" % name)
    return s[:8]


def compose(set_name, sequence, coverage):
    """The id for these fields under the current scheme."""
    s = normalise_set(set_name)
    body = "%s-%02d_%s" % (s, int(sequence), compact(coverage))
    disc_id = "%s_%s" % (body, check_char(body))
    if len(disc_id) > MAX_LABEL:
        raise IdError("id %r is longer than %d characters" % (disc_id, MAX_LABEL))
    return disc_id


def parse(disc_id):
    """{'scheme', 'set', 'sequence', 'coverage', 'valid'} for either scheme, or None."""
    text = (disc_id or "").strip().upper()
    m = ID_RE.match(text)
    if m:
        body = text.rsplit("_", 1)[0]
        return {"scheme": SCHEME, "set": m.group("set"), "sequence": int(m.group("seq")),
                "coverage": m.group("coverage"), "check": m.group("check"),
                "valid": check_char(body) == m.group("check")}
    m = LEGACY_RE.match(text)
    if m:
        return {"scheme": LEGACY_SCHEME, "set": m.group("set"), "sequence": int(m.group("seq")),
                "coverage": m.group("coverage"), "check": None, "valid": True}
    return None


def regenerate(disc):
    """Rebuild the id from a Disc record's fields (None for legacy or incomplete records)."""
    if disc.get("IdScheme") != SCHEME:
        return None
    try:
        return compose(disc.get("Set"), disc.get("Sequence"), disc.get("Coverage"))
    except (IdError, TypeError, ValueError):
        return None


def suggest(text, known_ids):
    """Known ids closest to a mistyped one (same length, fewest differing characters)."""
    text = text.strip().upper()
    scored = []
    for k in known_ids:
        if len(k) == len(text):
            diff = sum(1 for a, b in zip(k.upper(), text) if a != b)
            if diff <= 2:
                scored.append((diff, k))
    return [k for _, k in sorted(scored)]
