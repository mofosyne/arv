#!/bin/sh
# Checks arv-assist (arv describe, arv tag, arv models, and the GUI's suggest and llm-status)
# against a fake model server (dev-tools/fake-llm.c) and a fake llama-embedding
# (dev-tools/fake-llama-embedding), with the C arv for make and find. No network, no models.
#
#   sh check.sh build/arv-assist        (make check builds what it needs)
set -eu
tool=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
arv=$repo/src/arvc/build/arvc
ptyrun=$repo/src/arvc/build/ptyrun
fake=$here/build/fake-llm
embedder=$repo/dev-tools/fake-llama-embedding
dir=$here/build/check
rm -rf "$dir"
mkdir -p "$dir"
cd "$dir"
ok() { echo "ok: $*"; }
no() { echo "FAILED: $*"; [ -f log ] && tail -c 600 log; exit 1; }
export ARV_APE=none HOME="$dir/userhome" XDG_CONFIG_HOME="$dir/userhome/.config" XDG_DATA_HOME="$dir/userhome/.local/share"
unset ARV_HOME ARCHIVE_LLM_URL ARCHIVE_LLM_MODEL ARCHIVE_LLAMA_EMBEDDING
put() { mkdir -p "$(dirname "$1")"; printf '%s' "$2" > "$1"; touch -d "@${3:-1560000000}" "$1"; }

# ------------------------------------------------------------------ the fake server
cat > text.reply <<'EOF'
{"title": "Family trip photos 2019", "description": "Photos from a 2019 trip, one folder per day.",
 "subjects": ["Travel", "family", "travel"], "questions": ["Where was the 2019 trip?", "Who took the photos?"],
 "folder_tags": {"photos/2019 trip": ["travel", "2019"], "no/such/folder": ["x"]}}
EOF
printf 'Caption: People on a sandy beach.\nTags: beach, sea, Beach' > image.reply
"$fake" port log text.reply image.reply &
server=$!
trap 'kill $server 2>/dev/null' EXIT
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s port ] && break; sleep 0.2; done
url=http://127.0.0.1:$(cat port)/v1
llm="--llm-url $url"

put Trip/photos/"2019 trip"/IMG_0001.JPG jpeg
put Trip/README.txt "Pictures from our holiday."

# ------------------------------------------------------------------ describe
"$tool" describe Trip $llm --show-inventory > inv.txt
grep -q "Pictures from our holiday." inv.txt && grep -q "photos/2019 trip/ - 1 files" inv.txt \
    && grep -q "^File dates (modification time): 2019 to 2019" inv.txt \
    && ok "the inventory: folders, dates, and README text" || { cat inv.txt; no "inventory"; }
"$tool" --home home describe Trip $llm --save draft.json </dev/null 2>/dev/null || no "describe --save"
grep -q '"agent": "llm:fake-model"' draft.json && grep -q '"model": "fake-model"' log \
    && tr -d '\n ' < draft.json | grep -q '"subjects":\["travel","family"\]' \
    && tr -d '\n ' < draft.json | grep -q '"folder_tags":{"photos/2019trip":\["travel","2019"\]}' \
    && grep -q '"authorship": "suggested"' draft.json \
    && ok "describe --save: the model from /models; subjects lowercased and once each; a folder that is not there dropped" \
    || { cat draft.json; no "the draft"; }
"$arv" --home home make -y --no-ecc --formats no --draft draft.json -o t.iso Trip > make.out 2>&1 || { cat make.out; no "make --draft"; }
id=$(cut -f1 make.out | tail -1)
grep -q "^Title: Family trip photos 2019" home/catalog/archive.rec && grep -q "^Type: metadata modification" home/catalog/archive.rec \
    && "$arv" --home home find travel | grep -q "TAG   $id" \
    && ok "arv make --draft takes it: title, subjects, folder tags (found by arv find)" || no "make with the draft"

