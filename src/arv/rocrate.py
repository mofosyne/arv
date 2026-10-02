"""Optional RO-Crate 1.2 metadata for a disc's payload (``--ro-crate``).

Follows the RO-Crate 1.2 BagIt notes: the crate files live in the payload
directory (data/ro-crate-metadata.json, data/ro-crate-preview.html) and are
listed in the bag manifests like any payload file. The source folder is not
modified; the two files are grafted into the image.
"""

import datetime
import json
import re
from urllib.parse import quote

from . import formats as formats_mod
from .html import _e, human_size

METADATA = "ro-crate-metadata.json"
PREVIEW = "ro-crate-preview.html"
SPEC = "https://w3id.org/ro/crate/1.2"
CONTEXT = "https://w3id.org/ro/crate/1.2/context"
FILE_LIMIT = 100000  # above this, files are summarised rather than listed one by one


def temporal(coverage):
    """'2020-2025' -> '2020/2025' (ISO 8601 interval); '2023' stays."""
    m = re.fullmatch(r"(\d{4})-(\d{4})", coverage or "")
    return "%s/%s" % m.groups() if m else coverage


def build(disc, entries, formats=None):
    """Return the ro-crate-metadata.json document for one disc."""
    formats = formats or {}
    root = {
        "@id": "./",
        "@type": "Dataset",
        "name": disc.get("Title"),
        "identifier": {"@id": "#disc-id"},
        "datePublished": disc.get("Date"),
        "description": disc.get("Description") or "Archival disc %s" % disc.get("Id"),
    }
    graph = [
        {"@id": METADATA, "@type": "CreativeWork", "conformsTo": {"@id": SPEC}, "about": {"@id": "./"}},
        root,
        {"@id": "#disc-id", "@type": "PropertyValue", "name": "Disc id", "propertyID": "archive disc id",
         "value": disc.get("Id")},
    ]
    if disc.get("Coverage"):
        root["temporalCoverage"] = temporal(disc.get("Coverage"))
    subjects = disc.get_all("Subject")
    if subjects:
        root["keywords"] = subjects[0] if len(subjects) == 1 else subjects
    if disc.get("Creator"):
        root["creator"] = root["publisher"] = {"@id": "#creator"}
        graph.append({"@id": "#creator", "@type": "Person", "name": disc.get("Creator")})
    rights = disc.get("Rights")
    if rights:
        if re.match(r"https?://", rights):
            root["license"] = {"@id": rights}
            graph.append({"@id": rights, "@type": "CreativeWork", "name": rights,
                          "description": "Licence for the contents of this disc"})
        else:
            root["copyrightNotice"] = rights
    if disc.get("Part"):
        root["isPartOf"] = {"@id": "#set"}
        graph.append({"@id": "#set", "@type": "CreativeWork", "name": "%s (%s)" % (disc.get("Set"), disc.get("Part"))})
    root["contentSize"] = str(sum(e.size for e in entries))

    if len(entries) > FILE_LIMIT:
        root["description"] += (" (%d files; per-file details are in the disc's catalog/listings, "
                                 "catalog/formats and manifest files)" % len(entries))
        return {"@context": CONTEXT, "@graph": graph}

    parts, pronoms = [], {}
    for e in entries:
        entity = {"@id": _id(e.path), "@type": "File", "name": e.path.rsplit("/", 1)[-1],
                  "contentSize": str(e.size)}
        entity["dateModified"] = datetime.datetime.fromtimestamp(e.mtime, datetime.timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ")
        fmt = formats.get(e.path)
        if fmt:
            enc = []
            if fmt.get("mime"):
                enc.append(fmt["mime"])
            url = formats_mod.pronom_url(fmt.get("puid"))
            if url:
                enc.append({"@id": url})
                pronoms.setdefault(url, fmt)
            if enc:
                entity["encodingFormat"] = enc if len(enc) > 1 else enc[0]
        parts.append({"@id": entity["@id"]})
        graph.append(entity)
    root["hasPart"] = parts
    for url, fmt in pronoms.items():
        name = fmt.get("format") or fmt.get("puid")
        if fmt.get("version"):
            name += " " + fmt["version"]
        graph.append({"@id": url, "@type": "WebPage", "name": name})
    return {"@context": CONTEXT, "@graph": graph}


def _id(path):
    """RO-Crate data entity ids are URI paths relative to the crate root."""
    return quote(path)


def dumps(doc):
    return json.dumps(doc, ensure_ascii=False, indent=1) + "\n"


def preview(disc, entries):
    """Small human-readable page RO-Crate tools show; the full viewer is ../index.html."""
    return (
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>%s</title></head><body>"
        "<h1>%s</h1><p>%s</p><p>Disc %s &middot; %d files, %s</p>"
        "<p>See <a href=\"../index.html\">index.html</a> on the disc for the full file list and "
        "<a href=\"ro-crate-metadata.json\">ro-crate-metadata.json</a> for the machine-readable description.</p>"
        "</body></html>\n"
        % (_e(disc.get("Title")), _e(disc.get("Title")), _e(disc.get("Description") or ""),
           _e(disc.get("Id")), len(entries), human_size(sum(e.size for e in entries)))
    )
