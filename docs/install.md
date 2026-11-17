# Installing Kinoklip

## AppImage (every distribution)

Download `Kinoklip-x86_64.AppImage` from the [latest release](https://github.com/SeniorMatt/kinoklip/releases/latest)
(on ARM64 computers: `Kinoklip-aarch64.AppImage`).

- **With [Gear Lever](https://flathub.org/apps/it.mijorus.gearlever):** open the file with it. Gear Lever adds
  Kinoklip to the app menu and updates it.
- **By hand:** right-click > Properties > "Is executable", then double-click it.

If double-clicking shows nothing, install `fuse3` (or `libfuse2` on older distributions), or run it with
`--appimage-extract-and-run`.

## Dev build (help with testing)

<p align="center"><img src="../resources/icons/app/kinoklip-dev-128.png" width="96" alt="Kinoklip Dev"></p>

<p align="center"><a href="https://github.com/SeniorMatt/kinoklip/releases/download/dev/KinoklipDev-x86_64.AppImage"><b>Download Kinoklip Dev</b></a></p>

The next version, built from the newest code before it is tested. It has the features that are not
released yet and may have new bugs, so keep backups of your projects. It installs next to the normal
Kinoklip as "Kinoklip Dev" with this red icon, and the two share their settings. Open it with Gear
Lever to get each new dev build in one click; the version shows the build, for example
`1.9.0-faea4fd`. The [dev release page](https://github.com/SeniorMatt/kinoklip/releases/tag/dev) lists
what is new.

Found a bug or something that works differently from Resolve? Please
[open an issue](https://github.com/SeniorMatt/kinoklip/issues/new/choose) and say which build you used.

## Nix / NixOS

The repository is a Nix flake (x86_64 and aarch64 Linux), built from source with nixpkgs libraries:

```sh
nix run github:SeniorMatt/kinoklip                  # try it
nix profile install github:SeniorMatt/kinoklip      # install it
nix run github:SeniorMatt/kinoklip#appimage         # the release AppImage instead
```

On NixOS, add it as a flake input and install it system-wide (`hardware.graphics.enable = true;`
gives it OpenGL and VAAPI):

```nix
{
  inputs.kinoklip.url = "github:SeniorMatt/kinoklip";
  # in the module list: ({ pkgs, ... }: {
  #   environment.systemPackages = [ inputs.kinoklip.packages.${pkgs.system}.default ];
  # })
}
```

## Arch Linux

An AUR package is on the way. Until then, use the AppImage, or build the package from
[packaging/aur/kinoklip-bin](../packaging/aur/kinoklip-bin) with `makepkg -si`.

## Building from source

See [building.md](building.md).
