#!/usr/bin/env python3
"""(Re)write the expected outputs of the fixtures from the current implementation.

The fixture files are the contract a port (for example to C) is checked against,
so they change only deliberately. Run this after an intended change, then read
`git diff tests/fixtures` before committing:

    python3 tests/fixtures/generate.py          # rewrite
    python3 tests/fixtures/generate.py --check  # exit 1 if anything would change
"""

import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE)

import tsv  # noqa: E402
from archivetool import catalog, discid, names, recfile, sets  # noqa: E402

ERROR = "ERROR"


def attempt(fn, *args):
    try:
        return fn(*args)
    except (discid.IdError, ValueError):
        return ERROR


def build():
    out = {}

    payloads = ["PHOTOS-07_2015-2024", "TRIP-01_201907", "TAXES-03_2019", "A", "0", "ZZZZZZZZ-99_9999", "photos-07_2015-2024"]
    out["check-chars.tsv"] = (["payload", "check character"], [[p, discid.check_char(p)] for p in payloads],
                              "Luhn mod 36 over the letters and digits (case-insensitive; - and _ ignored).")

    compose_cases = [("PHOTOS", 7, "2015/2024"), ("photos", 7, "2015/2024"), ("TRIP", 1, "2019-07"),
                     ("TRIP", 1, "2019-07-14/2019-07-20"), ("TRIP", 1, "2019-12-24/2020-01-02"),
                     ("TAXES", 3, "2019"), ("BOOKS", 2, "199X"), ("MEDIA", 4, "1995~"),
                     ("CODE", 5, "[1998,1999]"), ("PROJ", 1, "2015-2024"), ("PROJECTS", 123, "2020/2025"),
                     ("VERYLONGSETNAME", 1, "2020"), ("X", 1, "2020"), ("OPEN", 1, "2019/.."),
                     ("BAD", 1, "sometime")]
    out["disc-id-compose.tsv"] = (["set", "sequence", "coverage", "id"],
                                  [[s, n, c, attempt(discid.compose, s, n, c)] for s, n, c in compose_cases],
                                  "Scheme set-seq-coverage/1: SET-SEQ_COVERAGE_CHECK. ERROR: the fields cannot make an id.")

    good = discid.compose("PHOTOS", 7, "2015/2024")
    body = good[:-2]
    typos = [good[:i] + ("0" if good[i] != "0" else "1") + good[i + 1:] for i in range(len(body)) if body[i] not in "-_"]
    swaps = [good[:i] + body[i + 1] + body[i] + good[i + 2:] for i in range(len(body) - 1)
             if body[i] not in "-_" and body[i + 1] not in "-_" and body[i] != body[i + 1]]
    parse_cases = [good, good.lower(), "  %s  " % good] + typos + swaps + [
        "2020-2025_PROJECTS_01", "2019_TAXES_3", "not an id", "", "PHOTOS-7_2015_A"]
    rows = []
    for text in parse_cases:
        p = discid.parse(text)
        if p is None:
            rows.append([text, "none", "", "", "", "", "no"])
        else:
            rows.append([text, p["scheme"], p["set"], p["sequence"], p["coverage"], p["check"] or "",
                         "yes" if p["valid"] else "no"])
    out["disc-id-parse.tsv"] = (["text", "scheme", "set", "sequence", "coverage", "check", "valid"], rows,
                                "scheme none: not an id of any scheme. Every single-character typo and neighbour\n"
                                "swap of the first id must be valid=no.")

    coverages = ["2019", "2019-07", "2019-07-14", "2019-02", "2020-02", "2015/2024", "2019-07/2019-08",
                 "2019-07-14/2019-07-20", "199X", "19XX", "2019-XX", "1995~", "2019?", "2019%", "[1998,1999]",
                 "{2001,2003}", "2019/..", "../2019", "2020-2025", "2019-13", "2019-02-30", "sometime", ""]
    rows = []
    for c in coverages:
        span = attempt(discid.coverage_dates, c)
        first, last = (ERROR, ERROR) if span == ERROR else (("", "") if span is None else
                                                             (span[0].isoformat(), span[1].isoformat()))
        rows.append([c, attempt(discid.to_edtf, c), attempt(discid.compact, c), first, last])
    out["coverage.tsv"] = (["input", "edtf", "compact (in ids)", "first day", "last day"], rows,
                           "EDTF subset. Empty days: unknown. 0001-01-01 / 9999-12-31: open-ended (..).")

    covers_cases = [("2015/2024", "2019"), ("2015/2024", "2030"), ("2019-07-14/2019-07-20", "2019-07-15"),
                    ("2019-07-14/2019-07-20", "2019-08"), ("2019-07", "2019"), ("199X", "1995-06"),
                    ("2019/..", "2030"), ("../2019", "1900"), ("2015-2024", "2016/2017"), ("2019", "2020")]
    out["covers.tsv"] = (["coverage", "query", "overlaps"],
                         [[c, q, "yes" if discid.covers(c, q) else "no"] for c, q in covers_cases], "")

    tags = ["travel", "  Travel  ", "Place : Kyoto", "person:Alice Smith", "event:wedding-2019",
            "a, b", "not a namespace: x y", "source:pixel-7"]
    out["tags.tsv"] = (["input", "normalised", "hierarchical (XMP)"],
                       [[t, catalog.normalise_tag(t), catalog.hierarchical(catalog.normalise_tag(t))] for t in tags],
                       "Folder tags: plain words or namespace:value.")

    rules = [("*.jpg", "trip/IMG_1.JPG"), ("*.jpg", "trip/IMG_1.jpeg"), ("*.git", "tool/.git/HEAD"),
             ("*/gerbers/*", "board/gerbers/top.gbr"), ("*/gerbers/*", "gerbers/top.gbr"),
             ("*/Taxes/*", "2019/taxes/return.pdf"), ("Makefile", "src/Makefile"), ("Makefile", "src/Makefile.am")]
    out["match-rules.tsv"] = (["pattern", "path", "matches"],
                              [[p, f, "yes" if sets.path_matches(p, f) else "no"] for p, f in rules],
                              "Match globs: without / against each path segment, with / against the whole path;\n"
                              "case ignored.")

    name_cases = ["plain.txt", "a" * 103, "a" * 104, "ü" * 254, "ü" * 255, "日" * 127, "日" * 128,
                  "a" * 127 + "日", "a" * 126 + "日", "semi;colon.txt", "star*?.txt", "photo \U0001F600 ok.txt",
                  "日本語の名前.txt"]
    rows = []
    for fs in ("hybrid", "udf250"):
        for n in name_cases:
            sev = {i[1] for i in names.check([n], fs)}
            rows.append([fs, n, "error" if "error" in sev else "warning" if sev else "ok"])
    out["names.tsv"] = (["filesystem", "file name", "result"], rows,
                        "ok: kept exactly everywhere. warning: Windows/macOS show another name (hybrid:\n"
                        "Joliet/UDF 1.02). error: the image cannot hold it (udf250: 254 characters, or 127\n"
                        "when any is beyond U+00FF; nothing beyond U+FFFF).")

    vocab = sets.load(None, os.path.join(HERE, "vocab.rec"))
    out["vocab-paths.tsv"] = (["code", "paths (space separated)"],
                              [[c, " ".join(vocab.paths(c))] for c in sorted(vocab.entries)] + [["NOPE", ""]],
                              "Every path from a top-level entry, for vocab.rec.")
    words = ["TRIP", "trip", "trips", "holiday", "Holidays", "vacation", "2019_Vacation", "pictures",
             "Projects", "PROJECTS", "Photos", "electronics", "zzz"]
    out["vocab-words.tsv"] = (["word", "resolve (typed)", "guess (folder name)"],
                              [[w, vocab.resolve(w) or "", vocab.guess(w) or ""] for w in words],
                              "resolve: a code beats an alias. guess: an alias beats a code (folder names).")

    for path in sorted(glob.glob(os.path.join(HERE, "recfile", "*.rec"))):
        rows = []
        for n, r in enumerate(recfile.read(path)):
            for name, value in r.fields:
                rows.append([n, r.type or "", name, value])
        out[os.path.join("recfile", os.path.basename(path)[:-4] + ".expected.tsv")] = (
            ["record", "type", "field", "value"], rows,
            "Every field of every record (descriptors included), in file order.")
    return out


def main():
    check = "--check" in sys.argv[1:]
    changed = []
    for name, (header, rows, comment) in build().items():
        path = os.path.join(HERE, name)
        tmp = path + ".new"
        tsv.write(tmp, header, rows, comment)
        with open(tmp, encoding="utf-8") as f:
            new = f.read()
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if new != old:
            changed.append(name)
        if check or new == old:
            os.remove(tmp)
        else:
            os.replace(tmp, path)
    for name in changed:
        print(("would change: " if check else "wrote: ") + name)
    return 1 if check and changed else 0


if __name__ == "__main__":
    sys.exit(main())
