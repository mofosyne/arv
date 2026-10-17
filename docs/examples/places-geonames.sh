#!/bin/sh
# places-geonames.sh: more places for arv's gazetteer, from GeoNames (CC BY 4.0, geonames.org).
#
# arv's own gazetteer holds Natural Earth's countries and its 1,251 largest cities, and the regions
# of iso-codes when it is installed. This adds GeoNames' populated places, down to towns, with their
# positions, so describe --ask can name the town a folder or its photos' GPS points to. arv never
# downloads anything; this does, once, and arv reads what it writes:
#
#   $XDG_DATA_HOME/arv/places/geonames-citiesN.rec   (~/.local/share/arv/places/ by default)
#
# A place a tag uses is copied into the archive's own config/places.rec, so the record and its
# discs never depend on this file; delete it, or write it again later, as you like.
#
#   sh places-geonames.sh [15000|5000|1000|500]
#     the smallest population a place must have (GeoNames' citiesN files): 15000 by default,
#     about 26,000 places and 3 MB; 500 is about 220,000 places and 25 MB
#
# Needs curl, unzip and awk. GeoNames asks for credit: each place keeps Same: geonames:ID.
set -eu
n=${1:-15000}
case $n in 15000|5000|1000|500) ;; *) echo "usage: places-geonames.sh [15000|5000|1000|500]" >&2; exit 2 ;; esac
out=${XDG_DATA_HOME:-$HOME/.local/share}/arv/places
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
base=https://download.geonames.org/export/dump
curl -sSf --retry 5 --retry-all-errors -o "$work/cities.zip" "$base/cities$n.zip"
curl -sSf --retry 5 --retry-all-errors -o "$work/admin1.txt" "$base/admin1CodesASCII.txt"
unzip -p "$work/cities.zip" "cities$n.txt" > "$work/cities.txt"
mkdir -p "$out"
# keys as arv's gazetteer makes them: the name in ASCII; a name two places share takes the
# country's code, then the region's, then GeoNames' id
awk -F '\t' -v when="$(date +%Y-%m-%d)" -v n="$n" '
function slug(s) { s = tolower(s); gsub(/[^a-z0-9]+/, "-", s); gsub(/^-+|-+$/, "", s); return s }
FNR == NR { region[$1] = $2; next }
($7 == "P") {
    id[++m] = $1; name[m] = $2; ascii[m] = $3 != "" ? $3 : $2; lat[m] = $5; lon[m] = $6; cc[m] = $9
    reg[m] = region[$9 "." $11]; code[m] = $11 != "" ? $9 "-" $11 : ""
    k1[m] = slug(ascii[m]); c1[k1[m]]++
}
END {
    print "# GeoNames (geonames.org, CC BY 4.0): populated places of " n " people or more, written by"
    print "# arv'"'"'s docs/examples/places-geonames.sh on " when ". arv reads it as extra place data; a place a tag"
    print "# uses is copied into the archive'"'"'s own config/places.rec. Country: the country it is in (ISO 3166-1)."
    print ""
    print "%rec: Place"
    print "%key: Name"
    for (i = 1; i <= m; i++) {
        k = k1[i]
        if (c1[k] > 1) { k = k "-" tolower(cc[i]); k2[i] = k; c2[k]++ }
    }
    for (i = 1; i <= m; i++) {
        k = k1[i]
        if (c1[k] > 1) {
            k = k2[i]
            if (c2[k] > 1) { k = k "-" slug(reg[i]); c3[k]++; if (c3[k] > 1 || reg[i] == "") k = k "-" id[i] }
        }
        printf "\nName: place:%s\nLabel: %s\n", k, name[i]
        if (ascii[i] != name[i]) printf "Alias: %s\n", ascii[i]
        if (reg[i] != "") printf "Qualifier: %s\n", reg[i]
        if (code[i] != "") printf "Region: %s\n", code[i]      # often the ISO 3166-2 code (JP-26, US-CA)
        if (reg[i] != "") printf "Region: %s\n", reg[i]
        printf "Kind: city\nCountry: %s\nAt: %+06.2f%+07.2f/\nSame: geonames:%s\n", cc[i], lat[i], lon[i], id[i]
    }
}' "$work/admin1.txt" "$work/cities.txt" > "$work/out.rec"
mv "$work/out.rec" "$out/geonames-cities$n.rec"
echo "wrote $out/geonames-cities$n.rec ($(grep -c '^Name: ' "$out/geonames-cities$n.rec") places); arv programs lists it"
