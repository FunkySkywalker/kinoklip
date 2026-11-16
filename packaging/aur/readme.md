# Arch User Repository: kinoklip-bin

`kinoklip-bin/` holds the files of the AUR package `kinoklip-bin`: the release AppImage, extracted
to `/opt/kinoklip`, with a `/usr/bin/kinoklip` launcher, the menu entry, MIME type, icons and
licence. It is not published yet.

## Test it before publishing

Needs an Arch system or container; `makepkg` must not run as root.

```sh
cd packaging/aur/kinoklip-bin
makepkg -f          # downloads the AppImage, checks its sha256, builds the package
namcap PKGBUILD *.pkg.tar.zst
sudo pacman -U kinoklip-bin-*.pkg.tar.zst
kinoklip --version
```

## Publish the first time

1. Make an account on https://aur.archlinux.org and add an SSH public key to the profile.
2. Clone the empty package repository, copy the files, make `.SRCINFO` and push:

```sh
git clone ssh://aur@aur.archlinux.org/kinoklip-bin.git aur-kinoklip-bin
cp packaging/aur/kinoklip-bin/PKGBUILD aur-kinoklip-bin/
cd aur-kinoklip-bin
makepkg --printsrcinfo > .SRCINFO
git add PKGBUILD .SRCINFO
git commit -m "kinoklip-bin 1.9.0"
git push
```

## Update on each release

1. Download the release checksums: `gh release download vX.Y.Z -p SHA256SUMS.txt`.
2. In `PKGBUILD` set `pkgver=X.Y.Z`, `pkgrel=1`, and put the two checksums of
   `Kinoklip-x86_64.AppImage` and `Kinoklip-aarch64.AppImage` into `sha256sums_x86_64` and
   `sha256sums_aarch64`.
3. Test with `makepkg -f`, regenerate `.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO`.
4. Copy `PKGBUILD` and `.SRCINFO` into the AUR clone, commit and push. Keep a copy of both in this
   folder so the repository stays the source of truth.

The Nix flake has the same version and checksums in `packaging/nix/appimage.nix`.
