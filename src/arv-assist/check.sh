#!/bin/sh
# Checks arv-assist (arv describe, arv tag, and the GUI's suggest and llm-status)
# against a fake OpenAI-compatible server (dev-tools/fake-llm.c), with the C arv for make and find.
# No network, no models.
#
#   sh check.sh build/arv-assist        (make check builds what it needs)
set -eu
tool=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
arv=$repo/src/arv/build/arv
ptyrun=$repo/src/arv/build/ptyrun
fake=$here/build/fake-llm
dir=$here/build/check
rm -rf "$dir"
mkdir -p "$dir"
cd "$dir"
ok() { echo "ok: $*"; }
no() { echo "FAILED: $*"; [ -f log ] && tail -c 600 log; exit 1; }
export ARV_NO_EXTRA_PLACES=1 ARV_APE=none HOME="$dir/userhome" XDG_CONFIG_HOME="$dir/userhome/.config" XDG_DATA_HOME="$dir/userhome/.local/share"
unset ARV_HOME ARCHIVE_LLM_URL ARCHIVE_LLM_MODEL
put() { mkdir -p "$(dirname "$1")"; printf '%s' "$2" > "$1"; touch -d "@${3:-1560000000}" "$1"; }

# ------------------------------------------------------------------ the fake server
cat > text.reply <<'EOF'
{"title": "Family trip photos 2019", "description": "Photos from a 2019 trip, one folder per day.",
 "subjects": ["Travel", "family", "travel"], "questions": ["Where was the 2019 trip?", "Who took the photos?"],
 "tags": {"photos/2019 trip": ["travel", "2019"], "no/such/folder": ["x"]}}
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
grep -q "Pictures from our holiday." inv.txt && grep -q "photos/2019 trip/ - 1 file" inv.txt \
    && grep -q "^File dates (modification time): 2019 to 2019" inv.txt \
    && ok "the inventory: folders, dates, and README text" || { cat inv.txt; no "inventory"; }
"$tool" --home home describe Trip $llm --save draft.json </dev/null 2>/dev/null || no "describe --save"
grep -q '"agent": "llm:fake-model"' draft.json && grep -q '"model": "fake-model"' log \
    && tr -d '\n ' < draft.json | grep -q '"subjects":\["travel","family"\]' \
    && tr -d '\n ' < draft.json | grep -q '"tags":{"photos/2019trip":\["travel","2019"\]}' \
    && grep -q '"authorship": "suggested"' draft.json \
    && ok "describe --save: the model from /models; subjects lowercased and once each; a folder that is not there dropped" \
    || { cat draft.json; no "the draft"; }
"$arv" --home home make -y --no-ecc --no-formats --draft draft.json -o t.iso Trip > make.out 2>&1 || { cat make.out; no "make --draft"; }
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
grep -q "it is not this computer" err.txt && ok "fenced JSON is read, prose is refused, a remote server is refused" || no "remote refusal"

# interactive: answer one question, skip the other; then accept title, description, subjects; drop the tags
printf 'Kyoto, Japan\n\n\n\n\nn\n' > answers
"$ptyrun" answers "$tool" --home home describe Trip $llm --rounds 1 --save d2.json > pty.out 2>&1 || { cat pty.out; no "interactive describe"; }
tail -1 log | grep -q "Kyoto, Japan" && grep -q '"Q: Where was the 2019 trip?\\nA: Kyoto, Japan"' d2.json \
    && tr -d '\n ' < d2.json | grep -q '"tags":{}' && grep -q '"authorship": "edited"' d2.json \
    && ok "interactive: the answer refines the next request and becomes a note; dropped tags make it edited" \
    || { cat d2.json; no "interactive describe"; }

# a draft applied to a disc that exists
printf '{"title": "New title", "subjects": ["travel"], "notes": ["Q: Who?\\nA: Us"], "tags": {"photos": ["travel"]}, "agent": "llm:test"}\n' > apply.json
"$tool" --home home describe "$id" --apply apply.json > apply.out || no "describe --apply"
grep -q "^Title: New title" home/catalog/archive.rec && grep -q "^Note: Q: Who?" home/catalog/archive.rec \
    && grep -A6 "^Note: updated Title" home/catalog/archive.rec >/dev/null \
    && grep -B8 "updated Title, Subject, Note, tags (suggested by a model, accepted by a person)" home/catalog/archive.rec | grep -q "^Agent: llm:test" \
    && grep -B8 "updated Title, Subject, Note, tags" home/catalog/archive.rec | grep -q "^Authorship: accepted" \
    && ok "describe --apply: title, subjects, notes and folder tags into the catalogue, accepted by the person" \
    || { cat apply.out; tail -30 home/catalog/archive.rec; no "describe --apply"; }

# a draft from elsewhere, on standard input, left a suggestion: one folder's tags and one file's join
# the others', and a path the disc does not have is skipped
printf '{"tags": {"photos/2019 trip": ["event:trip-2019"], "photos/2019 trip/IMG_0001.JPG": ["best"], "no/such": ["x"]}, "agent": "llm:other"}\n' \
    | "$tool" --home home describe "$id" --apply - --suggested > apply2.out 2> apply2.err || no "describe --apply -"
tsv=$(find home -path "*$id*" -name tags.tsv | head -1)
awk 'BEGIN { RS = "" } END { print }' home/catalog/archive.rec > last.rec      # the newest event
grep -q "^Tags: photos/2019 trip	event:trip-2019$" last.rec && grep -q "^Sets: Tags:photos/2019 trip$" last.rec \
    && grep -q "^Tags: photos/2019 trip/IMG_0001.JPG	best$" last.rec && ! grep -q "no/such" last.rec \
    && ! grep -q "^Tags: photos	" last.rec && grep -q "has no folder or file no/such" apply2.err \
    && ! grep -q "event:trip-2019" "$tsv" \
    && "$arv" --home home find best | grep -q "^TAG   $id  .*data/photos/2019 trip/IMG_0001.JPG  (best)" \
    && "$arv" --home home find travel | grep -q "^TAG   $id  .*data/photos/  (travel)" \
    && grep -q "^Authorship: suggested" last.rec && grep -q "^Agent: llm:other" last.rec && ! grep -q "^Agent: human:" last.rec \
    && ok "describe --apply - --suggested: tags recorded as an event (a file's too; the disc's tags.tsv as burned), found by find; an unknown path skipped; a suggestion" \
    || { cat apply2.out apply2.err; cat last.rec; no "describe --apply - --suggested"; }
