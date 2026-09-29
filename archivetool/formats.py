"""File format identification with Siegfried (PRONOM IDs).

Optional: used when the `sf` command is available. Results go to
catalog/formats/<disc-id>.csv so that decades from now it is clear exactly
which format (and version) each file is, even if its extension is wrong.
"""

import csv
import io
import os
import shutil
import subprocess

COLUMNS = ("path", "puid", "format", "version", "mime", "basis", "warning")


class FormatsError(Exception):
    pass


def available():
    return shutil.which("sf") is not None


def identify(src, sf_home=None):
    """Run Siegfried over ``src``. Returns (header text, {relative path: row dict})."""
    base = ["sf"] + (["-home", sf_home] if sf_home else [])
    version = subprocess.run(base + ["-version"], capture_output=True, text=True).stdout
    proc = subprocess.run(base + ["-csv", "-multi", str(os.cpu_count() or 1), src],
                          capture_output=True, text=True)
    if not proc.stdout.startswith("filename,"):
        message = (proc.stderr or proc.stdout).strip()
        raise FormatsError(message.splitlines()[-1] if message else "sf produced no output")
    rows = {}
    for rec in csv.DictReader(io.StringIO(proc.stdout)):
        rel = os.path.relpath(rec["filename"], src).replace(os.sep, "/")
        rows[rel] = {
            "path": rel,
            "puid": rec.get("id", ""),
            "format": rec.get("format", ""),
            "version": rec.get("version", ""),
            "mime": rec.get("mime", ""),
            "basis": rec.get("basis", ""),
            "warning": rec.get("warning", ""),
        }
    header = "".join("# %s\n" % line for line in version.strip().splitlines())
    return header, rows


def write(path, header, rows, entries):
    """Write the formats file for one disc (only ``entries``, in their order)."""
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write("# PRONOM format identification (https://www.nationalarchives.gov.uk/PRONOM/)\n")
        f.write(header)
        writer = csv.DictWriter(f, fieldnames=COLUMNS, lineterminator="\n")
        writer.writeheader()
        for e in entries:
            writer.writerow(rows.get(e.path) or {"path": e.path, "puid": "UNKNOWN"})


def read(path):
    with open(path, encoding="utf-8", newline="") as f:
        lines = [line for line in f if not line.startswith("#")]
    return {row["path"]: row for row in csv.DictReader(lines)}


def pronom_url(puid):
    if not puid or puid == "UNKNOWN":
        return None
    return "https://www.nationalarchives.gov.uk/PRONOM/" + puid
