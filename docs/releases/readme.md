# Release notes

One file per release, `v<version>.md` (e.g. `v0.4.1-alpha.1.md`); its text is the GitHub
release description. Releases are normal GitHub releases (not pre-releases), so
`/releases/latest` points at the newest one; the rolling `dev` build is the only pre-release.

## Before publishing
Run the memory checks first ([contributing.md](../contributing.md#memory-leaks)):
`scripts/check_leaks.sh` (clean exit, no new suppression of our own code) and `ctest -L soak`
(passes, no QEXPECT_FAIL left). Then the whole test suite, then package.

## Assets to upload
- `Kinoklip-x86_64.AppImage`
- `Kinoklip-x86_64.AppImage.zsync` (Gear Lever and [AppImageUpdate](https://github.com/AppImageCommunity/AppImageUpdate) find updates through it;
  without it the update check fails)
- `SHA256SUMS.txt` (`sha256sum Kinoklip-x86_64.AppImage > SHA256SUMS.txt`)
- `Kinoklip-aarch64.AppImage` and `Kinoklip-aarch64.AppImage.zsync` (ARM64): not made locally.
  `scripts/publish_release.sh` starts the GitHub workflow `arm64-release.yml` after publishing; it
  builds on GitHub's ARM runner, adds both files to the release and appends their lines to
  `SHA256SUMS.txt`. Check the run finished (`gh run list --workflow arm64-release.yml`) before announcing.

Both made by `scripts/package_linux.sh` in the container, see
[packaging/linux/readme.md](../../packaging/linux/readme.md). Keep the file names without the
version: the update information inside the AppImage looks for `Kinoklip-*x86_64.AppImage.zsync`.

## Download links and the release page header
The release page text is made by `scripts/release_notes.sh <tag>`: a centred header (icon, tagline,
download links for x86_64 and ARM64, guide, Discord, website, install help) and then the `### New`,
`### Improved` and `### Fixed` lists of the release's file here. Do not put download text in the
files here; anything before the first `### ` heading is left out. The main [readme.md](../../readme.md)
and the website's download page use `/releases/latest/download/...`, so they need no change per release.

## Flatpak
`scripts/publish_release.sh` also starts the `stable` Flatpak build for the release tag
(`.github/workflows/flatpak.yml`), and `scripts/publish_dev.sh` starts `nightly`. Check the run
finished (`gh run list --workflow flatpak.yml`); the repository is
<https://seniormatt.github.io/kinoklip-flatpak/>.
