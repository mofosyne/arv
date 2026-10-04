"""The archivist log: Appraisal records (docs/smart-archive-format.md, "Appraisals").

An appraisal says how much something matters, to whom, and why, in words that read as English:

    Target: TRIP-01_2019_4:day1 Fushimi Inari/
    Importance: essential for self
    Importance: important for family
    Basis: the only photos of that day
    Authorship: human
    Agent: human:LOGIN
    Review: 2031-01-01

Appraisals are appended, never edited. For a target, the newest appraisal wins among those of
the highest standing (a person's, then software's rules, then a model's suggestion); a target
without one takes the nearest appraisal above it: file, folder, disc, then the disc's set.
"""

import datetime
import re

from . import catalog, recfile

LEVELS = ("essential", "important", "useful", "incidental")   # most important first
AUDIENCE_RE = re.compile(r"^[a-z0-9][a-z0-9:_-]*$")
IMPORTANCE_RE = re.compile(r"^\s*(\w+)\s+for\s+(.+?)\s*$", re.IGNORECASE)
# a person's judgement outranks rules, and rules outrank a model's unreviewed suggestion
STANDING = {"human": 2, "accepted": 2, "edited": 2, "automatic": 1, "suggested": 0}


class AppraisalError(ValueError):
    pass


def parse_importance(text):
    """'essential for family' -> ('essential', 'family')."""
    m = IMPORTANCE_RE.match(text or "")
    if not m:
        raise AppraisalError("importance must read '<level> for <audience>', e.g. 'essential for family' "
                             "(levels: %s), not %r" % (", ".join(LEVELS), text))
    level, audience = m.group(1).lower(), m.group(2).strip().lower()
    if level not in LEVELS:
        raise AppraisalError("unknown importance level %r (levels, most first: %s)" % (level, ", ".join(LEVELS)))
    if not AUDIENCE_RE.match(audience):
        raise AppraisalError("audience %r: one word of letters, digits, '-', '_' or ':' (e.g. self, family, "
                             "heirs, colleagues, public)" % audience)
    return level, audience


def review_date(text, today=None):
    """YYYY-MM-DD, or a span from today: 5y, 18m."""
    today = today or catalog.today_date()
    m = re.match(r"^(\d+)\s*([ym])$", (text or "").strip().lower())
    if m:
        months = int(m.group(1)) * (12 if m.group(2) == "y" else 1)
        y, mo = divmod(today.month - 1 + months, 12)
        day = min(today.day, 28)
        return datetime.date(today.year + y, mo + 1, day).isoformat()
    try:
        return datetime.date.fromisoformat(text.strip()).isoformat()
    except (AttributeError, ValueError):
        raise AppraisalError("review date: YYYY-MM-DD or a span such as 5y or 18m, not %r" % text)


def new_appraisal(target, importance, basis=None, review=None, agent=None, authorship="human", date=None):
    """importance: ['<level> for <audience>', ...]. Raises AppraisalError for anything invalid."""
    agents = [agent or catalog.person()] if isinstance(agent, (str, type(None))) else list(agent)
    if authorship not in catalog.AUTHORSHIP:
        raise AppraisalError("authorship must be one of: %s" % ", ".join(catalog.AUTHORSHIP))
    parsed = [parse_importance(i) for i in importance]
    audiences = [a for _, a in parsed]
    if len(set(audiences)) != len(audiences):
        raise AppraisalError("one importance per audience")
    if not parsed and not basis:
        raise AppraisalError("an appraisal needs an importance or a basis")
    if authorship in ("automatic", "suggested") and any(level == "incidental" for level, _ in parsed):
        # leaving something off a disc must be a person's decision, never a machine's
        raise AppraisalError("only a person can mark something incidental")
    r = recfile.Record("Appraisal", [("Target", target)])
    r.fields += [("Importance", "%s for %s" % p) for p in parsed]
    if basis:
        r.add("Basis", basis)
    r.fields += [("Date", date or catalog.today()), ("Authorship", authorship)]
    r.fields += [("Agent", a) for a in agents]
    if review:
        r.add("Review", review)
    return r


def importance(record):
    """{audience: level} of one appraisal (malformed lines are skipped: old or foreign records)."""
    out = {}
    for text in record.get_all("Importance"):
        try:
            level, audience = parse_importance(text)
        except AppraisalError:
            continue
        out[audience] = level
    return out


def overall(record):
    """The highest level across audiences: something essential to anyone must survive."""
    levels = importance(record).values()
    return min(levels, key=LEVELS.index) if levels else None


def current(cat, target):
    """The appraisal in force for exactly this target, or None."""
    found = [(STANDING.get(catalog.authorship(a), 0), a.get("Date") or "", i, a)
             for i, a in enumerate(cat.appraisals) if a.get("Target") == target]
    return max(found)[3] if found else None


def chain(cat, target):
    """The target and everything above it, most specific first: file, folders, disc, set."""
    if target.startswith(("set:", "collection:")):
        return [target]
    disc_id, path, _ = catalog.parse_item(target)
    out = []
    if path:
        parts = path.rstrip("/").split("/")
        if not path.endswith("/"):
            out.append("%s:%s" % (disc_id, path))
            parts = parts[:-1]
        for i in range(len(parts), 0, -1):
            out.append("%s:%s/" % (disc_id, "/".join(parts[:i])))
    out.append(disc_id)
    disc = cat.disc(disc_id)
    if disc is not None and disc.get("Set"):
        out.append("set:" + disc.get("Set"))
    return out


def effective(cat, target):
    """(appraisal, the target it was made for) in force for target, inherited if need be; (None, None)."""
    for t in chain(cat, target):
        a = current(cat, t)
        if a is not None:
            return a, t
    return None, None


def due(cat, on=None):
    """Appraisals in force whose Review date has come, oldest first."""
    on = on or catalog.today()
    targets = []
    for a in cat.appraisals:
        if a.get("Target") not in targets:
            targets.append(a.get("Target"))
    out = [current(cat, t) for t in targets]
    return sorted((a for a in out if a.get("Review") and a.get("Review") <= on), key=lambda a: a.get("Review"))
