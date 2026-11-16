# Building Kinoklip

## Primary: the Arch Linux container (Qt 6.12, KDE Frameworks 6, Breeze)

The real KDE Breeze look (Breeze widget style, KDE colour schemes, Breeze icons; [docs/archive/plan.md](archive/plan.md) section 4)
needs Qt >= 6.6 and KDE Frameworks 6, so the main toolchain is the podman container `kk-arch`
(Arch Linux: latest Qt 6, KF6, Breeze, FFmpeg, clang; the repository is mounted at the same
path). Packages it needs: `base-devel cmake ninja qt6-base qt6-multimedia qt6-svg
qt6-multimedia-ffmpeg ffmpeg kcolorscheme kiconthemes breeze breeze-icons libheif libraw
patchelf zstd xorg-server-xvfb clang`.

```sh
podman exec -w "$PWD" kk-arch sh -c 'cmake -S . -B build-arch -G Ninja && cmake --build build-arch -j4'
podman exec -w "$PWD" kk-arch sh -c 'QT_QPA_PLATFORM=offscreen ctest --test-dir build-arch -j4'
podman exec -w "$PWD" kk-arch ./build-arch/kinoklip [project.kkproj]     # needs a display, see below
```
CMake reports `KDE Frameworks 6 (Breeze colour schemes + icons): ON`. Release AppImage
(`dist/Kinoklip-x86_64.AppImage`) from the container: see [packaging/linux/readme.md](../packaging/linux/readme.md).

## Test groups

Every test has exactly one ctest label, set from the lists in `tests/groups.cmake`:

| Group | Contents | Command | Target time (-j2) |
| --- | --- | --- | --- |
| `quick` | seconds-long logic and UI tests: core, editops, timeline, serialization, parity, vim, panels | `scripts/test.sh quick` (`ctest -L quick`) | about 3 minutes |
| `full` | quick plus slower tests that are not real-time (whole-window pages, stabilizer, process-level, render) | `scripts/test.sh full` (`ctest -LE release`) | about 7 minutes |
| `release` | everything, including real-time playback, speed and timing tests (test_cutperf, test_keyframecut, test_pageturnplayback, test_cutlookahead, test_editplaying, soak, anything over a minute) | `scripts/test.sh release` (plain `ctest`) | about 25 minutes, quiet machine |

Day to day: `scripts/test.sh quick`. Before a commit that touches shared code: `full`. Before a
release: `release` on a quiet machine (the timing tests fail under load), plus the leak checks in
[contributing.md](contributing.md). `scripts/test.sh` takes more ctest arguments
(`scripts/test.sh quick -R timeline`); `KK_JOBS` sets the job count (default 2).

## Local dev builds

`scripts/dev_build.sh` makes a test AppImage on this computer, for trying new work before a
release (nothing goes to GitHub): it builds `build-arch-release` in the container, runs a test
group, packages and checks the bundle, and prints the path and size of
`dist/dev/Kinoklip-dev-<version>-<commit>-x86_64.AppImage`. The newest 3 are kept.

| Option | Effect |
| --- | --- |
| (none) | build, `full` tests, package |
| `--quick` | `quick` tests instead of `full` |
| `--no-tests` | skip the tests |
| `--force` | package even if tests fail |

A dev AppImage carries no update information and no `.zsync`, so it never updates itself to or
from the release channel. `--version` and Help > About show `<version> dev (<commit>)`
(`-dirty` after the commit when tracked files have uncommitted changes). The stamp is a file in
the bundle (`usr/share/kinoklip/dev-build`, written when `package_linux.sh` runs with
`KK_DEV_BUILD=<commit>`), so the release build directory is never reconfigured and
`scripts/package_linux.sh` without it makes the release package as before.

A new test must be added to one of the three lists in `tests/groups.cmake`; the ctest
`check_test_groups` fails for a test in no list or in two. Put a test in `release` when it plays
video in real time or asserts on speed or timing; in `full` when it runs longer than about 5 s;
otherwise in `quick`.

## Fallback: host toolchain without KF6 (e.g. Linux Mint 22: Qt 6.4.2, FFmpeg 6.1)

