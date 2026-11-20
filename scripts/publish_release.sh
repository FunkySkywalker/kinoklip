#!/bin/sh
# Publishes the AppImage in dist/ as the latest GitHub release. (The readme's "Download here" link
# points at the releases page, so it needs no update.) Run from the repository root after packaging:
#   scripts/publish_release.sh v1.1.0-beta.1 "Kinoklip 1.1.0 Beta"
# Before it: scripts/test.sh release (quiet machine), scripts/check_leaks.sh and `ctest -L soak` pass (docs/contributing.md, "Memory leaks").
# Afterwards it starts the ARM64 build (.github/workflows/arm64-release.yml, GitHub's ARM runner),
# which adds Kinoklip-aarch64.AppImage(.zsync) to the release; it needs the workflow on main.
# Needs: gh (logged in), dist/Kinoklip-x86_64.AppImage(.zsync), dist/SHA256SUMS.txt and
# docs/releases/<tag>.md.
set -eu
tag="$1"
title="$2"
notes="docs/releases/$tag.md"
[ -f "$notes" ] || { echo "missing $notes" >&2; exit 1; }
for f in dist/Kinoklip-x86_64.AppImage dist/Kinoklip-x86_64.AppImage.zsync dist/SHA256SUMS.txt; do
    [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
done
(cd dist && sha256sum -c SHA256SUMS.txt)

# The tag may already exist locally (made at the commit the AppImage was built from).
if ! git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    git tag -a "$tag" -m "$title"
fi
echo "Tag $tag -> $(git rev-list -n1 "$tag")"
git push origin "$tag"
# The page: a centred header (scripts/release_notes.sh) and the lists of docs/releases/<tag>.md.
page="$(mktemp)"
trap 'rm -f "$page"' EXIT
scripts/release_notes.sh "$tag" >"$page"
gh release create "$tag" --latest --title "$title" --notes-file "$page" \
    dist/Kinoklip-x86_64.AppImage dist/Kinoklip-x86_64.AppImage.zsync dist/SHA256SUMS.txt

# ARM64: only the release file is built on GitHub (free ARM runner); it is added to this release
# (with its .zsync and a line in SHA256SUMS.txt) when the workflow finishes, about an hour.
# Not an error if it cannot be started: run the same command later.
gh workflow run arm64-release.yml --ref main -f tag="$tag" ||
    echo "Could not start the ARM64 build; run: gh workflow run arm64-release.yml --ref main -f tag=$tag" >&2

# Flatpak (stable branch of https://seniormatt.github.io/kinoklip-flatpak/), built and published by
# GitHub Actions from the tag; a few hours at most. Not an error if it cannot be started.
gh workflow run flatpak.yml --ref main -f ref="$tag" -f branch=stable ||
    echo "Could not start the Flatpak build; run: gh workflow run flatpak.yml --ref main -f ref=$tag -f branch=stable" >&2

echo "Published $title: https://github.com/SeniorMatt/kinoklip/releases/tag/$tag"
