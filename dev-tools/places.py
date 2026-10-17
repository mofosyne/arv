#!/usr/bin/env python3
"""Writes arv's gazetteer and world outline from Natural Earth (public domain, naturalearthdata.com):

  src/arv/data/default_places.rec   countries and populated places, as known names of the place
                                    namespace (research/plan.md, "Known names, as authority control")
  src/arv/data/world.txt            the land as one SVG path, for arv map --svg and discs' index.html
  src/arv-assist/data/countries.txt the countries' outlines, for which country a photo's GPS is in

  python3 dev-tools/places.py [DIR]     (DIR: the Natural Earth GeoJSON files, else fetched with curl)

Keys are made once, from the English name in ASCII: place:kyoto. A name two places share is told
apart by the country's ISO code (place:victoria-ca), then by the region (place:portland-us-oregon),
and each gets a Qualifier; a form that names two entries is always asked about, never taken.
Positions are to two decimals (about a kilometre): enough to say which city, no more.
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import unicodedata

BASE = "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/"
FILES = ["ne_50m_admin_0_countries", "ne_50m_populated_places_simple", "ne_110m_land", "ne_110m_admin_0_countries"]
HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "src", "arv", "data")

# common short forms that Natural Earth leaves out
EXTRA_ALIASES = {
    "united-kingdom": ["UK", "Britain", "Great Britain"],
    "united-states-of-america": ["USA", "United States", "America"],
}


def ascii_of(s):
    return unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()


def slug(s):
    return re.sub(r"[^a-z0-9]+", "-", ascii_of(s).lower()).strip("-")


def at(lat, lon):
    """ISO 6709, decimal degrees: +35.03+135.75/"""
    return "%+06.2f%+07.2f/" % (lat, lon)


def load(src):
    out = {}
    for f in FILES:
        p = os.path.join(src, f + ".geojson")
        if not os.path.exists(p):
            subprocess.run(["curl", "-sSf", "--retry", "5", "--retry-all-errors", "-o", p, BASE + f + ".geojson"], check=True)
        with open(p, encoding="utf-8") as fp:
            out[f] = json.load(fp)["features"]
    return out


def gazetteer(nef):
    countries, by_a3, records = [], {}, []
    for x in nef["ne_50m_admin_0_countries"]:
        p = x["properties"]
        label = p["NAME_EN"] or p["NAME"]
        iso = p["ISO_A2_EH"] if p["ISO_A2_EH"] not in (None, "-99") else p["ISO_A2"]
        countries.append({"key": slug(label), "label": label, "iso": iso if iso != "-99" else None,
                          "aliases": [p["NAME"], p["NAME_LONG"], p["NAME_ALT"]], "lat": p["LABEL_Y"], "lon": p["LABEL_X"],
                          "same": p["WIKIDATAID"]})
        by_a3[p["ADM0_A3"]] = countries[-1]
        by_a3.setdefault(p["SOV_A3"], countries[-1])
    taken = {c["key"] for c in countries}
    cities = []
    for x in nef["ne_50m_populated_places_simple"]:
        p = x["properties"]
        country = by_a3.get(p["adm0_a3"]) or by_a3.get(p["sov_a3"])
        aliases = [p["nameascii"]] + (p["namealt"] or "").split("|")
        cities.append({"label": p["name"], "base": slug(p["nameascii"] or p["name"]), "aliases": aliases,
                       "country": country, "region": (p["adm1name"] or "").strip(), "iso": (p["iso_a2"] or "").lower(),
                       "lat": p["latitude"], "lon": p["longitude"]})
    count = {}
    for c in cities:
        count[c["base"]] = count.get(c["base"], 0) + 1
    for c in cities:                    # a shared name: the country, then the region, tell them apart
        c["key"], c["qualifier"] = c["base"], None
        if count[c["base"]] > 1 or c["base"] in taken:
            c["key"] = "%s-%s" % (c["base"], c["iso"] or "xx")
            where = c["country"]["label"] if c["country"] else None
            c["qualifier"] = ", ".join(v for v in (c["region"], where) if v)
    again = {}
    for c in cities:
        again[c["key"]] = again.get(c["key"], 0) + 1
    for c in cities:
        if again[c["key"]] > 1:
            c["key"] = "%s-%s" % (c["key"], slug(c["region"] or "x"))
    keys = [c["key"] for c in cities] + [c["key"] for c in countries]
    assert len(keys) == len(set(keys)), "keys must be unique"

    def aliases_of(label, more):
        out = []
        for a in more + [ascii_of(label)]:
            a = re.sub(r"\s+", " ", a or "").strip()
            if a and a != label and a not in out:
                out.append(a)
        return out

    for c in sorted(countries, key=lambda c: c["key"]):
        r = ["Name: place:" + c["key"], "Label: " + c["label"]]
        r += ["Alias: " + a for a in aliases_of(c["label"], c["aliases"] + EXTRA_ALIASES.get(c["key"], []))]
        r += ["Kind: country"]
        if c["iso"]:
            r.append("Country: " + c["iso"])
        r.append("At: " + at(c["lat"], c["lon"]))
        if c["same"]:
            r.append("Same: wikidata:" + c["same"])
        records.append(r)
    for c in sorted(cities, key=lambda c: c["key"]):
        r = ["Name: place:" + c["key"], "Label: " + c["label"]]
        r += ["Alias: " + a for a in aliases_of(c["label"], c["aliases"])]
        if c["qualifier"]:
            r.append("Qualifier: " + c["qualifier"])
        r.append("Kind: city")
        if c["country"]:
            r.append("Parent: place:" + c["country"]["key"])
        if c["region"]:
            r.append("Region: " + c["region"])
        r.append("At: " + at(c["lat"], c["lon"]))
        records.append(r)
    return records, len(countries), len(cities), by_a3


HEADER = """\
# The gazetteer: places, as known names of the place namespace (arv tags show place:kyoto).
#
# Copied to <home>/config/places.rec on first use; edit that copy freely, or change entries with
# arv tags add|label|alias|merge|split|retire place:KEY (each an event). Made by
# dev-tools/places.py from Natural Earth (public domain, naturalearthdata.com): %d countries and
# %d populated places. Add your own (a village, a house, a site) with
#   arv tags add place:fushimi-inari --label "Fushimi Inari" --parent place:kyoto [--at +34.97+135.77/]
# A place added with a position or an extent is private (it stays home) unless --public says otherwise.
#
# The fields of a known name (see tags.rec), and:
#   Kind      country, region, city or site
#   Parent    the place it is in (broader): fushimi-inari, kyoto, japan
#   Country   ISO 3166-1 alpha-2, for a country
#   At        where, ISO 6709 in decimal degrees (WGS84): +35.03+135.75/ (a city's to about a km)
#   Extent    the box it covers, two opposite corners: +34.96+135.76/+34.98+135.78/ (a park, a farm)
#   Reach     how near a photo must be to At to be here: 2km, 500m (your places: 3km unless it says)
#   Region    the region it is in, by name or ISO 3166-2 code, as its source writes it (repeatable):
#             with iso-codes installed, arv puts the place in that region (Uji in Kyoto Prefecture)
#
# arv describe --ask offers a place found in a folder's names or text, and the nearest one to its
# photos' GPS (the record keeps the place, never the coordinates); arv map shows them.

