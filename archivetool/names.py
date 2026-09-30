"""File names that a disc's filesystem cannot keep exactly, found before an image is built.

The payload keeps every name exactly in the BagIt manifests either way; this is
about what people see when they open the disc on different systems.

hybrid (ISO 9660 + Rock Ridge + Joliet + UDF 1.02, genisoimage):
  Linux reads Rock Ridge, which keeps names exactly (up to 255 bytes).
  Windows and macOS read Joliet or UDF, where genisoimage
  - truncates each name to 103 characters,
  - replaces * : ; ? \\ with _,
  - cuts a name at the first character beyond U+FFFF (emoji and the like).
  These are warnings: the files are all there, some names differ.

udf250 (UDF 2.50, udfmake):
  a name is stored as one compression byte plus 1 byte per character when every
  character is at most U+00FF, else 2 bytes per character, in at most 255 bytes:
  at most 254 such characters, or 127 otherwise. Characters beyond U+FFFF cannot
  be stored. Beyond these limits udfmake writes a wrong name or a corrupt image,
  so these are errors.

Both: names with characters Windows forbids (< > : " \\ | ? *) are shown
changed on Windows (UDF readers apply the spec's renaming for them), and names
that differ only in letter case clash on Windows, which then shows only one.
"""

import collections

JOLIET_MAX = 103
JOLIET_REPLACED = set('*:;?\\')
WINDOWS_FORBIDDEN = set('<>:"\\|?*')


def _udf_bytes(name):
    wide = any(ord(c) > 0xFF for c in name)
    return 1 + len(name) * (2 if wide else 1)


def check(paths, filesystem):
    """[(path, severity, problem)] for payload paths (relative, '/'-separated).

    severity is 'error' (the image would be wrong) or 'warning' (some systems show another name).
    """
    issues = []
    seen = set()
    for path in paths:
        parts = path.split("/")
        for depth, name in enumerate(parts):
            key = "/".join(parts[:depth + 1])
            if key in seen:
                continue
            seen.add(key)
            beyond = [c for c in name if ord(c) > 0xFFFF]
            if filesystem == "udf250":
                if beyond:
                    issues.append((key, "error", "has characters beyond U+FFFF (%s), which UDF cannot store"
                                   % "".join(beyond[:3])))
                elif _udf_bytes(name) > 255:
                    wide = any(ord(c) > 0xFF for c in name)
                    issues.append((key, "error", "%d characters; UDF holds at most %s"
                                   % (len(name), "127 when a name has characters beyond U+00FF" if wide
                                      else "254")))
                bad = sorted(set(name) & WINDOWS_FORBIDDEN)
                if bad:
                    issues.append((key, "warning", "%s not allowed in Windows names: shown changed there"
                                   % " ".join(bad)))
            else:
                if beyond:
                    issues.append((key, "warning", "cut at %s on Windows/macOS (Joliet/UDF end the name there)"
                                   % beyond[0]))
                if len(name) > JOLIET_MAX:
                    issues.append((key, "warning", "%d characters: shortened to %d on Windows/macOS"
                                   % (len(name), JOLIET_MAX)))
                bad = sorted(set(name) & (JOLIET_REPLACED | WINDOWS_FORBIDDEN))
                if bad:
                    issues.append((key, "warning", "%s shown as _ or changed on Windows/macOS" % " ".join(bad)))
    # names that differ only in case, per folder
    folders = collections.defaultdict(dict)
    for key in sorted(seen):
        parent, _, name = key.rpartition("/")
        other = folders[parent].setdefault(name.casefold(), name)
        if other != name:
            issues.append((key, "warning", "differs from %r only in letter case: Windows shows only one"
                           % other))
    return issues


def report(issues, limit=10):
    """Lines for the terminal: the first few issues of each kind and a count."""
    lines = []
    by_severity = collections.OrderedDict((s, [i for i in issues if i[1] == s]) for s in ("error", "warning"))
    for severity, items in by_severity.items():
        for path, _, problem in items[:limit]:
            shown = path if len(path) <= 70 else path[:40] + "…" + path[-25:]
            lines.append("  %s: %s: %s" % (severity, shown, problem))
        if len(items) > limit:
            lines.append("  ... and %d more %ss" % (len(items) - limit, severity))
    return lines