# a disc made after: its catalogue carries the earlier disc's tags now, as burned and changed since
put Later/note.txt "later"
"$arv" --home home make -y --no-ecc --no-formats --set CODE --keep-stage --output-dir later Later >/dev/null 2>&1 \
    || no "make a later disc"
snap=$(find later -path "*/catalog/volumes/$id/tags.tsv" | head -1)
[ -n "$snap" ] && grep -q "^photos/2019 trip	event:trip-2019" "$snap" && grep -q "^photos/2019 trip/IMG_0001.JPG	best" "$snap" \
    && grep -q "^photos	travel" "$snap" \
    && ok "a later disc carries an earlier disc's tags as they are now (tags.tsv), and the events that changed them" \
    || { cat "$snap"; no "later disc's snapshot of tags"; }

# ------------------------------------------------------------------ data objects
# the folder made into a disc above is a data object; it said what its README says
grep -q "^Description: Pictures from our holiday.$" home/catalog/archive.rec || no "the object's description by rules"
"$tool" --home home describe "$id" $llm --show-inventory > dinv.txt
mkdir -p Code/proj && (cd Code/proj && git init -q && printf '# Proj\n\nProj reads the weather station logs.\n' > README.md \
    && git add -A && GIT_AUTHOR_DATE=2012-01-01T00:00:00Z git -c user.name=t -c user.email=t@example.invalid commit -qm one)
"$tool" describe Code $llm --show-inventory > cinv.txt
grep -qx "  the whole disc: Pictures from our holiday." dinv.txt \
    && grep -q "^Git repositories in it, as they describe themselves:" cinv.txt \
    && grep -qx '  proj/: "Proj": Proj reads the weather station logs. (2012-01-01)' cinv.txt \
    && ok "the model hears what a disc's data objects, or a folder's git repositories, say about themselves" \
    || { cat dinv.txt cinv.txt; no "objects in the inventory"; }
"$tool" --home home describe object:Trip $llm --show-inventory > oinv.txt
grep -q "^Archive name: Trip" oinv.txt && grep -q "  Description: Pictures from our holiday." oinv.txt \
    && grep -q "photos/2019 trip/ - 1 file" oinv.txt && grep -q "^--- README.txt ---" oinv.txt \
    && ok "describe OBJECT: the model sees its files (from its disc's listing), its README and what it says already" \
    || { cat oinv.txt; no "object inventory"; }
printf '{"title": "Our trip", "description": "Photos, one folder a day.", "notes": ["Kept by Mum"], "subjects": ["x"], "agent": "human:me"}\n' \
    | "$tool" --home home describe Trip --apply - > oapply.out 2> oapply.err || { cat oapply.err; no "describe OBJECT --apply"; }
uuid=$(sed -n '/^%rec: Object/,$p' home/catalog/archive.rec | grep -m1 '^Uuid: ' | cut -d' ' -f2)
grep -qx "Trip: updated Title, Description, Note" oapply.out && grep -q "Subjects, captions and the tags of folders within are a disc's" oapply.err \
    && grep -A3 "^Object: object:$uuid" home/catalog/archive.rec | grep -q "^Type: metadata modification" \
    && grep -q "^Note: updated Title, Description, Note of version 1 (written by a person)" home/catalog/archive.rec \
    && "$arv" --home home objects Trip | grep -qx '    "Our trip": Photos, one folder a day.' \
    && "$arv" --home home objects Trip | grep -qx '    note: Kept by Mum' \
    && ok "describe OBJECT --apply: its title, description and notes, with an event naming it (object:UUID)" \
    || { cat oapply.out oapply.err; tail -20 home/catalog/archive.rec; no "describe OBJECT --apply"; }
printf '{"objects": {".": {"title": "Trip photos"}, "nope": {"title": "x"}}, "agent": "llm:other"}\n' \
    | "$tool" --home home describe "$id" --apply - --suggested > oapply2.out 2> oapply2.err || no "a disc's draft with objects"
grep -q "updated object Trip (Title)" oapply2.out && grep -q "has no data object nope" oapply2.err \
    && grep -q "^Title: Trip photos" home/catalog/archive.rec \
    && grep -q "^Note: updated Title of version 1 (suggested by a model, not reviewed)" home/catalog/archive.rec \
    && ok "a disc's draft describes its objects by their path on it; one it does not have is skipped" \
    || { cat oapply2.out oapply2.err; no "objects in a disc's draft"; }
put Trip/photos/day2/IMG_0002.JPG jpeg2
"$arv" --home home make -y --no-ecc --no-formats -o obj2.iso Trip > make2.out 2>&1 || { cat make2.out; no "make, version 2"; }
[ "$(grep -c '^Title: Trip photos' home/catalog/archive.rec)" -eq 2 ] && [ "$(grep -c '^Note: Kept by Mum' home/catalog/archive.rec)" -eq 2 ] \
    && "$arv" --home home objects Trip | grep -q "Trip/  folder, 2 versions" \
    && "$arv" --home home objects Trip | grep -qx '    "Trip photos": Photos, one folder a day.' \
    && { ! command -v 7z >/dev/null || { 7z x -oobj2 obj2.iso catalog/archive.rec >/dev/null && grep -q "^Object: object:$uuid" obj2/catalog/archive.rec; }; } \
    && ok "a new version keeps what a person or a model wrote about the object; the disc's catalogue carries who wrote it" \
    || { cat make2.out; "$arv" --home home objects Trip; no "carried to version 2"; }
