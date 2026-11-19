#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# dev_build.sh -- a dev AppImage for trying new work before a release, built on this computer:
# dist/dev/KinoklipDev-x86_64.AppImage (+ .zsync); scripts/publish_dev.sh puts it on GitHub.
# A copy named Kinoklip-dev-<version>-<commit>-x86_64.AppImage is kept for history.
#
# Usage: scripts/dev_build.sh [--quick | --no-tests] [--force]
#   (default)    build, run the `full` test group, package
#   --quick      run the `quick` test group instead
#   --no-tests   skip the tests
#   --force      package even if tests fail
#
# Steps: build build-arch-release in the kk-arch container, run scripts/test.sh, package with
# scripts/package_linux.sh (KK_DEV_BUILD: the dev channel's update information, so Gear Lever
# updates a dev copy from the "dev" pre-release only; --version and Help > About show
# "<version> dev (<commit>)"), run scripts/check_bundle.sh, keep the newest 3 history copies.
# The release build directory is not reconfigured: the dev stamp is a file in the bundle.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT="$PWD"
BUILD=build-arch-release
OUT="$ROOT/dist/dev" # dist/ is gitignored
KEEP=3

GROUP=full
FORCE=0
for arg in "$@"; do
    case "$arg" in
    --quick) GROUP=quick ;;
    --no-tests) GROUP="" ;;
    --force) FORCE=1 ;;
    -h | --help)
        sed -n '3,17p' "$0"
        exit 0
        ;;
    *)
        echo "unknown option: $arg" >&2
        exit 2
        ;;
    esac
done

log() { echo "dev_build: $*" >&2; }

COMMIT="$(git rev-parse --short=7 HEAD)"
git diff --quiet HEAD -- 2>/dev/null || COMMIT="$COMMIT-dirty"

log "building $BUILD"
podman exec -w "$PWD" kk-arch cmake --build "$BUILD" -j4

if [ -n "$GROUP" ]; then
    log "tests: $GROUP"
    if ! scripts/test.sh "$GROUP"; then
        if [ "$FORCE" = 1 ]; then
            log "tests failed, going on (--force)"
        else
            log "tests failed, no package made (--force packages anyway)"
            exit 1
        fi
    fi
fi

log "packaging $COMMIT"
mkdir -p "$OUT"
podman exec -w "$PWD" -e KK_BUILD_DIR="$BUILD" -e KK_DEV_BUILD="$COMMIT" -e KK_DIST_DIR="$OUT" \
    kk-arch scripts/package_linux.sh --no-build >&2
rm -rf "$OUT/kinoklip-x86_64" # the AppDir, only needed for the AppImage

IMAGE="$OUT/KinoklipDev-x86_64.AppImage"
[ -f "$IMAGE" ] || { echo "dev_build: no AppImage made" >&2; exit 1; }
scripts/check_bundle.sh "$IMAGE" >&2
VERSION="$("$IMAGE" --version 2>/dev/null | tail -n1 | awk '{print $2}')"
cp --reflink=auto "$IMAGE" "$OUT/Kinoklip-dev-$VERSION-$COMMIT-x86_64.AppImage"

# keep the newest $KEEP
ls -t "$OUT"/Kinoklip-dev-*-x86_64.AppImage | tail -n +$((KEEP + 1)) | while read -r old; do
    log "removing $old"
    rm -f "$old" "$old.zsync"
done

echo "$IMAGE ($(du -h "$IMAGE" | cut -f1))"
