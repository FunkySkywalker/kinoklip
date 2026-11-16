{
  description = "Kinoklip: a video editor with the layout and shortcuts of DaVinci Resolve";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system);
      pkgsFor = system: import nixpkgs { inherit system; overlays = [ self.overlays.default ]; };
    in
    {
      overlays.default = final: prev: {
        kinoklip = final.callPackage ./packaging/nix/package.nix { src = self; };
        kinoklip-appimage = final.callPackage ./packaging/nix/appimage.nix { };
      };

      packages = forAllSystems (system:
        let pkgs = pkgsFor system; in
        {
          # Built from source with nixpkgs libraries.
          default = pkgs.kinoklip;
          kinoklip = pkgs.kinoklip;
          # The release AppImage from GitHub, run in an FHS sandbox (fallback).
          appimage = pkgs.kinoklip-appimage;
        });

      apps = forAllSystems (system: {
        default = {
          type = "app";
          program = "${(pkgsFor system).kinoklip}/bin/kinoklip";
          meta.description = "Kinoklip video editor";
        };
        appimage = {
          type = "app";
          program = "${(pkgsFor system).kinoklip-appimage}/bin/kinoklip";
          meta.description = "Kinoklip video editor (release AppImage)";
        };
      });

      devShells = forAllSystems (system:
        let pkgs = pkgsFor system; in
        {
          default = pkgs.mkShell {
            inputsFrom = [ pkgs.kinoklip ];
            packages = with pkgs; [ ccache gdb git python3 xvfb-run dbus ];
            # Needed when running the freshly built binary outside the package wrapper.
            shellHook = ''
              export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}:${pkgs.qt6.qtwayland}/${pkgs.qt6.qtbase.qtPluginPrefix}:${pkgs.qt6.qtsvg}/${pkgs.qt6.qtbase.qtPluginPrefix}:${pkgs.qt6.qtimageformats}/${pkgs.qt6.qtbase.qtPluginPrefix}"
              export LIBVA_DRIVERS_PATH="''${LIBVA_DRIVERS_PATH:-/run/opengl-driver/lib/dri}"
              echo "Kinoklip dev shell: cmake -S . -B build -G Ninja && cmake --build build --target kinoklip"
            '';
          };
        });

      formatter = forAllSystems (system: (pkgsFor system).nixpkgs-fmt);
    };
}
