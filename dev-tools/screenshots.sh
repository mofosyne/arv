#!/bin/sh
# Takes the documentation's screenshots from the samples, so they can be retaken after a change:
#   docs/screenshots/disc.png       the index.html every disc carries (from samples/discs/TRIP-01_2019_4.iso)
#   docs/screenshots/gui/VIEW.png   arv gui on samples/home, every view (docs/gui.md), VIEW as in its
#                                   address (gui/discs.png is #discs); setup.png is the first run, with no
#                                   archive. A view that shows nothing until it is used is used first: a
#                                   plan of the sample sources, a folder looked at, a search, a job
#   docs/screenshots/assistant.png  Record › Discs with the Assistant open on a conversation whose replies
#                                   are scripted (dev-tools/fake-llm.c): it shows what the panel does, not
#                                   what a model would say
#
#   sh dev-tools/screenshots.sh
# Needs a Chromium or Chrome ($CHROMIUM, else chromium, chromium-browser or google-chrome on PATH); for
# disc.png 7z and the sample images (samples/fetch-discs.sh); for arv gui's, Node.js with Playwright and
# python3 (samples/gen_sources.py). It works in ${TMPDIR:-/tmp}/arv-screenshots, a fixed place, as the
# pictures show its paths: that folder stands for the person's home folder. docs/screenshots/ is not on
# discs (disc-tools.txt lists what is).
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
out=$repo/docs/screenshots
iso=$repo/samples/discs/TRIP-01_2019_4.iso
browser=${CHROMIUM:-$(command -v chromium || command -v chromium-browser || command -v google-chrome || true)}
[ -n "$browser" ] || { echo "screenshots.sh: no Chromium or Chrome found (set CHROMIUM)" >&2; exit 1; }
[ -x "$repo/src/arv/build/arv" ] && [ -x "$repo/src/arv-gui/build/arv-gui" ] || make -s -C "$repo" >&2
tmp=${TMPDIR:-/tmp}/arv-screenshots
rm -rf "${tmp:?}"
mkdir -p "$tmp/.run"
gui= llm=
trap '[ -n "$gui" ] && kill "$gui" 2>/dev/null; [ -n "$llm" ] && kill "$llm" 2>/dev/null; rm -rf "${tmp:?}"' EXIT
mkdir -p "$out/gui"
shot() {    # shot WIDTHxHEIGHT URL FILE
    "$browser" --headless=new --no-sandbox --disable-gpu --hide-scrollbars --virtual-time-budget=3000 \
        --window-size="$(echo "$1" | tr x ,)" --screenshot="$3" "$2" >/dev/null 2>&1
    [ -s "$3" ] || { echo "screenshots.sh: no $3" >&2; exit 1; }
    echo "wrote $3"
}

# a disc's index.html: it is self-contained (skipped without the sample images)
if [ -f "$iso" ]; then
    7z x -o"$tmp/.run/disc" "$iso" index.html >/dev/null
    shot 1000x760 "file://$tmp/.run/disc/index.html" "$out/disc.png"
else
    echo "screenshots.sh: $iso is missing (samples/fetch-discs.sh): disc.png left as it is" >&2
fi

command -v node >/dev/null && node -e "require('playwright')" 2>/dev/null || {
    echo "screenshots.sh: no Node.js with Playwright: gui/ and assistant.png left as they are" >&2; exit 0; }