printf 'Sure!\n```json\n%s\n```\n' "$(cat text.reply)" > fenced.reply
cp text.reply plain.reply
cp fenced.reply text.reply
"$tool" describe Trip $llm </dev/null 2>/dev/null | grep -q '"title": "Family trip photos 2019"' || no "fenced JSON"
echo "I cannot help with that." > text.reply
if "$tool" describe Trip $llm </dev/null >/dev/null 2>err.txt; then no "a reply without JSON accepted"; fi
grep -q "did not return JSON" err.txt || no "no-JSON message"
cp plain.reply text.reply
if "$tool" describe Trip --llm-url http://203.0.113.5:11434/v1 </dev/null >/dev/null 2>err.txt; then no "remote server used"; fi
grep -q "not a local address" err.txt && ok "fenced JSON is read, prose is refused, a remote server is refused" || no "remote refusal"

# interactive: answer one question, skip the other; then accept title, description, subjects; drop the tags
printf 'Kyoto, Japan\n\n\n\n\nn\n' > answers
"$ptyrun" answers "$tool" --home home describe Trip $llm --rounds 1 --save d2.json > pty.out 2>&1 || { cat pty.out; no "interactive describe"; }
tail -1 log | grep -q "Kyoto, Japan" && grep -q '"Q: Where was the 2019 trip?\\nA: Kyoto, Japan"' d2.json \
    && tr -d '\n ' < d2.json | grep -q '"folder_tags":{}' && grep -q '"authorship": "edited"' d2.json \
    && ok "interactive: the answer refines the next request and becomes a note; dropped tags make it edited" \
    || { cat d2.json; no "interactive describe"; }

# a draft applied to a disc that exists
printf '{"title": "New title", "subjects": ["travel"], "notes": ["Q: Who?\\nA: Us"], "folder_tags": {"photos": ["travel"]}, "agent": "llm:test"}\n' > apply.json
"$tool" --home home describe "$id" --apply apply.json > apply.out || no "describe --apply"
grep -q "^Title: New title" home/catalog/archive.rec && grep -q "^Note: Q: Who?" home/catalog/archive.rec \
    && grep -A6 "^Note: updated Title" home/catalog/archive.rec >/dev/null \
    && grep -B8 "updated Title, Subject, Note, folder tags (suggested by a model, accepted by a person)" home/catalog/archive.rec | grep -q "^Agent: llm:test" \
    && grep -B8 "updated Title, Subject, Note, folder tags" home/catalog/archive.rec | grep -q "^Authorship: accepted" \
    && ok "describe --apply: title, subjects, notes and folder tags into the catalogue, accepted by the person" \
    || { cat apply.out; tail -30 home/catalog/archive.rec; no "describe --apply"; }

# ------------------------------------------------------------------ vision
i=0
while [ $i -lt 5 ]; do
    # a 1x1 PNG, made bigger than the 1 KiB "too small to be a photo" cut-off by a text chunk
    { printf '\211PNG\r\n\032\n'; head -c 1100 /dev/zero | tr '\0' 'c'; } > "img$i.png"
    put "Beach/photos/beach day/IMG_$i.png" "$(cat "img$i.png")"
    i=$((i + 1))
done
printf '{"title": "Beach", "description": "A day at the beach.", "subjects": ["beach"], "questions": [], "folder_tags": {}}' > text.reply
: > log
PATH=/usr/bin:/bin "$tool" describe Beach $llm --vision --vision-per-folder 2 --save v.json </dev/null 2>/dev/null || no "describe --vision"
[ "$(grep -c '"image_url"' log)" -eq 2 ] && grep -q 'data:image/png;base64,' log \
    && tail -1 log | grep -q "People on a sandy beach." \
    && tr -d '\n ' < v.json | grep -q '"photos/beachday":\["beach","sea"\]' && grep -q 'sandy beach' v.json \
    && ok "--vision: two images per folder seen; the text model hears what they show; tags and captions in the draft" \
    || { cat v.json; no "vision"; }
