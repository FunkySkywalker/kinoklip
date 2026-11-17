#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# publish_dev.sh -- puts the dev build from scripts/dev_build.sh on GitHub as the rolling "dev"
# pre-release: https://github.com/SeniorMatt/kinoklip/releases/tag/dev
#
# Usage: scripts/publish_dev.sh [notes-file]
#
# The release keeps one set of files (KinoklipDev-x86_64.AppImage, its .zsync, SHA256SUMS-dev.txt),
# replaced on every publish, and the "dev" tag moves to the build's commit. A dev copy opened
# with Gear Lever updates from here in one click: its update information looks for KinoklipDev-*
# files in the newest release that has them. Release copies look for Kinoklip-* files and skip
# this release, so publishing a dev build never changes what release users get.
# The notes say the build is untested and list the commits since the last release.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
OUT=dist/dev
IMAGE="$OUT/KinoklipDev-x86_64.AppImage"
TAG=dev

log() { echo "publish_dev: $*" >&2; }
[ -f "$IMAGE" ] && [ -f "$IMAGE.zsync" ] || { log "no $IMAGE (+ .zsync): run scripts/dev_build.sh"; exit 1; }

STAMP="$("$IMAGE" --version 2>/dev/null | tail -n1)" # "Kinoklip 1.9.0 dev (abc1234)"
COMMIT="$(sed -n 's/.*dev (\([0-9a-f]*\)).*/\1/p' <<<"$STAMP")"
[ -n "$COMMIT" ] || { log "cannot read the commit from: $STAMP"; exit 1; }
git cat-file -e "$COMMIT^{commit}" || { log "unknown commit $COMMIT"; exit 1; }
git merge-base --is-ancestor "$COMMIT" origin/main 2>/dev/null || {
    log "pushing main (the dev build's commit must be on GitHub)"
    git push -q origin main
}

(cd "$OUT" && sha256sum KinoklipDev-x86_64.AppImage KinoklipDev-x86_64.AppImage.zsync > SHA256SUMS-dev.txt)

NOTES="${1:-}"
if [ -z "$NOTES" ]; then
    NOTES="$(mktemp)"
    trap 'rm -f "$NOTES"' EXIT
    LAST="$(git describe --tags --abbrev=0 --match 'v*' "$COMMIT")"
    {
        # The dev icon on top (resources/icons/app/kinoklip-dev.aseprite), from the dev build's commit.
        echo "<img src=\"https://raw.githubusercontent.com/SeniorMatt/kinoklip/$COMMIT/resources/icons/app/kinoklip-dev-128.png\" width=\"96\" alt=\"Kinoklip Dev icon\">"
        echo
        echo "**Development build, not tested.** $STAMP, built $(date -u +%F)."
        echo
        echo "For trying new work before the next release. It can break things: keep backups of"
        echo "your projects. Open \`KinoklipDev-x86_64.AppImage\` with Gear Lever and it updates to"
        echo "every new dev build with one click. Release copies of Kinoklip never update to dev builds."
        echo
        echo "Changes since $LAST:"
        git log --no-merges --format='- %s' "$LAST..$COMMIT" | cut -c1-200
    } > "$NOTES"
fi

git tag -f "$TAG" "$COMMIT" >/dev/null
git push -q -f origin "refs/tags/$TAG"

if gh release view "$TAG" >/dev/null 2>&1; then
    gh release edit "$TAG" --title "Kinoklip Dev build ($COMMIT)" --notes-file "$NOTES" \
        --prerelease --latest=false --draft=false >/dev/null
else
    gh release create "$TAG" --verify-tag --title "Kinoklip Dev build ($COMMIT)" \
        --notes-file "$NOTES" --prerelease --latest=false >/dev/null
fi
# The AppImage first, the .zsync last: an updater that reads the new .zsync finds the new file.
gh release upload "$TAG" "$IMAGE" --clobber
gh release upload "$TAG" "$OUT/SHA256SUMS-dev.txt" "$IMAGE.zsync" --clobber
# Files of older dev schemes (Kinoklip-dev-<version>-<commit>-...) do not belong here.
gh release view "$TAG" --json assets -q '.assets[].name' | { grep -E '^Kinoklip-dev-' || true; } | while read -r a; do
    gh release delete-asset "$TAG" "$a" -y
done

log "published $STAMP: https://github.com/SeniorMatt/kinoklip/releases/tag/$TAG"