rm -r Trip/photos/day2
# at a terminal: answer one question, skip one, accept the title and description (no subjects or
# folder tags are asked for an object), apply
printf 'Kyoto\n\n\n\ny\n' > oanswers
"$ptyrun" oanswers "$tool" --home home describe object:Trip $llm --rounds 1 > opty.out 2>&1 || { cat opty.out; no "interactive describe OBJECT"; }
! grep -q "^> *Subjects: \[Enter\]\|^Subjects: \[Enter\]\|Keep the .* folder tags" opty.out && grep -q "Apply to the data object Trip (version 2)" opty.out \
    && [ "$(grep -c '^Title: Family trip photos 2019' home/catalog/archive.rec)" -eq 1 ] \
    && grep -q "^Note: Q: Where was the 2019 trip?" home/catalog/archive.rec \
    && grep -q "^Note: updated Title, Description, Note of version 2 (suggested by a model, accepted by a person)" home/catalog/archive.rec \
    && ok "describe OBJECT at a terminal: questions, then its title and description reviewed, applied to its newest version" \
    || { cat opty.out; no "interactive describe OBJECT"; }

# ------------------------------------------------------------------ questions kept for later
# make --draft kept the unattended draft's two questions about the disc
grep -q "2 questions kept for later" make.out && "$arv" --home home todo | grep -qx "  $id  2 questions" \
    || { cat make.out; "$arv" --home home todo; no "make --draft keeps the draft's questions"; }
printf '{"questions": ["where was the 2019 trip?", "Which camera?", ""], "agent": "llm:night"}\n' \
    | "$tool" --home home describe "$id" --apply - --suggested > q.out || no "describe --apply with questions"
grep -q "; 1 question kept for later (arv describe $id --answer)" q.out && "$arv" --home home todo | grep -qx "  $id  3 questions" \
    && grep -q "^Agent: llm:night" home/drafts/questions.rec \
    && ok "questions: a draft's are kept for its disc (make --draft, describe --apply), each once; arv todo counts them" \
    || { cat q.out; cat home/drafts/questions.rec; no "questions from drafts"; }
if "$tool" --home home describe "$id" --answer </dev/null >/dev/null 2>qerr.txt; then no "--answer without a terminal"; fi
grep -q "asks at a terminal" qerr.txt || no "--answer message"
printf 'Kyoto\n\n.\n' > qanswers
"$ptyrun" qanswers "$tool" --home home describe "$id" --answer > qa.out 2>&1 || { cat qa.out; no "describe --answer"; }
grep -q "^Note: Q: Where was the 2019 trip?" home/catalog/archive.rec && grep -q "^+ A: Kyoto" home/catalog/archive.rec \
    && grep -q "^Note: updated Note (written by a person)" home/catalog/archive.rec \
    && grep -A3 "^Text: Who took the photos?" home/drafts/questions.rec | grep -q "^Skipped: " \
    && grep -q "^Text: Which camera?" home/drafts/questions.rec && ! grep -q "^Text: Where was" home/drafts/questions.rec \
    && "$arv" --home home todo | grep -qx "  $id  1 question" && grep -q "1 answered (kept as notes), 1 skipped, 1 still waiting" qa.out \
    && ok "describe --answer (no model): an answer becomes a note by the person, Enter skips for good, '.' leaves the rest" \
    || { cat qa.out; cat home/drafts/questions.rec; no "describe --answer"; }
printf '{"questions": ["Who took the photos?"]}\n' | "$tool" --home home describe "$id" --apply - > q2.out
! grep -q "kept for later" q2.out || no "a skipped question came back"
# at a terminal, the kept question comes first, and the model sees its answer
: > log
printf 'Canon\n\n\n\n\n\n\nn\n' > qanswers2
"$ptyrun" qanswers2 "$tool" --home home describe "$id" $llm --rounds 1 > qb.out 2>&1 || { cat qb.out; no "describe with questions waiting"; }
grep -q "1 question about $id, kept from before" qb.out && head -1 log | grep -q 'Which camera?\\nA: Canon' \
    && ! "$arv" --home home todo | grep -q "question" \
    && ok "describe at a terminal asks the kept questions first; the model sees the answers" \
    || { cat qb.out; head -c 1500 log; no "kept questions before the model"; }

# ------------------------------------------------------------------ vision
i=0
while [ $i -lt 5 ]; do
    # a 1x1 PNG, made bigger than the 1 KiB "too small to be a photo" cut-off by a text chunk
    { printf '\211PNG\r\n\032\n'; head -c 1100 /dev/zero | tr '\0' 'c'; } > "img$i.png"
    put "Beach/photos/beach day/IMG_$i.png" "$(cat "img$i.png")"
    i=$((i + 1))
done
printf '{"title": "Beach", "description": "A day at the beach.", "subjects": ["beach"], "questions": [], "tags": {}}' > text.reply
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
        grep -q '"captions": {}' r.json || { cat r.json; no "an echoed answer kept: $reply"; }
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

# ------------------------------------------------------------------ which model: found, chosen, and an external one
ARV_LLM_PROBE="Fake=$url;Nothing=http://127.0.0.1:1/v1" OPENAI_API_KEY=sk-not-shown "$tool" llm-status --all --json > all.json \
    || no "llm-status --all"
tr -d '\n ' < all.json | grep -q '"name":"Fake","url":"'"$url"'","id":"fake","on":true,"found":true,"ready":true,"models":\[{"id":"fake-model"' \
    && tr -d '\n ' < all.json | grep -q '"name":"Nothing","url":"http://127.0.0.1:1/v1","id":"nothing","on":true,"found":false,"ready":false' \
    && tr -d '\n ' < all.json | grep -q '"name":"OpenAI","url":"https://api.openai.com/v1","key":"OPENAI_API_KEY","id":"openai","on":false,"set":true,"ready":false' \
    && tr -d '\n ' < all.json | grep -q '"name":"Anthropic","url":"https://api.anthropic.com/v1","key":"ANTHROPIC_API_KEY","id":"anthropic","on":false,"set":false' \
    && ! grep -q sk-not-shown all.json \
    && ok "llm-status --all: the servers on this computer and their models, and which providers' keys are set (never the key), each off until turned on" \
    || { cat all.json; no "llm-status --all"; }
