#!/bin/sh
# Make the sample discs: samples/discs/*.iso (about 40 MB in total) and their
# home catalogue in samples/home.
#
#   samples/make-samples.sh [OUTPUT_DIR]      (default: samples/)
#
# Needs: python3, genisoimage, git, the speed47 dvdisaster fork (the stock
# 0.79.10 build pads every image to CD size), and lib/udfmake (built here with
# make if missing). The discs use a custom 3000-sector "medium" (6 MB) so they
# stay small; real discs use --medium bd25 (the default) or bd100.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
out=$(mkdir -p "${1:-$here}" && cd "${1:-$here}" && pwd)

if ! dvdisaster --help 2>&1 | grep -q no-bdr-defect-management; then
    echo "error: needs the speed47 dvdisaster fork on PATH (https://github.com/speed47/dvdisaster)" >&2
    exit 1
fi
[ -x "$repo/lib/udfmake/build/udfmake" ] || make -s -C "$repo/lib/udfmake"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
src=$work/sources
python3 "$here/gen_sources.py" "$src"

home=$out/home
discs=$out/discs
rm -rf "$home" "$discs"
mkdir -p "$discs"
a() { python3 "$repo/archive" --home "$home" "$@"; }
common="-y --medium-sectors 3000 --output-dir $discs --creator Sample_Person --formats no"

draft() {  # draft NAME JSON: a hand-written metadata draft
    printf '%s\n' "$2" > "$work/$1.json"
    echo "$work/$1.json"
}

echo "== locations"
a location add HOME "Home"
a location add STUDY "Study" --in HOME
a location add BOX1 "Box 1, blue lid" --in STUDY
a location add SAFE "Fire safe" --in HOME
a location add OFFSITE "Parents' house"

echo "== 1. Kyoto trip: TRIP, namespaced folder tags, public"
a make $common --set trip --location BOX1 --access public \
    --draft "$(draft trip '{"title": "Kyoto, July 2019",
      "description": "Three days in Kyoto: shrines, temples and the bamboo grove.",
      "subjects": ["travel", "Japan"], "agent": "sample script (hand-written)",
      "notes": ["Sample disc made by samples/make-samples.sh; the photos are generated."],
      "folder_tags": {".": ["travel", "place:kyoto", "event:kyoto-2019"],
                      "day1 Fushimi Inari": ["place:fushimi-inari"], "day2 Kinkaku-ji": ["place:kinkaku-ji"],
                      "day2 Arashiyama": ["place:arashiyama", "nature"], "day3 Kiyomizu-dera": ["place:kiyomizu-dera"]}}')" \
    "$src/2019-07_Kyoto_trip"

echo "== 2. Weather station: the alias 'project' -> PROJ; Match rules add CODE and ELEC"
a make $common --set project --location BOX1 \
    --draft "$(draft proj '{"title": "Weather station project",
      "description": "ESP32 weather station: firmware, KiCad board, gerbers and the git repository.",
      "agent": "sample script (hand-written)",
      "folder_tags": {"firmware": ["code", "project:weather-station"],
                      "hardware": ["electronics", "project:weather-station"]}}')" \
    "$src/Projects_weather_station"

echo "== 3. Taxes 2019: sealed (other discs show only its id and location)"
a make $common --set taxes --location SAFE --access sealed --title "Tax return 2019" \
    --description "Tax return and quarterly receipts for 2019." "$src/Taxes_2019"

echo "== 4. Scanned letters: SCAN plus the LETTERS category, split over several discs"
a make $common --set scan --category letters --location BOX1 --split --title "Letters 1995-2008, scanned" \
    --description "Scans of letters kept since 1995." "$src/Scans_letters"

echo "== 5. Family photos as a UDF 2.50 image; made last, so it carries the whole catalogue"
a make $common --set family --location OFFSITE --filesystem udf250 \
    --draft "$(draft family '{"title": "Family photos 2020-2021",
      "description": "Birthdays, the garden, the beach and Christmas.",
      "agent": "sample script (hand-written)",
      "folder_tags": {"2020 Birthday": ["family", "event:birthday-2020"], "2020 Garden": ["home", "nature"],
                      "2021 Beach": ["travel", "place:beach"], "2021 Christmas": ["celebration"]}}')" \
    "$src/Family_photos_2020-2021"

echo "== after burning (home catalogue only)"
for d in $(a list | cut -f1); do
    a burned "$d" --copies 1 >/dev/null
done
a burned "$(a list --in TRIP | cut -f1)" --copies 1 --location OFFSITE --note "second copy for the parents"
a note "$(a list --in PROJ | cut -f1)" "Board rev B; rev A gerbers were never ordered."

echo "== check every image with dvdisaster"
for iso in "$discs"/*.iso; do
    a check --image "$iso" | tail -1
done
a index

echo "== result"
a list
ls -l "$discs"
du -ch "$discs"/*.iso | tail -1
