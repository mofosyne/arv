"""search.html plus its data files: search this disc and every earlier disc in a browser.

Works from file:// with no network: data is loaded with <script src> (fetch()
of local files is blocked by browsers). Per-disc file lists load on demand.

    search.html
    catalog/web/discs.js           ARCHIVE_THIS, ARCHIVE_DISCS
    catalog/web/files/<id>.js      ARCHIVE_FILES["<id>"] = "size\\tpath\\n..."
"""

import json
import os

from .html import CSS

DISC_FIELDS = ("Id", "Part", "Title", "Set", "Coverage", "Date", "Location", "Description", "Subject", "Note", "Files", "Copies")


def _js(value):
    """JSON that is safe inside a <script> element."""
    return (json.dumps(value, ensure_ascii=False)
            .replace("</", "<\\/").replace("\u2028", "\\u2028").replace("\u2029", "\\u2029"))


def disc_summary(disc):
    out = {}
    for name in DISC_FIELDS:
        values = disc.get_all(name)
        if values:
            out[name] = values if name in ("Subject", "Note") else values[0]
    return out


def write_web_data(web_dir, this_id, discs, listing_paths):
    """discs: Disc records; listing_paths: {disc_id: path to a listing .tsv}."""
    os.makedirs(os.path.join(web_dir, "files"), exist_ok=True)
    with open(os.path.join(web_dir, "discs.js"), "w", encoding="utf-8") as f:
        f.write("var ARCHIVE_THIS = %s;\nvar ARCHIVE_DISCS = %s;\n"
                % (_js(this_id), _js([disc_summary(d) for d in discs])))
    for disc_id, path in listing_paths.items():
        rows = []
        for size, _mtime, rel in read_listing(path):
            rows.append("%s\t%s" % (size, rel))
        with open(os.path.join(web_dir, "files", disc_id + ".js"), "w", encoding="utf-8") as f:
            f.write("window.ARCHIVE_FILES = window.ARCHIVE_FILES || {};\n"
                    "window.ARCHIVE_FILES[%s] = %s;\n" % (_js(disc_id), _js("\n".join(rows))))