%%rec: Place
%%key: Name
%%mandatory: Name
%%allowed: Name Label Alias Qualifier Related Same Basis Private Retired Kind Parent Country At Extent Reach Region
"""


def outline(land):
    """The land as one path, equirectangular, half a degree a unit: viewBox 0 0 720 360."""
    parts = []
    for x in land:
        g = x["geometry"]
        polys = g["coordinates"] if g["type"] == "MultiPolygon" else [g["coordinates"]]
        for poly in polys:
            ring = poly[0]
            pts = []
            for lon, lat in ring:
                p = (round((lon + 180) * 2), round((90 - lat) * 2))
                if not pts or p != pts[-1]:
                    pts.append(p)
            if len(pts) < 4:
                continue
            s = "M%d %d" % pts[0]
            px, py = pts[0]
            rel = []
            for qx, qy in pts[1:]:
                rel.append("%d %d" % (qx - px, qy - py))
                px, py = qx, qy
            parts.append(s + "l" + " ".join(rel) + "z")
    return "".join(parts) + "\n"


def borders(nef, by_a3):
    """Each country's outer rings, a line each: KEY, then lon,lat pairs in tenths of a degree. The
    1:110m outlines (a few km off at a border; a small country missing falls back to its cities)."""
    out = ["# The countries' outlines (Natural Earth 1:110m, public domain), written by dev-tools/places.py:",
           "# a ring a line, the gazetteer's key then longitude,latitude pairs in tenths of a degree. arv",
           "# describe --ask finds the country a photo's GPS is in (the smallest ring holding it: an enclave)."]
    for x in nef["ne_110m_admin_0_countries"]:
        p = x["properties"]
        c = by_a3.get(p["ADM0_A3"]) or by_a3.get(p["SOV_A3"])
        if not c:
            continue
        g = x["geometry"]
        for poly in (g["coordinates"] if g["type"] == "MultiPolygon" else [g["coordinates"]]):
            pts = []
            for lon, lat in poly[0]:
                q = (round(lon * 10), round(lat * 10))
                if not pts or q != pts[-1]:
                    pts.append(q)
            if len(pts) >= 4:
                out.append("place:%s\t%s" % (c["key"], " ".join("%d,%d" % q for q in pts)))
    return "\n".join(out) + "\n"


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp()
    nef = load(src)
    records, nc, np, by_a3 = gazetteer(nef)
    with open(os.path.join(DATA, "default_places.rec"), "w", encoding="utf-8") as fp:
        fp.write(HEADER % (nc, np))
        for r in records:
            fp.write("\n" + "\n".join(r) + "\n")
    with open(os.path.join(DATA, "world.txt"), "w", encoding="utf-8") as fp:
        fp.write(outline(nef["ne_110m_land"]))
    with open(os.path.join(HERE, "..", "src", "arv-assist", "data", "countries.txt"), "w", encoding="utf-8") as fp:
        fp.write(borders(nef, by_a3))
    print("wrote default_places.rec (%d countries, %d places), world.txt and countries.txt" % (nc, np))


if __name__ == "__main__":
    main()
