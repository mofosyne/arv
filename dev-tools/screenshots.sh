#!/bin/sh
# Takes the documentation's screenshots from the samples, so they can be retaken after a change:
#   docs/screenshots/gui.png        arv gui on samples/home: Record › Discs (#discs), under the three tabs
#   docs/screenshots/disc.png       the index.html every disc carries (from samples/discs/TRIP-01_2019_4.iso)
#   docs/screenshots/assistant.png  the same, the Assistant open on a conversation whose replies are
#                                   scripted (dev-tools/fake-llm.c): it shows what the panel does, not
#                                   what a model would say; needs Node.js with Playwright, else skipped
#
#   sh dev-tools/screenshots.sh
# Needs a Chromium or Chrome ($CHROMIUM, else chromium, chromium-browser or
# google-chrome on PATH), and for disc.png 7z and the sample images (samples/fetch-discs.sh). docs/screenshots/ is
# export-ignore: it stays out of the tools/ copy on every disc.
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
out=$repo/docs/screenshots
iso=$repo/samples/discs/TRIP-01_2019_4.iso
browser=${CHROMIUM:-$(command -v chromium || command -v chromium-browser || command -v google-chrome || true)}
[ -n "$browser" ] || { echo "screenshots.sh: no Chromium or Chrome found (set CHROMIUM)" >&2; exit 1; }
[ -x "$repo/src/arv/build/arv" ] && [ -x "$repo/src/arv-gui/build/arv-gui" ] || make -s -C "$repo" >&2
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
"$repo/src/arv-gui/build/arv-gui" --home home --no-browser --port 0 > gui.out 2>&1 &
gui=$!
i=0
until url=$(grep -o 'http://[^ ]*' gui.out); do
    i=$((i + 1))
    [ $i -lt 50 ] || { cat gui.out >&2; exit 1; }
    sleep 0.2
done
shot 1300x700 "$url#discs" "$out/gui.png"

# the Assistant: the model's side scripted, its reads run for real against the samples
command -v node >/dev/null && node -e "require('playwright')" 2>/dev/null || {
    echo "screenshots.sh: no Node.js with Playwright: assistant.png left as it is" >&2; exit 0; }
kill "$gui"
make -s -C "$repo/src/arv-assist" build/fake-llm >&2
cat > reply.txt <<'EOF'
{"read": ["find", "place:kyoto"]}
===
{"read": ["todo", "--json"]}
===
{"say": "The Kyoto trip is TRIP-01_2019_4: copy A is in Home / Study / Box 1, copy B at Parents' house. Neither copy has been read back since it was burned, and its appraisal (essential) asks for 3 copies in 3 places, so one more copy somewhere else would help."}
===
{"propose": ["check", "/dev/sr0", "--copy", "A", "TRIP-01_2019_4"], "why": "Reads copy A in the drive, every sector, and logs the check."}
EOF
"$repo/src/arv-assist/build/fake-llm" port llm.log reply.txt reply.txt &
llm=$!
trap '[ -n "$gui" ] && kill "$gui" 2>/dev/null; kill "$llm" 2>/dev/null; rm -rf "${tmp:?}"' EXIT
until [ -s port ]; do sleep 0.1; done
rm gui.out
mkdir -p "$tmp/Documents/Family archive"            # long enough for the header to show it short
cp -r "$repo/samples/home" "$tmp/Documents/Family archive/.arv"
XDG_CONFIG_HOME=$tmp/config "$repo/src/arv-gui/build/arv-gui" --home "$tmp/Documents/Family archive/.arv" --no-browser --port 0 \
    --llm-url "http://127.0.0.1:$(cat port)/v1" --llm-model "a local model" > gui.out 2>&1 &
gui=$!
until url=$(grep -o 'http://[^ ]*' gui.out 2>/dev/null); do sleep 0.2; done
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