"$tool" llm-use --url "$url" --model fake-model > use.out || no "llm-use"
grep -q "^Url: $url" userhome/.config/arv/models.rec && grep -q "^Model: fake-model" userhome/.config/arv/models.rec \
    && ! grep -q "^Remote" userhome/.config/arv/models.rec \
    && "$tool" llm-status | grep -q '"available": true, "url": "'"$url"'", "model": "fake-model", "external": false' \
    && "$tool" llm-status --all --json | tr -d '\n ' | grep -q '"text":{"url":"'"$url"'","model":"fake-model","from":"settings","external":false,"available":true}' \
    && "$tool" describe Trip --save chosen.json </dev/null 2>/dev/null && grep -q '"agent": "llm:fake-model"' chosen.json \
    && ok "llm-use keeps the choice in the XDG config's arv/models.rec, and describe uses it with no options" \
    || { cat use.out userhome/.config/arv/models.rec; no "llm-use"; }
"$tool" llm-test --url "$url" > test.json && "$tool" llm-test --url "$url" --pictures > testpic.json \
    && grep -q '"ok": true, "url": "'"$url"'", "model": "fake-model", "external": false' test.json && grep -q '"json": true' test.json \
    && grep -q '"saw_red": false' testpic.json && tail -1 log | grep -q 'iVBORw0KGgo' \
    && "$tool" llm-test --url http://127.0.0.1:1/v1 | grep -q '"ok": false' \
    && ok "llm-test: a fixed question (a red square for pictures), nothing from any file: answered, how fast, in JSON" \
    || { cat test.json testpic.json; no "llm-test"; }
"$tool" llm-use --off > /dev/null && "$tool" llm-status | grep -q '"available": false, "error": "model help is turned off' \
    && "$tool" describe Trip --save off.json </dev/null 2>&1 | grep -q "model help is turned off" \
    && "$tool" llm-status $llm | grep -q '"available": true' \
    && "$tool" llm-use --on > /dev/null && "$tool" llm-status | grep -q '"available": true' \
    && "$tool" llm-use --pictures "$url" --pictures-model fake-model > /dev/null && "$tool" llm-use --forget pictures > /dev/null \
    && grep -q "^Url: $url" userhome/.config/arv/models.rec && ! grep -q "^Vision" userhome/.config/arv/models.rec \
    && ok "model help off: no model asked unless a command names one; on again; a choice forgotten alone" \
    || { cat userhome/.config/arv/models.rec; no "on and off"; }
"$tool" llm-use --url https://api.openai.com/v1 --model m > prov.out 2>&1 && no "a provider chosen while off"
grep -q "OpenAI is off" prov.out \
    && "$tool" llm-use --enable openai > /dev/null && grep -q "^On: openai" userhome/.config/arv/models.rec \
    && OPENAI_API_KEY=k "$tool" llm-status --all --json | tr -d '\n ' | grep -q '"key":"OPENAI_API_KEY","id":"openai","on":true,"set":true' \
    && "$tool" llm-use --disable openai > /dev/null && ! grep -q "^On:" userhome/.config/arv/models.rec \
    && ARV_LLM_PROBE="Fake=$url" "$tool" llm-use --disable fake > /dev/null && grep -q "^Off: fake" userhome/.config/arv/models.rec \
    && ! grep -q "^Url" userhome/.config/arv/models.rec \
    && ARV_LLM_PROBE="Fake=$url" "$tool" llm-status --all --json | tr -d '\n ' | grep -q '"id":"fake","on":false,"found":false' \
    && "$tool" llm-use --disable ollama > /dev/null && "$tool" llm-status | grep -q "11434/v1 is turned off" \
    && "$tool" llm-status $llm | grep -q '"available": true' \
    && "$tool" llm-use --enable ollama > /dev/null && ARV_LLM_PROBE="Fake=$url" "$tool" llm-use --enable fake > /dev/null \
    && ! grep -q "^Off:" userhome/.config/arv/models.rec && "$tool" llm-use --enable nosuch 2>&1 | grep -q "no model server or provider nosuch" \
    && "$tool" llm-use --url "$url" --model fake-model > /dev/null \
    && ok "each source on or off: a provider off until turned on, a server here on until turned off (never asked), what it was chosen for forgotten" \
    || { cat prov.out userhome/.config/arv/models.rec; no "sources on and off"; }
ARCHIVE_LLM_KEY=sekrit "$tool" llm-status $llm > /dev/null && grep -q "^auth: Bearer sekrit" log \
    && ok "a key from the environment goes to the server as a bearer token" || no "the key"
"$tool" llm-use --url https://llm.example.invalid/v1 > ext.out 2>&1 && no "an external provider chosen without --external"
grep -q "is not this computer" ext.out && ! grep -q invalid userhome/.config/arv/models.rec \
    && "$tool" llm-use --pictures http://192.0.2.1/v1 > pic.out 2>&1 && no "a remote pictures model"
grep -q "pictures go only to a model on this computer" pic.out \
    && "$tool" llm-use --url https://llm.example.invalid/v1 --model m --external > /dev/null \
    && grep -q "^Remote: yes" userhome/.config/arv/models.rec \
    && "$tool" llm-status > ext.json 2> ext.err; grep -q '"available": false' ext.json \
    && grep -q "Using llm.example.invalid, an external inference provider" ext.err \
    && "$tool" describe Trip --vision > vis.out 2>&1 && no "pictures with an external text model"
grep -q "pictures go only to a model on this computer, and the text model is not one" vis.out \
    && "$tool" llm-use --clear > /dev/null && ! grep -q "^Url" userhome/.config/arv/models.rec \
    && ok "an external provider: refused unless chosen with --external, then said at each use; pictures never sent to one" \
    || { cat ext.out pic.out ext.json ext.err vis.out; no "an external provider"; }

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
emb="--embed-url $url --vocab $dir/tags.rec"
"$tool" --home th tag Stuff --show-summaries $emb > sum.txt
! grep -q DSC sum.txt && grep -q "beach trip: 1 photos" sum.txt || { cat sum.txt; no "summaries"; }
"$tool" --home th tag Stuff --top 1 $emb </dev/null > t.json 2>/dev/null || no "tag"
tr -d '\n ' < t.json | grep -q '"tags":{"beachtrip":\["travel"\],"catsandkittens":\["pets"\],"scripts":\["code"\]}' \
    && grep -q '"agent": "embeddings:fake-model"' t.json && grep -q '"authorship": "suggested"' t.json \
    && ok "tag: each folder gets its closest tag; camera names left out of the summaries" || { cat t.json; no "tag suggestions"; }
