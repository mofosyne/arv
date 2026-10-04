"""Check the implementation against the language-neutral fixtures in tests/fixtures/.

The fixtures are plain TSV (see tests/fixtures/README.md), so a port in another
language can run the same cases. This runner reads only the files.
"""

import glob
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
FIX = os.path.join(HERE, "fixtures")
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "src"))
sys.path.insert(0, FIX)

import tsv  # noqa: E402
from arv import catalog, discid, image, names, recfile, sets  # noqa: E402

ERROR = "ERROR"


def attempt(fn, *args):
    try:
        return fn(*args)
    except (discid.IdError, ValueError):
        return ERROR


def cases(name):
    return tsv.read(os.path.join(FIX, name))


class FixtureTest(unittest.TestCase):
    def test_check_chars(self):
        for payload, expected in cases("check-chars.tsv"):
            self.assertEqual(discid.check_char(payload), expected, payload)

    def test_disc_id_compose(self):
        for set_name, seq, coverage, expected in cases("disc-id-compose.tsv"):
            self.assertEqual(attempt(discid.compose, set_name, int(seq), coverage), expected, (set_name, coverage))

    def test_disc_id_parse(self):
        for text, scheme, set_name, seq, coverage, check, valid in cases("disc-id-parse.tsv"):
            p = discid.parse(text)
            if scheme == "none":
                self.assertIsNone(p, text)
                continue
            got = [p["scheme"], p["set"], str(p["sequence"]), p["coverage"], p["check"] or "",
                   "yes" if p["valid"] else "no"]
            self.assertEqual(got, [scheme, set_name, seq, coverage, check, valid], text)

    def test_coverage(self):
        for text, edtf, compact, first, last in cases("coverage.tsv"):
            self.assertEqual(attempt(discid.to_edtf, text), edtf, text)
            self.assertEqual(attempt(discid.compact, text), compact, text)
            span = attempt(discid.coverage_dates, text)
            got = (ERROR, ERROR) if span == ERROR else ("", "") if span is None else \
                (span[0].isoformat(), span[1].isoformat())
            self.assertEqual(got, (first, last), text)

    def test_covers(self):
        for coverage, query, expected in cases("covers.tsv"):
            self.assertEqual("yes" if discid.covers(coverage, query) else "no", expected, (coverage, query))

    def test_tags(self):
        for text, normalised, hierarchical in cases("tags.tsv"):
            self.assertEqual(catalog.normalise_tag(text), normalised, text)
            self.assertEqual(catalog.hierarchical(normalised), hierarchical, text)

    def test_match_rules(self):
        for pattern, path, expected in cases("match-rules.tsv"):
            self.assertEqual("yes" if sets.path_matches(pattern, path) else "no", expected, (pattern, path))

    def test_names(self):
        for name, expected in cases("names.tsv"):
            sev = {i[1] for i in names.check([name])}
            got = "error" if "error" in sev else "warning" if sev else "ok"
            self.assertEqual(got, expected, name)

    def test_labels(self):
        for disc_id, text, expected in cases("labels.tsv"):
            self.assertEqual(image.volume_label(disc_id, text), expected, (disc_id, text))
            self.assertEqual(image.disc_id_from_label(expected), disc_id)

    def test_vocabulary(self):
        vocab = sets.load(None, os.path.join(FIX, "vocab.rec"))
        for code, paths in cases("vocab-paths.tsv"):
            self.assertEqual(" ".join(vocab.paths(code)), paths, code)
        for word, resolved, guessed in cases("vocab-words.tsv"):
            self.assertEqual(vocab.resolve(word) or "", resolved, word)
            self.assertEqual(vocab.guess(word) or "", guessed, word)

    def test_recfiles(self):
        for path in sorted(glob.glob(os.path.join(FIX, "recfile", "*.rec"))):
            got = [[str(n), r.type or "", k, v] for n, r in enumerate(recfile.read(path)) for k, v in r.fields]
            self.assertEqual(got, cases(os.path.join("recfile", os.path.basename(path)[:-4] + ".expected.tsv")),
                             path)

    def test_fixtures_are_current(self):
        """The generator would write exactly these files (catches cases added without expectations)."""
        proc = subprocess.run([sys.executable, os.path.join(FIX, "generate.py"), "--check"],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)


if __name__ == "__main__":
    unittest.main()
