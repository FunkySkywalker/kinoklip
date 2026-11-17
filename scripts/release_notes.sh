#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# release_notes.sh -- prints the GitHub release page text for a release tag:
#   scripts/release_notes.sh v1.10.0 > notes.md
# A centred header (the app icon, the tagline, download links, guide / Discord / website) made
# here, then the lists of docs/releases/<tag>.md (### New, ### Improved, ### Fixed). Anything in
# that file before its first "### " heading (the old per-release download text) is left out.
# The dev page (scripts/publish_dev.sh) looks the same with the dev icon and a warning.
set -eu
tag="$1"
notes="docs/releases/$tag.md"
[ -f "$notes" ] || { echo "missing $notes" >&2; exit 1; }

repo="https://github.com/SeniorMatt/kinoklip"
raw="https://raw.githubusercontent.com/SeniorMatt/kinoklip/$tag/resources/icons/app"
dl="$repo/releases/download/$tag"

cat <<EOF
<p align="center"><img src="$raw/kinoklip-128.png" width="128" alt="Kinoklip"></p>
<p align="center">A free video editor for Linux that works like DaVinci Resolve</p>
<p align="center"><a href="$dl/Kinoklip-x86_64.AppImage"><b>Download for Linux</b></a> · <a href="$dl/Kinoklip-aarch64.AppImage">ARM64</a></p>
<p align="center"><a href="$repo/blob/main/docs/guide/readme.md">Guide</a> · <a href="https://discord.gg/pCeHnvDn3x">Discord</a> · <a href="https://seniormatt.github.io/kinoklip/">Website</a> · <a href="$repo/blob/main/docs/install.md">Install help</a></p>

> [!TIP]
> Open it with [Gear Lever](https://flathub.org/apps/it.mijorus.gearlever) and later versions update in one click.

EOF
awk 'found || /^### /{found=1; print}' "$notes"