"$tool" --home th tag Stuff --save td.json $emb </dev/null 2>/dev/null || no "tag --save"
"$arv" --home th make -y --no-ecc --no-formats --draft td.json -o t2.iso Stuff > make2.out 2>&1 || { cat make2.out; no "make with tags"; }
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
grep -q '"model": "fake-model"' th/config/tag-examples.jsonl || no "the review was not remembered"
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
tr -d '\n ' < r.json | grep -q '"tags":{"board":\["electronics"\]}' && grep -q '"agent": "match rules"' r.json \
    && grep -q '"authorship": "automatic"' r.json || { cat r.json; no "rules only"; }
printf 'a\n' > answers
printf 'holiday, Place : Kyoto\na\n' > answers
"$ptyrun" answers "$tool" --home th tag Stuff --save typed.json $emb > pty3.out 2>&1 || { cat pty3.out; no "typed tags"; }
tr -d '\n ' < typed.json | grep -q '\["travel","place:kyoto"\]' && grep -q '"agent": "match rules + embeddings:fake-model"' typed.json \
    && ok "Match rules alone (automatic); typed tags normalised and aliases put in the vocabulary's words" \
    || { cat typed.json; no "aliases"; }

# /embeddings server selected by --embed-url
"$tool" --home th tag Stuff --top 1 --embed-url "$url" --vocab "$dir/tags.rec" </dev/null > e.json 2>/dev/null || no "--embed-url"
grep -q '"agent": "match rules + embeddings:fake-model"' e.json && tr -d '\n ' < e.json | grep -q '"catsandkittens":\["pets"\]' \
    && tr -d '\n ' < e.json | grep -q '"scripts":\["code"\]' \
    && ok "--embed-url: OpenAI-compatible embeddings server" || { cat e.json; no "embeddings server"; }

# local runtime/model-manager options and command are no longer accepted
if "$tool" --home th tag Stuff --top 1 > /dev/null 2>&1; then no "tag ran without an embeddings server"; fi
if "$tool" --home th tag Stuff --model bge-small-en-v1.5 > /dev/null 2>&1; then no "--model accepted"; fi
if "$tool" --home th tag Stuff --model-file model.gguf > /dev/null 2>&1; then no "--model-file accepted"; fi
if "$tool" --home th tag Stuff --llama-embedding llama-embedding > /dev/null 2>&1; then no "--llama-embedding accepted"; fi
if "$tool" --home th models status > /dev/null 2>&1; then no "models command accepted"; fi
ok "tag accepts only server embeddings or rules-only; models command removed"

# ------------------------------------------------------------------ describing without a model: --clues and --ask
"$arv" init askh >/dev/null 2>&1
put "ask/2019-07 Kyoto trip/day1/IMG_20190711_120000.jpg" a
put "ask/2019-07 Kyoto trip/day1/IMG_20190712_120000.jpg" b
put "ask/2019-07 Kyoto trip/fushimi/PXL_20190713_130000.jpg" c
put "ask/2019-07 Kyoto trip/notes.txt" d
"$tool" --home askh describe "ask/2019-07 Kyoto trip" --clues > clues.json 2> clues.err || { cat clues.err; no "describe --clues"; }
grep -q '^Clues: 4 files: 3 photos, 1 text document; dates 2019-07 (from names); named by a camera or phone' clues.err \
    && tr -d '\n ' < clues.json | grep -q '"tags":{".":\["travel","date:2019-07"\]}' \
    && grep -q '"title": "Kyoto trip, July 2019"' clues.json && grep -q '"authorship": "automatic"' clues.json \
    && grep -q '"description": "3 photos and 1 text document, July 2019."' clues.json \
    && ok "describe --clues: no model, no questions: dates from the names, a tag from an alias in them ('trip'), a title made from them" \
    || { cat clues.err clues.json; no "describe --clues"; }
# topics, event, place, person (typed), source, date (not EDTF, then EDTF), the folders too (fushimi: place, person), title, description
printf '\n\nkyoto\nann, bo\nsoon\n2019-07-11/2019-07-13\ny\n\n\nKyoto, July 2019\n\n' \
    | "$tool" --home askh describe "ask/2019-07 Kyoto trip" --ask --save ask.json > ask.out 2> ask.err || { cat ask.err; no "describe --ask"; }
tr -d '\n ' < ask.json | grep -q '"tags":{".":\["travel","event:kyoto-trip","place:kyoto","person:ann","person:bo","date:2019-07-11/2019-07-13"\]}' \
    && grep -q '"title": "Kyoto, July 2019"' ask.json \
    && grep -q '"description": "3 photos and 1 text document, in Kyoto, with Ann and Bo, of the Kyoto trip, 11 to 13 July 2019."' ask.json \
    && grep -q '"authorship": "human"' ask.json && grep -q 'soon is not an EDTF date' ask.err \
    && grep -q "^  \*1 travel  ('trip' is in the names)" ask.err && grep -q '^  \*1 kyoto-trip  (from the name)' ask.err \
    && ok "describe --ask: the clues' answers marked, a number or a typed answer each; a date must be EDTF; the description made from the answers" \
    || { cat ask.err ask.json; no "describe --ask"; }
