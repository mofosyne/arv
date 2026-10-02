"""Minimal reader/writer for GNU recutils recfiles (standard library only).

Supports what the archive catalogue uses: record descriptors (%rec, %key, ...),
``Name: value`` fields, ``+`` continuation lines and ``#`` comments. It does not
validate against descriptors; run ``recfix`` for that when recutils is installed.
"""

import re

FIELD_RE = re.compile(r"^([%a-zA-Z][a-zA-Z0-9_]*):[ \t]?(.*)$")


class Record:
    """An ordered list of (name, value) fields; names may repeat."""

    def __init__(self, type_=None, fields=None):
        self.type = type_
        self.fields = list(fields or [])

    def get(self, name, default=None):
        for key, value in self.fields:
            if key == name:
                return value
        return default

    def get_all(self, name):
        return [value for key, value in self.fields if key == name]

    def set(self, name, value):
        """Replace the first occurrence of ``name`` (dropping the rest), or append it."""
        out, done = [], False
        for key, old in self.fields:
            if key == name:
                if not done:
                    out.append((key, value))
                    done = True
            else:
                out.append((key, old))
        if not done:
            out.append((name, value))
        self.fields = out

    def add(self, name, value):
        self.fields.append((name, value))

    @property
    def is_descriptor(self):
        return any(key.startswith("%") for key, _ in self.fields)

    def __repr__(self):
        return "Record(%r, %r)" % (self.type, self.fields)


def parse(text):
    """Parse recfile text into a list of Records (descriptors included, in file order)."""
    text = text.replace("\r\n", "\n").replace("\r", "\n")  # any line endings
    records = []
    current_type = None
    fields = []

    def flush():
        nonlocal fields, current_type
        if not fields:
            return
        record = Record(current_type, fields)
        if record.is_descriptor:
            current_type = record.get("%rec")
            record.type = current_type
        records.append(record)
        fields = []

    lines = text.split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        i += 1
        if line.startswith("#"):
            continue
        if not line.strip():
            flush()
            continue
        if line.startswith("+") and fields:
            cont = line[1:]
            if cont.startswith(" "):
                cont = cont[1:]
            name, value = fields[-1]
            fields[-1] = (name, value + "\n" + cont)
            continue
        # A trailing backslash joins the next physical line
        while line.endswith("\\") and i < len(lines):
            line = line[:-1] + lines[i]
            i += 1
        match = FIELD_RE.match(line)
        if not match:
            raise ValueError("recfile: cannot parse line %d: %r" % (i, line))
        fields.append((match.group(1), match.group(2)))
    flush()
    return records


def format_field(name, value):
    value = str(value)
    first, *rest = value.split("\n")
    lines = ["%s: %s" % (name, first) if first else "%s:" % name]
    lines += ["+ " + part if part else "+" for part in rest]
    return "\n".join(lines)


def format_record(record):
    return "\n".join(format_field(name, value) for name, value in record.fields)


def dumps(records):
    return "\n\n".join(format_record(r) for r in records) + "\n"


def read(path):
    with open(path, encoding="utf-8") as f:
        return parse(f.read())


def write(path, records):
    with open(path, "w", encoding="utf-8") as f:
        f.write(dumps(records))