for reply in 'Caption: A dog on grass.|Tags: Dog, grass, dog, park.|A dog on grass.|dog, grass, park' \
             '{"caption": "A cat.", "tags": "cat; pet"}|A cat.|cat, pet' \
             'A dog on grass.|A dog on grass.|' \
             'Caption: one short sentence|Tags: 3 to 6 short lowercase tags||'; do
    case $reply in
        Caption:*one*) printf 'Caption: one short sentence\nTags: 3 to 6 short lowercase tags' > image.reply; want_cap= ; want_tags= ;;
        Caption:*) printf 'Caption: A dog on grass.\nTags: Dog, grass, dog, park.' > image.reply; want_cap="A dog on grass."; want_tags="dog, grass, park" ;;
        \{*) printf '{"caption": "A cat.", "tags": "cat; pet"}' > image.reply; want_cap="A cat."; want_tags="cat, pet" ;;
        *) printf 'A dog on grass.' > image.reply; want_cap="A dog on grass."; want_tags= ;;
    esac
    PATH=/usr/bin:/bin "$tool" describe Beach $llm --vision --vision-per-folder 1 --save r.json </dev/null 2>/dev/null || no "vision reply"
    if [ -n "$want_cap" ]; then
        grep -q "\"photos/beach day\": \"$want_cap\"" r.json || { cat r.json; no "caption from: $reply"; }
    else
        grep -q '"folder_captions": {}' r.json || { cat r.json; no "an echoed answer kept: $reply"; }
    fi
    [ -z "$want_tags" ] || tr -d '\n' < r.json | sed 's/  */ /g' | grep -q "\"photos/beach day\": \[ \"$(echo "$want_tags" | sed 's/, /", "/g')\" \]" \
        || { cat r.json; no "tags from: $reply"; }
done
ok "image replies: Caption/Tags lines, JSON, bare text; tags cleaned and once each; an echo of the prompt ignored"

# ------------------------------------------------------------------ the GUI's calls
"$tool" llm-status $llm | grep -q '"available": true, "url": "'"$url"'", "model": "fake-model"' \
    && "$tool" llm-status --llm-url http://127.0.0.1:1/v1 | grep -q '"available": false' \
    || no "llm-status"
cp plain.reply text.reply
printf '{"source": "%s", "answers": [["Where?", "Kyoto"], ["Who?", " "]]}' "$dir/Trip" | "$tool" --home home suggest $llm > s.json
grep -q '"title": "Family trip photos 2019"' s.json && grep -q '"agent": "llm:fake-model"' s.json && grep -q '"seen": {}' s.json \
    && tail -1 log | grep -q 'Q: Where?\\nA: Kyoto' && ! tail -1 log | grep -q 'Q: Who?' \
    && ok "suggest and llm-status for arv-gui: JSON in and out, blank answers left out" || { cat s.json; no "suggest"; }

# ------------------------------------------------------------------ tag
cat > tags.rec <<'EOF'
%rec: Tag

Name: pets
Description: cats dogs kittens puppies

Name: code
Description: source code python programming scripts

Name: travel
Description: trip holiday sightseeing beach
EOF
put Stuff/"cats and kittens"/IMG_0001.jpg x
put Stuff/scripts/"python code.py" x
put Stuff/"beach trip"/DSC_0001.JPG x
echo "fake model" > fake.gguf
emb="--llama-embedding $embedder --model-file $dir/fake.gguf --vocab $dir/tags.rec"
"$tool" --home th tag Stuff --show-summaries $emb > sum.txt
! grep -q DSC sum.txt && grep -q "beach trip: 1 photos" sum.txt || { cat sum.txt; no "summaries"; }
"$tool" --home th tag Stuff --top 1 $emb </dev/null > t.json 2>/dev/null || no "tag"
tr -d '\n ' < t.json | grep -q '"folder_tags":{"beachtrip":\["travel"\],"catsandkittens":\["pets"\],"scripts":\["code"\]}' \
    && grep -q '"agent": "embeddings:fake.gguf"' t.json && grep -q '"authorship": "suggested"' t.json \
    && ok "tag: each folder gets its closest tag; camera names left out of the summaries" || { cat t.json; no "tag suggestions"; }
