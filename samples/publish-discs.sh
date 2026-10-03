#!/bin/sh
# Publish samples/discs/*.iso as the assets of the "samples" release (replacing the previous
# ones), with a SHA256SUMS file. Run after samples/make-samples.sh and pushing its commit.
# Needs the GitHub CLI (gh), logged in with write access.
#
#   samples/publish-discs.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
cd "$here/discs"
ls ./*.iso >/dev/null
sha256sum ./*.iso | sed 's|  \./|  |' > SHA256SUMS
commit=$(git rev-parse HEAD)
notes="Sample discs made by samples/make-samples.sh at $commit ($(date -u +%Y-%m-%d)). Download with samples/fetch-discs.sh, or one by one below; SHA256SUMS lists their checksums."
if gh release view samples >/dev/null 2>&1; then
    gh release edit samples --target "$commit" --notes "$notes"
    for old in $(gh release view samples --json assets --jq '.assets[].name'); do
        gh release delete-asset samples "$old" --yes
    done
else
    gh release create samples --target "$commit" --title "Sample discs" --notes "$notes" --latest=false
fi
gh release upload samples ./*.iso SHA256SUMS