"$arv" -C askh make -y --no-ecc --no-formats --set TRIP --output-dir askout "ask/2019-07 Kyoto trip" >/dev/null 2>&1 || no "make (ask)"
id=$("$arv" -C askh list | awk '{ print $1; exit }')
printf '\n\n\nann\n\n\n\n\n' | "$tool" --home askh describe "$id" --ask --no-folders --save disc.json >/dev/null 2>&1 \
    && "$tool" --home askh describe "$id" --apply disc.json > askapply.out 2>&1 \
    && grep -q "updated Title, Description, Subject, tags" askapply.out && "$arv" -C askh find 'person:ann' | grep -q "$id" \
    && ok "describe --ask on a disc, then --apply: its title, description and tags" || { cat askapply.out; no "describe --ask on a disc"; }
uuid=$("$arv" -C askh objects --json | tr ',' '\n' | sed -n 's/.*"uuid": "\([^"]*\)".*/\1/p' | head -1)
printf '\n2\n\n\n\n\n\n' | "$tool" --home askh describe "$uuid" --ask --save obj.json >/dev/null 2>&1 \
    && "$tool" --home askh describe "$uuid" --apply obj.json > objapply.out 2>&1 && grep -q "tags" objapply.out \
    && "$arv" -C askh objects --json | grep -q '"tags": "[^"]*event:' \
    && [ "$("$arv" -C askh objects --json | grep -o 'event:[a-z0-9-]*' | sort -u | wc -l)" -eq 1 ] \
    && ok "describe --ask on a data object: its own tags too, a one-valued namespace's new value replacing the old" \
    || { cat objapply.out; "$arv" -C askh objects --json; no "describe --ask on an object"; }
mkdir -p fakebin && printf '#!/bin/sh\necho %s\n' "'[{\"SourceFile\": \"a\", \"DateTimeOriginal\": \"2021:12:24 18:00:00\", \"Model\": \"Pixel 7\"}, {\"SourceFile\": \"b\", \"CreateDate\": \"2021:12:25 09:00:00\", \"Model\": \"Pixel 7\"}]'" > fakebin/exiftool
chmod +x fakebin/exiftool
put ex/a.jpg x
put ex/b.jpg y
PATH="$dir/fakebin:$PATH" "$tool" --home askh describe ex --clues > ex.json 2> ex.err || no "describe --clues with EXIF"
grep -q 'dates 2021-12 (from EXIF); camera Pixel 7 (EXIF)' ex.err && tr -d '\n ' < ex.json | grep -q '"source:pixel-7"' \
    && grep -q '"description": "2 photos, from a Pixel 7, December 2021."' ex.json \
    && ok "describe --clues: EXIF dates and camera, through exiftool when it is there" || { cat ex.err ex.json; no "EXIF clues"; }
# the same questions for a program (the GUI): the answers so far in, the next question out; nothing kept until the last
"$arv" init qh >/dev/null 2>&1
put "q/2019-07 Kyoto trip/IMG_20190711_120000.jpg" a
echo '{"answers": ["", "", "", "zed"]}' | "$tool" --home qh describe "q/2019-07 Kyoto trip" --answers - > q1.json 2>/dev/null
grep -q '"done": false' q1.json && grep -q '"number": 5' q1.json && grep -q '"kind": "choose"' q1.json \
    && ! grep -q 'person:zed' qh/config/tags.rec && [ ! -e qh/config/word-examples.tsv ] \
    && echo '{"answers": ["", "", "", "zed", "", "", ""]}' | "$tool" --home qh describe "q/2019-07 Kyoto trip" --answers - > q2.json 2>/dev/null \
    && grep -q '"done": true' q2.json && tr -d '\n ' < q2.json | grep -q '"person:zed"' \
    && grep -q '^Name: person:zed$' qh/config/tags.rec && [ "$(grep -vc '^#' qh/config/word-examples.tsv)" -eq 1 ] \
    && ok "describe --answers: one question at a time as JSON for the GUI; names, examples and rules kept only with the last answer" \
    || { cat q1.json q2.json; no "describe --answers"; }
# places: the GPS of the photos (the nearest place of the gazetteer), a typed place made an entry in it, a city in the names
mkdir -p gpsbin && printf '#!/bin/sh\necho %s\n' "'[{\"SourceFile\": \"a\", \"DateTimeOriginal\": \"2019:07:13 10:00:00\", \"GPSLatitude\": 34.9671, \"GPSLongitude\": 135.7727}, {\"SourceFile\": \"b\", \"GPSLatitude\": 34.968, \"GPSLongitude\": 135.773}]'" > gpsbin/exiftool
chmod +x gpsbin/exiftool
put "shrine visit/a.jpg" x
put "shrine visit/b.jpg" y
PATH="$dir/gpsbin:$PATH" "$tool" --home askh describe "shrine visit" --clues > gps.json 2> gps.err || no "describe --clues with GPS"
grep -q '; GPS in 2 photos, near Kyoto;' gps.err && tr -d '\n ' < gps.json | grep -q '"place:kyoto"' \
    && ok "describe --clues: the photos' GPS gives the nearest place of the gazetteer (the place kept, never the position)" \
    || { cat gps.err gps.json; no "GPS clues"; }
printf '\n\n1, fushimi-inari\n-\n\n\n\n' | PATH="$dir/gpsbin:$PATH" "$tool" --home askh describe "shrine visit" --ask --save gps2.json > /dev/null 2> gps2.err \
    || { cat gps2.err; no "describe --ask with GPS"; }
grep -q '^  \*1 kyoto  (the GPS of 2 photos: 7 km from Kyoto)' gps2.err \
    && grep -q 'Known from now on: place:fushimi-inari (Fushimi Inari), in Kyoto' gps2.err \
    && grep -q '"description": "2 photos, in Kyoto and Fushimi Inari, ' gps2.json \
    && "$arv" -C askh tags show place:fushimi-inari | grep -q '^  in: Kyoto, Japan (place:kyoto)' \
    && "$arv" -C askh tags show place:fushimi-inari | grep -q '^  kind: site' \
    && ok "describe --ask: a place typed becomes an entry of the gazetteer, in the known place named with it" \
    || { cat gps2.err gps2.json; no "a new place from --ask"; }
