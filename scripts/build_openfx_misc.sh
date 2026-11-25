#!/usr/bin/env bash
# Builds the free OpenFX pack openfx-misc (GPL-2.0-or-later, Natron) from source, for the pack test
# of tests/ofx/test_ofx.cpp and for trying OpenFX plugins in Kinoklip (docs/plugins/openfx.md).
#
#   scripts/build_openfx_misc.sh <output dir> [jobs]
#
# Leaves <output dir>/Misc.ofx.bundle (Linux-x86-64 or Linux-arm-64 inside). Then
#   KK_OFX_MISC_DIR=<output dir> ctest -R test_ofx        # runs the pack test
#   cp -r <output dir>/Misc.ofx.bundle ~/.local/share/Kinoklip/ofx/
# Needs: git, cmake, ninja or make, a C++17 compiler, OpenGL and OpenMP headers (build-essential,
# libgl-dev). The CImg (G'MIC-style) plugins and Shadertoy are not built: they take much longer.
set -euo pipefail

OUT="${1:?usage: build_openfx_misc.sh <output dir> [jobs]}"
JOBS="${2:-2}"
# A fixed commit, so a CI run is repeatable (openfx-misc master, 2026-11-25).
COMMIT=a047cbda68fa24ecaf1dd3432b00eb261c287c38
SRC="$(mktemp -d)"
trap 'rm -rf "$SRC"' EXIT

git clone -q https://github.com/NatronGitHub/openfx-misc.git "$SRC/openfx-misc"
git -C "$SRC/openfx-misc" checkout -q "$COMMIT"
git -C "$SRC/openfx-misc" submodule update -q --init --depth 1
cmake -S "$SRC/openfx-misc" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_CIMG_PLUGINS=OFF -DBUILD_SHADERTOY=OFF >/dev/null
cmake --build "$SRC/build" -j"$JOBS"
mkdir -p "$OUT"
rm -rf "$OUT/Misc.ofx.bundle"
cp -r "$SRC/build/Plugins/Misc.ofx.bundle" "$OUT/"
# the licence travels with the binary (GPL-2.0-or-later; the OpenFX headers inside are BSD-3-Clause)
install -D -m 644 "$SRC/openfx-misc/LICENSE" "$OUT/Misc.ofx.bundle/Contents/Resources/LICENSE-openfx-misc.txt"
du -sh "$OUT/Misc.ofx.bundle"
