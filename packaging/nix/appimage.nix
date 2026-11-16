# The release AppImage from GitHub, run through appimageTools (flake.nix: packages.appimage).
# Bump on every release: version and the two hashes (SHA256SUMS.txt of the release, as SRI:
# nix hash convert --hash-algo sha256 --to sri <hex>).
{ lib, stdenv, appimageTools, fetchurl }:

let
  pname = "kinoklip-appimage";
  version = "1.9.0";
  sources = {
    x86_64-linux = {
      name = "Kinoklip-x86_64.AppImage";
      hash = "sha256-XEs/GCsiUhZkD6YQlNAOty+Dn4jes1kst6Mt8Xfu1bY=";
    };
    aarch64-linux = {
      name = "Kinoklip-aarch64.AppImage";
      hash = "sha256-Lb1Qfc4wR32yhc9qHrUBl9typ2z4cB70KAOWzgyhhd4=";
    };
  };
  source = sources.${stdenv.hostPlatform.system}
    or (throw "kinoklip-appimage: unsupported system ${stdenv.hostPlatform.system}");
  src = fetchurl {
    url = "https://github.com/SeniorMatt/kinoklip/releases/download/v${version}/${source.name}";
    inherit (source) hash;
  };
  contents = appimageTools.extract { inherit pname version src; };
in
appimageTools.wrapType2 {
  inherit pname version src;

  # The AppImage carries its own libraries; the host GPU drivers must be visible inside the sandbox.
  extraPkgs = pkgs: [ pkgs.libva pkgs.libglvnd pkgs.mesa ];

  extraInstallCommands = ''
    ln -s $out/bin/${pname} $out/bin/kinoklip
    install -Dm644 ${contents}/usr/share/applications/org.kinoklip.Kinoklip.desktop \
      $out/share/applications/org.kinoklip.Kinoklip.desktop
    substituteInPlace $out/share/applications/org.kinoklip.Kinoklip.desktop \
      --replace-quiet 'Exec=kinoklip' 'Exec=${pname}' \
      --replace-quiet 'TryExec=kinoklip' 'TryExec=${pname}'
    cp -r ${contents}/usr/share/icons $out/share/icons
  '';

  meta = {
    description = "Kinoklip video editor, the official release AppImage";
    homepage = "https://github.com/SeniorMatt/kinoklip";
    license = lib.licenses.gpl3Plus;
    sourceProvenance = [ lib.sourceTypes.binaryNativeCode ];
    platforms = builtins.attrNames sources;
    mainProgram = "kinoklip";
  };
}