# where the GPS is: inside a place's box, within its reach, a city of the country it is in (not across a border), a
# country by its outline (an enclave: the smaller), a country too small to have one (its city trusted)
printf '#!/bin/sh\necho "[{\\"SourceFile\\": \\"a\\", \\"GPSLatitude\\": $LAT, \\"GPSLongitude\\": $LON}]"\n' > gpsbin/exiftool
"$arv" -C askh tags add place:fushimi-inari --extent "34.960,135.765 34.975,135.780" >/dev/null \
    && "$arv" -C askh tags add place:my-farm --parent japan --at 36.5,138.0 --reach 1km >/dev/null || no "tags add --extent, --reach"
put gpsw/a.jpg x
where() { LAT=$1 LON=$2 PATH="$dir/gpsbin:$PATH" "$tool" --home askh describe gpsw --clues 2>&1 >/dev/null | grep -o 'GPS in [^;]*'; }
[ "$(where 34.97 135.77)" = "GPS in 1 photo, in Fushimi Inari" ] && [ "$(where 36.505 138.0)" = "GPS in 1 photo, near My Farm" ] \
    && [ "$(where 36.52 138.0)" = "GPS in 1 photo, near Nagano" ] && [ "$(where 48.57 7.81)" = "GPS in 1 photo, in Germany" ] \
    && [ "$(where 48.58 7.75)" = "GPS in 1 photo, near Strasbourg" ] && [ "$(where -29.5 28.2)" = "GPS in 1 photo, in Lesotho" ] \
    && [ "$(where 39.0 140.5)" = "GPS in 1 photo, in Japan" ] && [ "$(where 1.35 103.82)" = "GPS in 1 photo, near Singapore" ] \
    && [ "$(where 0 -150)" = "GPS in 1 photo" ] \
    && ok "GPS: a place's own box or reach first, then a city of the country its outline holds (Kehl is not Strasbourg), else the country" \
    || { for p in "34.97 135.77" "36.505 138.0" "36.52 138.0" "48.57 7.81" "48.58 7.75" "-29.5 28.2" "39.0 140.5" "1.35 103.82" "0 -150"; do
             echo "$p: $(eval where $p)"; done; no "GPS: boxes, reaches and borders"; }
put "osaka/a.txt" "Letters about Lima and Lima again"
printf '\n\n\n\n\n\n\n\n' | "$tool" --home askh describe osaka --ask --save oo.json > /dev/null 2> oo.err || { cat oo.err; no "describe --ask (cities)"; }
mkdir -p xdata/iso-codes/json && printf '{"3166-2": [{"code": "JP-27", "name": "Osaka", "type": "Prefecture"}]}\n' > xdata/iso-codes/json/iso_3166-2.json
printf '\n\n\n\n\n\n\n\n' | ARV_NO_EXTRA_PLACES= XDG_DATA_DIRS="$dir/xdata" "$tool" --home askh describe osaka --ask --save oo2.json > /dev/null 2> oo2.err \
    || { cat oo2.err; no "describe --ask (a region of a city's name)"; }
grep -q '^  \*[0-9] osaka  (known: Ōsaka, Japan)' oo.err && grep -q '^   [0-9] lima  (known: Lima, Peru)' oo.err \
    && grep -q '^  \*[0-9] osaka  (known: Ōsaka, Japan)' oo2.err && ! grep -q 'osaka-prefecture' oo2.err \
    && ok "describe --ask: a city of the gazetteer in the names is offered, marked, saying where it is; one its text only mentions, unmarked" \
    || { cat oo.err; no "cities in the names"; }
# ------------------------------------------------------------------ words and tags, counted: from the record, the answers, the text
"$arv" init wh >/dev/null 2>&1
for n in 1 2 3; do
    put "kicad-board-$n/main.kicad_pcb" "x$n"
    "$arv" -C wh make -y --no-ecc --no-formats --set PROJ --output-dir wout "kicad-board-$n" >/dev/null 2>&1 || no "make (words)"
done
for id in $("$arv" -C wh list | awk '{ print $1 }'); do
    printf '{"tags": {".": ["electronics"]}, "authorship": "human", "agent": "human:t"}' | "$tool" --home wh describe "$id" --apply - >/dev/null || no "apply (words)"
done
put "new kicad thing/a.kicad_sch" z
"$tool" --home wh describe "new kicad thing" --clues > w.json 2> w.err || no "describe --clues (learned)"
grep -qx "  points to electronics: 'kicad' in 3 of 3 folders tagged electronics" w.err \
    && tr -d '\n ' < w.json | grep -q '"tags":{".":\["electronics"' && [ -s wh/cache/words.tsv ] \
    && ok "words and tags, counted from the record: a word of the names that went with a tag before marks it, saying how often (cache/words.tsv)" \
    || { cat w.err w.json; no "learned from the record"; }
for n in 1 2 3; do
    put "papers$n/a.txt" "Invoice for VAT. HMRC reference. Invoice total. HMRC VAT return."
    put "papers$n/b.txt" "Another invoice from HMRC about VAT."
    answer=n; [ "$n" = 3 ] && answer=y
    printf 'finance\n-\n\n\n\n%s\n\n\n' "$answer" | "$tool" --home wh describe "papers$n" --ask --no-folders > /dev/null 2> "p$n.err" \
        || { cat "p$n.err"; no "describe --ask (text)"; }
done
grep -q "words in the text: hmrc, invoice, vat" p1.err \
    && grep -q "^'papers' has been in 3 of 3 folders tagged finance: make it a rule (Match: \*papers\*)" p3.err \
    && grep -q "Added to the vocabulary" p3.err && grep -A1 '^Name: finance$' wh/config/tags.rec | grep -qx 'Match: \*papers\*' \
    && [ "$(grep -c 'finance.papers.hmrc,invoice,vat$' wh/config/word-examples.tsv)" -eq 3 ] \
    && ok "describe --ask keeps what it was told (config/word-examples.tsv), the text's words too; a word that keeps going with a tag is offered as a rule, put in tags.rec" \
    || { cat p1.err p3.err; no "answers kept, a rule offered"; }
