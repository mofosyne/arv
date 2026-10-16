#!/bin/sh
# Writes src/arv-assist/data/common-words.txt: the commonest English words, which say nothing about
# what a folder holds ("name", "hello", "page"), for arv describe --ask to leave out of a text's
# words until the archive's own texts say which are common (research/plan.md). Counted over books
# in the public domain (Project Gutenberg), so the list carries no licence; a word must be in half
# of them at least, so no character's name gets in.
#
#   sh dev-tools/common-words.sh [N]       (N words, 1500 by default; needs curl)
set -eu
n=${1:-1500}
here=$(cd "$(dirname "$0")" && pwd)
out=$here/../src/arv-assist/data/common-words.txt
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# novels, stories and plain prose of the 19th and early 20th century, British and American
books="1342 84 1661 2701 98 1260 64317 2814 1400 345 5200 174 76 43 20203 25344 36 35 158 1952 2600 768 1080 219"
for b in $books; do
    curl -sSf --retry 5 --retry-all-errors --retry-delay 2 "https://www.gutenberg.org/cache/epub/$b/pg$b.txt" -o "$work/$b.txt"
    # the book only, without Project Gutenberg's header and licence at either end
    sed -n '/\*\*\* *START OF/,/\*\*\* *END OF/p' "$work/$b.txt" | tr 'A-Z' 'a-z' | tr -cs 'a-z' '\n' \
        | awk 'length($0) >= 3' > "$work/$b.words"
    sort -u "$work/$b.words" > "$work/$b.uniq"
done
count=$(echo $books | wc -w)
cat "$work"/*.uniq | sort | uniq -c | awk -v half=$((count / 2)) '$1 >= half { print $2 }' > "$work/shared"
cat "$work"/*.words | sort | uniq -c | sort -k2 > "$work/all"
{
    echo "# The commonest English words (three letters or more), which say nothing about what a folder holds:"
    echo "# arv describe --ask leaves them out of a text's words until the archive's own texts say which are"
    echo "# common. Counted by dev-tools/common-words.sh over $count public-domain books (Project Gutenberg);"
    echo "# each word is in half of them at least; then the words of today's letters, forms and email, which"
    echo "# the books lack. A word of your vocabulary (a tag or an alias in tags.rec) is never left out."
    join -1 2 -2 1 "$work/all" "$work/shared" | sort -k2,2nr -k1,1 | head -n "$n" | awk '{ print $1 }' > "$work/top"
    cat "$work/top"
    # the books are old: the words of today's letters, forms and email, which they lack
    for w in hello thanks regards sincerely attached attachment email mail phone mobile signature sign signed \
        address postcode name surname date page pages number reference ref please dear kind yes okay ok \
        online website click link download file files document copy subject sent received reply forward; do
        grep -qx "$w" "$work/top" || [ ${#w} -lt 3 ] || echo "$w"
    done
} > "$out"
echo "wrote $out ($(grep -vc '^#' "$out") words)"
