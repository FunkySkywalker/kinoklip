#!/bin/sh
# Runs a ctest group in the Arch container (docs/building.md, "Test groups").
#   scripts/test.sh quick     seconds-long logic and UI tests (day to day)
#   scripts/test.sh full      quick + slower tests, no real-time playback / timing tests
#   scripts/test.sh release   everything, including real-time tests; run on a quiet machine
# Extra arguments go to ctest, e.g. scripts/test.sh quick -R timeline.
set -e
group=${1:-quick}
[ $# -gt 0 ] && shift
cd "$(dirname "$0")/.."
build=${KK_BUILD_DIR:-build-arch-release}
case $group in
  quick) sel="-L quick" ;;
  full) sel="-LE release" ;;
  release) sel="" ;;
  *) echo "usage: $0 [quick|full|release] [ctest args]" >&2; exit 2 ;;
esac
# shellcheck disable=SC2086
exec podman exec -w "$PWD/$build" -e QT_QPA_PLATFORM=offscreen kk-arch \
  ctest -j"${KK_JOBS:-2}" --output-on-failure $sel "$@"