# the home folder: the sample sources (one file added to the Kyoto trip since its disc), a copy of the
# sample catalogue (the header shows its path: long enough to show it short), and a plan of two discs
python3 "$repo/samples/gen_sources.py" "$tmp/sources" >/dev/null
trip=$tmp/sources/2019-07_Kyoto_trip
echo "Nara, the last day: the deer park and Todai-ji." > "$trip/notes, day 4.txt"
touch -t 201907211800 "$trip/notes, day 4.txt"
home="$tmp/Documents/Family archive/.arv"
mkdir -p "$tmp/Documents/Family archive"
cp -r "$repo/samples/home" "$home"
a() { "$repo/src/arv/build/arv" --home "$home" "$@" >/dev/null; }
a plan new HOLIDAYS --title Holidays --set TRIP
a plan add HOLIDAYS "$trip" "$tmp/sources/Family_photos_2020-2021/2021 Beach"
a plan add HOLIDAYS "$tmp/sources/Family_photos_2020-2021/2021 Christmas" --disc new
unset ARV_HOME
export HOME="$tmp" XDG_CONFIG_HOME="$tmp/.config"
serve() {   # serve FOLDER ARGS...: arv gui, started in FOLDER; its address in $url
    rm -f "$tmp/.run/gui.out"
    (cd "$1" && shift && exec "$repo/src/arv-gui/build/arv-gui" --no-browser --port 0 "$@") > "$tmp/.run/gui.out" 2>&1 &
    gui=$!
    i=0
    until url=$(grep -o 'http://[^ ]*' "$tmp/.run/gui.out" 2>/dev/null); do
        i=$((i + 1))
        [ $i -lt 50 ] || { cat "$tmp/.run/gui.out" >&2; exit 1; }
        sleep 0.2
    done
}

# the first run: started in a folder with no archive here or above, and no default
serve "$tmp/sources"
URL=$url OUT=$out/gui BROWSER=$browser node - <<'EOF'
const { chromium } = require('playwright');
(async () => {
  const b = await chromium.launch({ executablePath: process.env.BROWSER });
  const p = await b.newPage({ viewport: { width: 1300, height: 820 } });
  await p.goto(process.env.URL); await p.waitForSelector('#tab-setup:not([hidden])');
  await p.waitForTimeout(300);
  await p.screenshot({ path: process.env.OUT + '/setup.png' });
  await b.close();
})();
EOF
echo "wrote $out/gui/setup.png"
kill "$gui"

# every view, each in a page of its own (the view is read from the address once), in the order of the tabs
serve "$tmp" --home "$home"
URL=$url OUT=$out/gui BROWSER=$browser SOURCES=$tmp/sources node - <<'EOF'
const { chromium } = require('playwright');
const { URL: url, OUT: out, SOURCES: sources } = process.env;
(async () => {
  const b = await chromium.launch({ executablePath: process.env.BROWSER });
  const ctx = await b.newContext({ viewport: { width: 1300, height: 820 } });
  let p = null;
  const settle = async () => { await p.waitForLoadState('networkidle'); await p.waitForTimeout(300); };
  async function open(view) {
    if (p) await p.close();
    p = await ctx.newPage();
    await p.goto(url + '#' + view);
    await p.waitForFunction(() => /owed|Nothing/.test(document.getElementById('owed-line').textContent));
    await settle();
  }
  async function shot(name, full) {
    await p.screenshot({ path: out + '/' + name + '.png', fullPage: !!full });
    console.log('wrote ' + out + '/' + name + '.png');
  }

  await open('make');                 // Archive
  await p.fill('#m-source', sources + '/2019-07_Kyoto_trip');
  await p.click('text=Fill from the files (no model)');
  await p.waitForFunction(() => document.getElementById('m-title').value !== '');
  await shot('make');
  await open('master');
  await p.click('#p-archived'); await p.waitForSelector('#p-discs .flag'); await settle();
  await shot('master');
  await open('status');
  await p.fill('#st-folder', sources + '/2019-07_Kyoto_trip'); await p.click('#st-form button[type=submit]');
  await p.waitForSelector('#st-out h2'); await settle();
  await shot('status');

  for (const v of ['objects', 'collections', 'discs']) { await open(v); await shot(v); }   // Record
  await p.click('#disc-table tr:has-text("TRIP-01_2019_4")');
  await p.waitForFunction(() => !/Reading/.test(document.getElementById('disc-detail').textContent)); await settle();
  await shot('discs-detail', true);
  for (const v of ['places', 'map', 'names']) { await open(v); await shot(v); }
  await open('search');
  await p.fill('#q', 'beach'); await p.click('#search-form button[type=submit]');
  await p.waitForSelector('#search-results h2'); await settle();
  await shot('search');

  for (const v of ['owed', 'tools', 'inventory']) { await open(v); await shot(v); }        // Verify
  await open('audit');
  await p.click('#audit-form button[type=submit]');
  await p.waitForFunction(() => !/Reading/.test(document.getElementById('audit-out').textContent)); await settle();
  await shot('audit');
  await open('restore'); await shot('restore');

  for (const v of ['models', 'programs']) { await open(v); await shot(v); }                // Settings

  // Jobs, last: making the plan's images (without error correction, quick) adds two discs to the record
  await open('master');
  await p.check('#p-noecc'); await p.click('#p-make-btn');
  await p.waitForSelector('#jobs .ok, #jobs .bad', { timeout: 60000 }); await settle();
  await shot('jobs');
  await b.close();
})();
EOF
kill "$gui"