"$tool" --home th tag Stuff --save td.json $emb </dev/null 2>/dev/null || no "tag --save"
"$arv" --home th make -y --no-ecc --formats no --draft td.json -o t2.iso Stuff > make2.out 2>&1 || { cat make2.out; no "make with tags"; }
tid=$(cut -f1 make2.out | tail -1)
"$arv" --home th find pets | grep -q "TAG   $tid" || no "find a tag"
"$tool" --home th tag "$tid" --apply $emb </dev/null 2>/dev/null || no "tag --apply"
grep -A6 "^Note: folder tags for 3 folders from the tag vocabulary" th/catalog/archive.rec >/dev/null \
    && grep -B8 "^Note: folder tags for 3 folders" th/catalog/archive.rec | grep -q "^Authorship: suggested" \
    && grep -B8 "^Note: title, description, subjects and folder tags taken from a draft" th/catalog/archive.rec | grep -q "^Authorship: accepted" \
    && ok "tag --save, make --draft (accepted), then tag --apply on the disc (suggested: no terminal)" \
    || { tail -40 th/catalog/archive.rec; no "tag events"; }

# learning from a review: tag a folder by hand, and a similar folder gets that tag offered
put Learn/"kyoto temples garden"/a.jpg x
printf 'japan\n' > answers
"$ptyrun" answers "$tool" --home th tag Learn $emb > pty2.out 2>&1 || { cat pty2.out; no "interactive tag"; }
grep -q '"model": "fake.gguf"' th/config/tag-examples.jsonl || no "the review was not remembered"
rm -rf Learn
put Learn/"kyoto temples garden"/b.jpg x
put Learn/scripts/x.py x
"$tool" --home th tag Learn --top 2 $emb </dev/null > l.json 2>/dev/null
tr -d '\n ' < l.json | grep -q '"kyototemplesgarden":\[[^]]*"japan"' && ! tr -d '\n ' < l.json | grep -q '"scripts":\[[^]]*"japan"' \
    && ok "tags you review are remembered: a folder like one you tagged gets that tag offered, others do not" \
    || { cat l.json; no "learning"; }

# rules, aliases, and a typed answer put in the vocabulary's words
printf 'Alias: holiday\n\nName: electronics\nDescription: circuits\nMatch: *.kicad_pcb\n' >> tags.rec
put Stuff/board/x.kicad_pcb x
"$tool" --home th tag Stuff --rules-only --vocab "$dir/tags.rec" </dev/null > r.json 2>/dev/null
tr -d '\n ' < r.json | grep -q '"folder_tags":{"board":\["electronics"\]}' && grep -q '"agent": "match rules"' r.json \
    && grep -q '"authorship": "automatic"' r.json || { cat r.json; no "rules only"; }
printf 'a\n' > answers
printf 'holiday, Place : Kyoto\na\n' > answers
"$ptyrun" answers "$tool" --home th tag Stuff --save typed.json $emb > pty3.out 2>&1 || { cat pty3.out; no "typed tags"; }
tr -d '\n ' < typed.json | grep -q '\["travel","place:kyoto"\]' && grep -q '"agent": "match rules + embeddings:fake.gguf"' typed.json \
    && ok "Match rules alone (automatic); typed tags normalised and aliases put in the vocabulary's words" \
    || { cat typed.json; no "aliases"; }

# an /embeddings server instead of llama-embedding
"$tool" --home th tag Stuff --top 1 --embed-url "$url" --vocab "$dir/tags.rec" </dev/null > e.json 2>/dev/null || no "--embed-url"
grep -q '"agent": "match rules + embeddings:fake-model"' e.json && tr -d '\n ' < e.json | grep -q '"catsandkittens":\["pets"\]' \
    && tr -d '\n ' < e.json | grep -q '"scripts":\["code"\]' \
    && ok "--embed-url: an embeddings server instead of the built-in model" || { cat e.json; no "embeddings server"; }

# ------------------------------------------------------------------ models
echo "something else" > bad.gguf
if "$tool" --home th models fetch --from bad.gguf > m.out 2>&1; then no "a model with the wrong checksum installed"; fi
grep -q "does not have the expected checksum" m.out && "$tool" --home th models status | grep -q "^bge-small-en-v1.5.*not downloaded" \
    && ok "models: a file with the wrong checksum is refused; status lists the model" || { cat m.out; no "models"; }
echo "all checks passed"
