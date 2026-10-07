#!/bin/sh
# Takes the documentation's two screenshots from the samples, so they can be retaken after a change:
#   docs/screenshots/gui.png    arv gui on samples/home: Record › Discs (#discs), under the three tabs
#   docs/screenshots/disc.png   the index.html every disc carries (from samples/discs/TRIP-01_2019_4.iso)
#
#   sh dev-tools/screenshots.sh
# Needs python3, a Chromium or Chrome ($CHROMIUM, else chromium, chromium-browser or
# google-chrome on PATH), and for disc.png 7z and the sample images (samples/fetch-discs.sh). docs/screenshots/ is
# export-ignore: it stays out of the tools/ copy on every disc.
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
out=$repo/docs/screenshots
iso=$repo/samples/discs/TRIP-01_2019_4.iso
browser=${CHROMIUM:-$(command -v chromium || command -v chromium-browser || command -v google-chrome || true)}
[ -n "$browser" ] || { echo "screenshots.sh: no Chromium or Chrome found (set CHROMIUM)" >&2; exit 1; }
[ -x "$repo/src/arv/build/arv" ] || make -s -C "$repo" >&2
tmp=$(mktemp -d)
gui=
trap '[ -n "$gui" ] && kill "$gui" 2>/dev/null; rm -rf "${tmp:?}"' EXIT
mkdir -p "$out"
shot() {    # shot WIDTHxHEIGHT URL FILE
    "$browser" --headless=new --no-sandbox --disable-gpu --hide-scrollbars --virtual-time-budget=3000 \
        --window-size="$(echo "$1" | tr x ,)" --screenshot="$3" "$2" >/dev/null 2>&1
    [ -s "$3" ] || { echo "screenshots.sh: no $3" >&2; exit 1; }
    echo "wrote $3"
}

# a disc's index.html: it is self-contained (skipped without the sample images)
if [ -f "$iso" ]; then
    7z x -o"$tmp/disc" "$iso" index.html >/dev/null
    shot 1000x760 "file://$tmp/disc/index.html" "$out/disc.png"
else
    echo "screenshots.sh: $iso is missing (samples/fetch-discs.sh): disc.png left as it is" >&2
fi

# arv gui, on a copy of the sample catalogue (the header shows its path: keep it short)
cp -r "$repo/samples/home" "$tmp/home"
cd "$tmp"
python3 "$repo/src/arv-gui/arv-gui" --home home --no-browser --port 0 > gui.out 2>&1 &
gui=$!
i=0
until url=$(grep -o 'http://[^ ]*' gui.out); do
    i=$((i + 1))
    [ $i -lt 50 ] || { cat gui.out >&2; exit 1; }
    sleep 0.2
done
shot 1300x700 "$url#discs" "$out/gui.png"
