"""Static index.html for a disc. No JavaScript, no network, works from file://.

It is a generated view: the recfiles and manifests on the disc are the source
of truth.
"""

import html
import urllib.parse

CSS = """
:root { --bg:#fdfdfc; --fg:#1d1d1b; --muted:#6b6b66; --line:#e2e1dc; --accent:#1f5fa8; --panel:#f4f3ef; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#161615; --fg:#e8e7e3; --muted:#9a9993; --line:#2e2d2a; --accent:#7fb0ea; --panel:#1f1f1d; }
}
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--fg);
       font:15px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }
main { max-width:960px; margin:0 auto; padding:24px 16px 64px; }
h1 { font-size:1.6rem; margin:0 0 4px; }
h2 { font-size:1.1rem; margin:32px 0 8px; border-bottom:1px solid var(--line); padding-bottom:4px; }
.id { color:var(--muted); font-family:ui-monospace, monospace; }
a { color:var(--accent); }
table { border-collapse:collapse; width:100%; }
th, td { text-align:left; padding:4px 8px; border-bottom:1px solid var(--line); vertical-align:top; }
th { color:var(--muted); font-weight:600; white-space:nowrap; width:1%; }
.note { background:var(--panel); padding:8px 12px; border-radius:6px; margin:6px 0; white-space:pre-wrap; }
.tree, .tree ul { list-style:none; margin:0; padding-left:18px; }
.tree { padding-left:0; font-family:ui-monospace, monospace; font-size:13px; }
.tree summary { cursor:pointer; }
.size { color:var(--muted); margin-left:8px; }
.scroll { overflow-x:auto; }
"""


def human_size(n):
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024 or unit == "TiB":
            return ("%d %s" % (n, unit)) if unit == "B" else ("%.1f %s" % (n, unit))
        n /= 1024


def _e(text):
    return html.escape(str(text))


def _tree(entries):
    root = {}
    for e in entries:
        node = root
        parts = e.path.split("/")
        for part in parts[:-1]:
            node = node.setdefault(part + "/", {})
        node[parts[-1]] = e
    return root


def _dir_size(node):
    total = 0
    for value in node.values():
        total += _dir_size(value) if isinstance(value, dict) else value.size
    return total


def _render_tree(node, out, depth=0):
    out.append('<ul class="tree">' if depth == 0 else "<ul>")
    for name in sorted(node, key=lambda k: (not k.endswith("/"), k.lower())):
        value = node[name]
        if isinstance(value, dict):
            out.append('<li><details%s><summary>%s<span class="size">%s</span></summary>'
                       % (" open" if depth == 0 else "", _e(name), human_size(_dir_size(value))))
            _render_tree(value, out, depth + 1)
            out.append("</details></li>")
        else:
            href = urllib.parse.quote("data/" + value.path)
            out.append('<li><a href="%s">%s</a><span class="size">%s</span></li>'
                       % (href, _e(name), human_size(value.size)))
    out.append("</ul>")


def _field_rows(disc, names):
    rows = []
    for name in names:
        values = disc.get_all(name)
        if values:
            rows.append("<tr><th>%s</th><td>%s</td></tr>" % (_e(name), "<br>".join(_e(v) for v in values)))
    return "\n".join(rows)


def render_index(disc, entries, snapshot_catalog):
    title = disc.get("Title", disc.get("Id"))
    out = [
        "<!doctype html>",
        '<html lang="en"><head><meta charset="utf-8">',
        '<meta name="viewport" content="width=device-width, initial-scale=1">',
        "<title>%s</title>" % _e(title),
        "<style>%s</style></head><body><main>" % CSS,
        "<h1>%s</h1>" % _e(title),
        '<div class="id">%s</div>' % _e(disc.get("Id")),
    ]
    for desc in disc.get_all("Description"):
        out.append('<p>%s</p>' % _e(desc))

    out.append('<p>To search this disc and the rest of the archive, see SEARCH in '
               '<a href="README.txt">README.txt</a>.</p>')
    out.append("<h2>About this disc</h2><table>")
    out.append(_field_rows(disc, ["Id", "Label", "Part", "Set", "Category", "Path", "Sequence", "Coverage", "Date", "Creator", "Subject",
                                  "Location", "Access", "Rights", "Media", "Filesystem", "Ecc", "Software"]))
    out.append("<tr><th>Contents</th><td>%s files, %s</td></tr>"
               % (_e(disc.get("Files")), human_size(int(disc.get("Bytes", "0")))))
    out.append("</table>")

    notes = disc.get_all("Note")
    if notes:
        out.append("<h2>Notes</h2>")
        out += ['<div class="note">%s</div>' % _e(n) for n in notes]

    out.append("<h2>Files</h2>")
    if entries:
        _render_tree(_tree(entries), out)
    else:
        out.append("<p>(empty)</p>")

    others = [d for d in snapshot_catalog.discs if d.get("Id") != disc.get("Id")] if snapshot_catalog else []
    if others:
        out.append("<h2>Other discs in this archive</h2>")
        out.append("<p>As of this disc's burn date. File lists: <code>catalog/volumes/</code>.</p>")
        out.append('<div class="scroll"><table><tr><th>Id</th><th>Title</th><th>Coverage</th>'
                   "<th>Location</th><th>Files</th></tr>")
        for d in others:
            out.append("<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>"
                       % (_e(d.get("Id", "")), _e(d.get("Title", "")), _e(d.get("Coverage", "")),
                          _e(snapshot_catalog.where(d)), _e(d.get("Files", ""))))
        out.append("</table></div>")

    out.append("<h2>Verify and recover</h2>")
    out.append('<p>See <a href="README.txt">README.txt</a>. Checksums: '
               '<a href="manifest-sha256.txt">manifest-sha256.txt</a>, '
               '<a href="manifest-sha512.txt">manifest-sha512.txt</a>. '
               'Catalogue: <a href="catalog.rec">catalog.rec</a>%s.</p>'
               % (', <a href="catalog/archive.rec">catalog/archive.rec</a>' if snapshot_catalog else ""))
    out.append("</main></body></html>\n")
    return "\n".join(out)