def write_listing(path, entries):
    """Plain-text listing with sizes and dates (the manifests only have checksums)."""
    import datetime
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# size\tmodified (UTC)\tpath (relative to data/)\n")
        for e in entries:
            mtime = datetime.datetime.fromtimestamp(e.mtime, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            f.write("%d\t%s\t%s\n" % (e.size, mtime, e.path))


def read_listing(path):
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            size, mtime, rel = line.split("\t", 2)
            yield size, mtime, rel


SEARCH_HTML = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Search archive</title>
<style>%(css)s
form { display:flex; flex-wrap:wrap; gap:8px 16px; align-items:center; margin:16px 0; }
input[type=search] { flex:1 1 280px; font:inherit; padding:8px 10px; border:1px solid var(--line);
  border-radius:6px; background:var(--panel); color:var(--fg); }
button { font:inherit; padding:8px 16px; border-radius:6px; border:1px solid var(--line);
  background:var(--accent); color:var(--bg); cursor:pointer; }
.hit { padding:6px 0; border-bottom:1px solid var(--line); overflow-wrap:anywhere; }
.hit .path { font-family:ui-monospace, monospace; font-size:13px; }
.where { color:var(--muted); font-size:13px; }
#status { color:var(--muted); }
</style></head><body><main>
<h1>Search</h1>
<div class="id">This disc: <span id="this"></span> &middot; <a href="index.html">Browse this disc</a></div>
<noscript><p>Search needs JavaScript. <a href="index.html">index.html</a> lists this disc without it, and
<code>catalog/listings/</code> has plain-text file lists of every disc in the catalogue.</p></noscript>
<form id="form">
  <input id="q" type="search" placeholder="Words in a file name or path, or a glob like *.jpg" autofocus>
  <label><input type="radio" name="scope" value="this" checked> This disc</label>
  <label><input type="radio" name="scope" value="all"> All discs</label>
  <button type="submit">Search</button>
</form>
<p id="status"></p>
<div id="disc-hits"></div>
<div id="file-hits"></div>
<script src="catalog/web/discs.js"></script>
<script>
(function () {
  "use strict";
  var LIMIT = 500;
  var discs = {}, lists = {};
  ARCHIVE_DISCS.forEach(function (d) { discs[d.Id] = d; });
  document.getElementById("this").textContent = ARCHIVE_THIS;

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  }

  function load(id, done) {
    if (id in lists) return done();
    var s = document.createElement("script");
    s.src = "catalog/web/files/" + encodeURIComponent(id) + ".js";
    s.onload = function () {
      var raw = (window.ARCHIVE_FILES || {})[id] || "";
      lists[id] = raw ? raw.split("\\n") : [];
      delete window.ARCHIVE_FILES[id];
      done();
    };
    s.onerror = function () { lists[id] = null; done(); };
    document.head.appendChild(s);
  }

  function loadAll(ids, done) {
    var i = 0;
    (function next() {
      if (i >= ids.length) return done();
      setStatus("Loading file lists " + (i + 1) + " / " + ids.length + " ...");
      load(ids[i++], function () { setTimeout(next, 0); });
    })();
  }

  function globToRegExp(glob) {
    var re = "";
    for (var i = 0; i < glob.length; i++) {
      var c = glob.charAt(i);
      if (c === "*") re += ".*";
      else if (c === "?") re += ".";
      else re += c.replace(/[.+^${}()|[\\]\\\\]/g, "\\\\$&");
    }
    return new RegExp("^" + re + "$", "i");
  }

  function matcher(query) {
    var tests = query.trim().split(/\\s+/).filter(Boolean).map(function (term) {
      if (/[*?]/.test(term)) { var re = globToRegExp(term); return function (s) { return re.test(s); }; }
      var t = term.toLowerCase();
      return function (s) { return s.toLowerCase().indexOf(t) !== -1; };
    });
    return function (s) { return tests.length && tests.every(function (t) { return t(s); }); };
  }

  function setStatus(text) { document.getElementById("status").textContent = text; }

  function humanSize(n) {
    n = Number(n);
    var units = ["B", "KiB", "MiB", "GiB", "TiB"], u = 0;
    while (n >= 1024 && u < units.length - 1) { n /= 1024; u++; }
    return u ? n.toFixed(1) + " " + units[u] : n + " B";
  }

  function dataHref(path) {
    return "data/" + path.split("/").map(encodeURIComponent).join("/");
  }

  function describe(d) {
    return d.Id + " \\u2014 " + (d.Title || "") + " \\u2014 " + (d.Location ? "stored at " + d.Location : "location not recorded");
  }

  function search(query, ids) {
    var match = matcher(query);
    var discBox = document.getElementById("disc-hits"), fileBox = document.getElementById("file-hits");
    discBox.textContent = ""; fileBox.textContent = "";

    var discHits = ids.filter(function (id) {
      var d = discs[id];
      return ["Id", "Title", "Description", "Coverage", "Subject", "Note"].some(function (k) {
        return [].concat(d[k] || []).some(match);
      });
    });
    if (discHits.length) {
      discBox.appendChild(el("h2", null, "Discs"));
      discHits.forEach(function (id) { discBox.appendChild(el("div", "hit", describe(discs[id]))); });
    }

    var total = 0, shown = 0, missing = [];
    var heading = el("h2", null, "Files");
    fileBox.appendChild(heading);
    ids.forEach(function (id) {
      var rows = lists[id];
      if (rows === null) { missing.push(id); return; }
      rows.forEach(function (row) {
        var tab = row.indexOf("\\t"), path = row.slice(tab + 1);
        if (!match(path)) return;
        total++;
        if (shown >= LIMIT) return;
        shown++;
        var hit = el("div", "hit");
        if (id === ARCHIVE_THIS) {
          var a = el("a", "path", path); a.href = dataHref(path); hit.appendChild(a);
          hit.appendChild(el("span", "size", humanSize(row.slice(0, tab))));
          hit.appendChild(el("div", "where", "On this disc"));
        } else {
          hit.appendChild(el("span", "path", path));
          hit.appendChild(el("span", "size", humanSize(row.slice(0, tab))));
          hit.appendChild(el("div", "where", "On disc " + describe(discs[id])));
        }
        fileBox.appendChild(hit);
      });
    });
    heading.textContent = "Files (" + total + (total > shown ? ", showing first " + shown : "") + ")";
    setStatus(total || discHits.length ? "" : "No matches." +
      (missing.length ? " File lists missing for: " + missing.join(", ") : ""));
  }

  document.getElementById("form").addEventListener("submit", function (ev) {
    ev.preventDefault();
    var query = document.getElementById("q").value;
    var all = document.querySelector("input[name=scope]:checked").value === "all";
    var ids = all ? ARCHIVE_DISCS.map(function (d) { return d.Id; }) : [ARCHIVE_THIS];
    loadAll(ids, function () { setStatus(""); search(query, ids); });
  });
})();
</script>
</main></body></html>
"""


def render_search():
    return SEARCH_HTML % {"css": CSS}