# the Assistant: the model's side scripted, its reads run for real against a fresh copy of the samples
make -s -C "$repo/src/arv-assist" build/fake-llm >&2
cat > "$tmp/.run/reply.txt" <<'EOF'
{"read": ["find", "place:kyoto"]}
===
{"read": ["todo", "--json"]}
===
{"say": "The Kyoto trip is TRIP-01_2019_4: copy A is in Home / Study / Box 1, copy B at Parents' house. Neither copy has been read back since it was burned, and its appraisal (essential) asks for 3 copies in 3 places, so one more copy somewhere else would help."}
===
{"propose": ["check", "/dev/sr0", "--copy", "A", "TRIP-01_2019_4"], "why": "Reads copy A in the drive, every sector, and logs the check."}
EOF
(cd "$tmp/.run" && exec "$repo/src/arv-assist/build/fake-llm" port llm.log reply.txt reply.txt) &
llm=$!
until [ -s "$tmp/.run/port" ]; do sleep 0.1; done
rm -rf "$home" "$tmp/.config"
cp -r "$repo/samples/home" "$home"
serve "$tmp" --home "$home" --llm-url "http://127.0.0.1:$(cat "$tmp/.run/port")/v1" --llm-model "a local model"
URL="$url#discs" OUT="$out/assistant.png" BROWSER="$browser" node - <<'EOF'
const { chromium } = require('playwright');
(async () => {
  const b = await chromium.launch({ executablePath: process.env.BROWSER });
  const p = await b.newPage({ viewport: { width: 1400, height: 820 } });
  await p.goto(process.env.URL); await p.waitForSelector('#owed-line');
  await p.waitForTimeout(500);
  await p.click('#chat-btn');
  for (const q of ['Is the Kyoto trip safe? Where are its copies?', 'Check copy A, it is in the drive now']) {
    const n = await p.locator('#chat .msg:not(.you), #chat .card').count();
    await p.fill('#chat-input', q); await p.keyboard.press('Enter');
    await p.waitForFunction(k => document.querySelectorAll('#chat .msg:not(.you), #chat .card').length > k, n);
  }
  await p.waitForTimeout(300);
  await p.screenshot({ path: process.env.OUT });
  await b.close();
})();
EOF
echo "wrote $out/assistant.png"

# 256 colours: under half the size, with no difference to see (with Pillow; else left as taken)
python3 -c 'import PIL' 2>/dev/null || { echo "screenshots.sh: no Pillow: the pictures left in full colour" >&2; exit 0; }
python3 - "$out"/gui/*.png "$out/assistant.png" <<'EOF'
import sys
from PIL import Image
for f in sys.argv[1:]:
    Image.open(f).convert("RGB").quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).save(f, optimize=True)
EOF
