#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# publish_dev.sh -- puts the dev build from scripts/dev_build.sh on GitHub as the rolling "dev"
# pre-release: https://github.com/SeniorMatt/kinoklip/releases/tag/dev
#
# Usage: scripts/publish_dev.sh [notes-file]
#        scripts/publish_dev.sh --notes-only   (rewrites the page text of the current dev release)
#        scripts/publish_dev.sh --from DIR     (GitHub Actions, .github/workflows/dev-build.yml: the
#                                               files come from DIR, the commit is already on GitHub)
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
TAG=dev

log() { echo "publish_dev: $*" >&2; }
NOTES_ONLY=
FROM_CI=
if [ "${1:-}" = --notes-only ]; then NOTES_ONLY=1; shift; fi
if [ "${1:-}" = --from ]; then
    [ -n "${2:-}" ] || { log "--from needs a directory"; exit 2; }
    OUT="$2"
    FROM_CI=1
    shift 2
    export APPIMAGE_EXTRACT_AND_RUN=1 # no FUSE on a CI runner
fi
IMAGE="$OUT/KinoklipDev-x86_64.AppImage"
[ -f "$IMAGE" ] && [ -f "$IMAGE.zsync" ] || { log "no $IMAGE (+ .zsync): run scripts/dev_build.sh"; exit 1; }

STAMP="$("$IMAGE" --version 2>/dev/null | tail -n1)" # "Kinoklip 1.9.0 dev (abc1234)"
COMMIT="$(sed -n 's/.*dev (\([0-9a-f]*\)).*/\1/p' <<<"$STAMP")"
[ -n "$COMMIT" ] || { log "cannot read the commit from: $STAMP"; exit 1; }
git cat-file -e "$COMMIT^{commit}" || { log "unknown commit $COMMIT"; exit 1; }
[ -n "$FROM_CI" ] || git merge-base --is-ancestor "$COMMIT" origin/main 2>/dev/null || {
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
        # Centred icon (resources/icons/app/kinoklip-dev.aseprite), name, download link; then the
        # changes users see (docs, packaging and script commits left out), up to the first ";".
        RAW="https://raw.githubusercontent.com/SeniorMatt/kinoklip/$COMMIT/resources/icons/app"
        DL="https://github.com/SeniorMatt/kinoklip/releases/download/$TAG/KinoklipDev-x86_64.AppImage"
        echo "<p align=\"center\"><img src=\"$RAW/kinoklip-dev-128.png\" width=\"128\" alt=\"Kinoklip Dev\"></p>"
        echo "<p align=\"center\">The next version, built $(date -u +%F) from <code>$COMMIT</code></p>"
        echo "<p align=\"center\"><a href=\"$DL\"><b>Download KinoklipDev-x86_64.AppImage</b></a></p>"
        echo
        echo "> [!WARNING]"
        echo "> Not tested yet. Keep backups of your projects."
        echo
        echo "Open it with Gear Lever once, and every new dev build arrives as a one-click update."
        echo
        echo "### New since ${LAST#v}"
        git log --no-merges --format='%s' "$LAST..$COMMIT" |
            { grep -vE '^(Docs|Notes|Nix|Readme|Roadmap|Menu reference|Release notes|Status|CI|Tests?|Dev channel|Dev icon|Dev builds|Dev release page|Release pages?|publish_dev|dev_build|package_linux|check_[a-z_]+|scripts?)\b' || true; } |
            sed -E 's/;.*//; s/ \([^)]*$//' | sed 's/^/- /'
    } > "$NOTES"
fi

if [ -n "$NOTES_ONLY" ]; then
    gh release edit "$TAG" --notes-file "$NOTES" >/dev/null
    log "page text rewritten"
    exit 0
fi

git tag -f "$TAG" "$COMMIT" >/dev/null
git push -q -f origin "refs/tags/$TAG"

if gh release view "$TAG" >/dev/null 2>&1; then
    gh release edit "$TAG" --title "Kinoklip Dev" --notes-file "$NOTES" \
        --prerelease --latest=false --draft=false >/dev/null
else
    gh release create "$TAG" --verify-tag --title "Kinoklip Dev" \
        --notes-file "$NOTES" --prerelease --latest=false >/dev/null
fi
# The AppImage first, the .zsync last: an updater that reads the new .zsync finds the new file.
gh release upload "$TAG" "$IMAGE" --clobber
gh release upload "$TAG" "$OUT/SHA256SUMS-dev.txt" "$IMAGE.zsync" --clobber
# Files of older dev schemes (Kinoklip-dev-<version>-<commit>-...) do not belong here.
gh release view "$TAG" --json assets -q '.assets[].name' | { grep -E '^Kinoklip-dev-' || true; } | while read -r a; do
    gh release delete-asset "$TAG" "$a" -y
done

# Flatpak nightly (https://seniormatt.github.io/kinoklip-flatpak/) from the same commit, built and
# published by GitHub Actions (the manifest comes from main). Not an error if it cannot be started.
gh workflow run flatpak.yml --ref main -f ref="$COMMIT" -f manifest_ref=main -f branch=nightly ||
    log "could not start the Flatpak nightly build"

log "published $STAMP: https://github.com/SeniorMatt/kinoklip/releases/tag/$TAG"
