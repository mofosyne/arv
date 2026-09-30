"""The fixture file format: tab-separated values, one case per line.

Lines starting with '#' are comments; the first comment line names the columns.
Inside a value, backslash escapes stand for characters TSV cannot hold:
\\t tab, \\n newline, \\\\ backslash. An empty value is an empty string.
Nothing else is special, so any language reads these with a split on tabs.
"""

ESCAPES = {"\\": "\\\\", "\t": "\\t", "\n": "\\n"}


def escape(value):
    return "".join(ESCAPES.get(c, c) for c in str(value))


def unescape(text):
    out, i = [], 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            out.append({"\\": "\\", "t": "\t", "n": "\n"}.get(text[i + 1], text[i + 1]))
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def read(path):
    """[[value, ...], ...] for every case line."""
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line and not line.startswith("#"):
                rows.append([unescape(v) for v in line.split("\t")])
    return rows


def write(path, header, rows, comment=""):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# " + "\t".join(header) + "\n")
        for line in comment.strip().splitlines():
            f.write("# " + line + "\n")
        for row in rows:
            f.write("\t".join(escape(v) for v in row) + "\n")