Requirements: CMake >= 3.24, Ninja, a C++20 compiler, Qt 6.4+ (Core, Gui, Widgets, Multimedia,
Svg, OpenGL, OpenGLWidgets, Concurrent, Test), FFmpeg 6 development libraries, pkg-config.
Without KF6 the app uses Fusion (or a host Breeze) with palettes read from the bundled KDE
colour scheme files, and Breeze icons only if the host has them installed.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo   # -DKK_USE_KF6=OFF forces the fallback
cmake --build build -j4
ctest --test-dir build --output-on-failure      # tests run with QT_QPA_PLATFORM=offscreen
./build/kinoklip [project.kkproj]
```

Useful options:
```sh
./build/kinoklip --reset-layout                     # ignore the saved window layout
QT_QPA_PLATFORM=offscreen ./build/kinoklip --menu-reference docs/menu-reference.md   # rewrite the menu and panel reference (scripts/menu-reference.sh)
./build/kinoklip --color-scheme BreezeLight         # colour scheme for this run (or "system")
./build/kinoklip --widget-style Fusion              # widget style for this run
QT_QPA_PLATFORM=offscreen ./build/kinoklip --screenshot edit out.png [project.kkproj]
#   targets: media edit deliver preferences keyboard inspector mediapool menu-color-theme menu-style
KK_NO_OPENGL=1 ./build/kinoklip                     # force the raster viewer fallback
```

### Command line (no window)

`kinoklip <command> --option value ...` runs one command of the command layer
(`src/app/commands/`) headless, on Qt's offscreen platform (no display needed), and exits.
`--json` prints the result as JSON; exit code 0 = done, 1 = the command failed, 2 = wrong command
line; messages and progress go to stderr. The window opens only when the first argument is not a
command (a project file, an option, nothing). Guide: [docs/guide/control-kinoklip-from-the-command-line.md](guide/control-kinoklip-from-the-command-line.md).
```sh
./build/kinoklip commands [--json]                  # every command (with --json: every option)
./build/kinoklip help clip add                      # options of one command
./build/kinoklip project new --project demo.kkproj --fps 25
./build/kinoklip media import --project demo.kkproj --file a.mp4
./build/kinoklip clip add --project demo.kkproj --media a.mp4
./build/kinoklip render --project demo.kkproj --output demo.mp4 --preset youtube_1080p
```
Commands: version; project new / info; media import / list / relink; timeline list / new /
select; clip list / add / move / trim / split / delete / set; marker add / list / delete;
transition add / set / remove; effect add / stack / set / remove / enable / list; title add / set;
generator add; keyframe set / list / delete; track list / add / set; export otio / edl; import otio;
frame export / view; transition list; session info; project save; render. `--live` after a command runs it in the
open window (Preferences > Local Control). `kinoklip mcp` serves the same commands to an AI
assistant (MCP) on stdin / stdout, see [docs/guide/control-kinoklip-from-an-ai-assistant.md](guide/control-kinoklip-from-an-ai-assistant.md). The older options above
(`--render`, `--render-frame`, `--screenshot`, ...) are unchanged. Tests: `test_commands` (the
layer), `test_cli` (the built binary as a process) and `test_livecontrol` (the open window).

## Nix / NixOS

`flake.nix` builds the application from source with nixpkgs' Qt 6, KDE Frameworks 6, FFmpeg,
ONNX Runtime, libheif and LibRaw (`packaging/nix/package.nix`; the version is read from
`CMakeLists.txt`). Only the application is built, not the tests.

```sh
nix build .#default                 # ./result/bin/kinoklip
nix build .#appimage                # the release AppImage in an FHS sandbox (packaging/nix/appimage.nix)
nix run . -- --version
nix develop                         # shell with cmake, ninja, Qt, FFmpeg, ONNX Runtime, ...
#   then: cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build -L quick
```

Use it from a NixOS configuration:

```nix
{
  inputs.kinoklip.url = "github:SeniorMatt/kinoklip";
  # ...
  environment.systemPackages = [ inputs.kinoklip.packages.${pkgs.system}.default ];
  # or: nixpkgs.overlays = [ inputs.kinoklip.overlays.default ];  then pkgs.kinoklip
  hardware.graphics.enable = true;   # OpenGL and VAAPI drivers (/run/opengl-driver)
}
```

On each release, update the version and the two hashes in `packaging/nix/appimage.nix`
(`nix hash convert --hash-algo sha256 --to sri <sha256 from SHA256SUMS.txt>`). The workflow
`.github/workflows/nix.yml` checks the flake on x86_64 and ARM64 runners.

## Build the website locally
The website (GitHub Pages) is plain HTML made from `website/`, `docs/guide/` and `docs/releases/`.
It needs Python 3 and the `markdown` module.

```sh
python3 scripts/build_website.py --check   # writes _site/ and checks every link, image and tag
python3 -m http.server -d _site            # then open http://localhost:8000/
```