put letters/x.txt "Dear sir, the HMRC invoice and VAT."
put letters/y.txt "HMRC VAT invoice again."
"$tool" --home wh describe letters --clues > l.json 2> l.err || no "describe --clues (text)"
grep -q "  points to finance: '[a-z]*' in 3 of 3 folders tagged finance, from the files' text" l.err \
    && tr -d '\n ' < l.json | grep -q '"finance"' \
    && ok "the words of text files point to tags too (weighed less than names'), learned from the answers" || { cat l.err; no "text words"; }
put forms/a.txt "Dear Sir,
Hello. Please find attached the self assessment for Anne Smith. Your name and address are below.
We met Anne Smith in Fushimi last week, and Fushimi was lovely. The family garden."
put forms/b.txt "Hello again. The self assessment is due. Anne Smith will visit Fushimi with the family.
Name: Bob
Address: 1 Lane"
"$tool" --home wh describe forms --clues > /dev/null 2> f.err || no "describe --clues (forms)"
grep -q "words in the text: anne smith, self assessment, fushimi, family" f.err && grep -q "names in the text: Anne Smith, Fushimi" f.err \
    && ! grep "words in the text" f.err | grep -q "hello\|name,\|address\|garden" \
    && ok "text: the language's commonest words left out (a built-in list), a vocabulary word kept ('family'); pairs that go together as phrases ('self assessment'); words written as names found" \
    || { cat f.err; no "common words, phrases, names"; }
printf '\n\n\n\n\n\n\n\n' | "$tool" --home wh describe forms --ask --no-folders > /dev/null 2> fa.err || no "describe --ask (forms)"
grep -A4 '^person:' fa.err | grep -q ' anne-smith  (a name in the text)' \
    && ok "describe --ask offers the text's names as people and places" || { cat fa.err; no "names offered"; }
"$arv" init rh >/dev/null 2>&1
for n in 1 2 3 4; do
    put "rose$n/a.txt" "We visited Rose at the farm. Then Rose made jam."
    put "rose$n/b.txt" "With Rose at the farm again."
    printf '\n\n\nrose\n\n\n\n\n\n' | "$tool" --home rh describe "rose$n" --ask --no-folders > /dev/null 2> "r$n.err" || { cat "r$n.err"; no "describe --ask (known)"; }
done
grep -q "Known from now on: person:rose (Rose), private" r1.err && grep -A2 '^Name: person:rose$' rh/config/tags.rec | grep -qx 'Label: Rose' \
    && grep -q '^  \*1 rose  (known: Rose)$' r2.err \
    && grep -q 'person: rose (known: Rose, in 3 folders before; taken without asking)' r4.err \
    && grep -q '^Object: name:person:rose$' rh/catalog/archive.rec \
    && ok "known names: a name confirmed in --ask becomes an entry; next time it is offered, marked; once used and in the same context, taken without asking (a common word too: 'Rose' written as a name)" \
    || { cat r1.err r2.err r4.err; no "known names in --ask"; }
if command -v pdftotext >/dev/null && command -v pdfinfo >/dev/null; then
    mkdir -p pdfs
    for f in a:"Tax return 2021":"HMRC self assessment" b:"Invoice 0042":"Invoice from HMRC"; do
        name=${f%%:*}; rest=${f#*:}; title=${rest%%:*}; text=${rest#*:}
        stream="BT /F1 12 Tf 10 100 Td ($text) Tj ET"
        { printf '%%PDF-1.4\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n'
          printf '2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n'
          printf '3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 300 144] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >> endobj\n'
          printf '4 0 obj << /Length %d >> stream\n%s\nendstream endobj\n' "${#stream}" "$stream"
          printf '5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n'
          printf '6 0 obj << /Title (%s) >> endobj\n' "$title"
          printf 'trailer << /Root 1 0 R /Info 6 0 R >>\n%%%%EOF\n'; } > "pdfs/$name.pdf"
    done
    "$tool" --home wh describe pdfs --clues > pdf.json 2> pdf.err || no "describe --clues (PDF)"
    grep -q '"description": "2 PDF documents (Tax return 2021, Invoice 0042)' pdf.json && grep -q "words in the text: hmrc" pdf.err \
        && ok "PDFs: their text through pdftotext, their titles through pdfinfo, in the description" || { cat pdf.err pdf.json; no "PDF clues"; }
    mkpdf() {   # mkpdf FILE TEXT: one page with TEXT on it; no TEXT: a page with nothing to read, as a scan has
        stream=${2:+"BT /F1 12 Tf 10 100 Td ($2) Tj ET"}
        { printf '%%PDF-1.4\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n'
          printf '2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n'
          printf '3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 300 144] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >> endobj\n'
          printf '4 0 obj << /Length %d >> stream\n%s\nendstream endobj\n' "${#stream}" "$stream"
          printf '5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n'
          printf 'trailer << /Root 1 0 R >>\n%%%%EOF\n'; } > "$1"
    }
    mkdir -p mixed scans
    mkpdf mixed/a.pdf "Council tax bill"; mkpdf mixed/b.pdf "Water bill"; mkpdf mixed/c.pdf ""
    mkpdf scans/1.pdf ""; mkpdf scans/2.pdf ""
    "$tool" --home wh describe mixed --clues > mixed.json 2> mixed.err || no "describe --clues (a scan among PDFs)"
    "$tool" --home wh describe scans --clues > scans.json 2> scans.err || no "describe --clues (scans)"
    grep -q '; 1 of 3 PDFs read with no text (scans: source:scan; ocrmypdf --skip-text, then describe again)' mixed.err \
        && tr -d '\n ' < mixed.json | grep -q '"c.pdf":\["source:scan"\]' && ! tr -d '\n ' < mixed.json | grep -q '".":\[[^]]*source:scan' \
        && tr -d '\n ' < scans.json | grep -q '".":\[[^]]*"source:scan"' \
        && ok "PDFs with no text are scans: said, with what to do (ocrmypdf --skip-text), and tagged source:scan (the folder's when most are, else each)" \
        || { cat mixed.err mixed.json scans.err scans.json; no "scans"; }
fi
echo "all checks passed"
