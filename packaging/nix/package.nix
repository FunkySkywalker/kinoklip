# Kinoklip built from source with nixpkgs libraries (flake.nix: packages.default).
{ lib
, stdenv
, src
, cmake
, ninja
, pkg-config
, python3
, ccache
, qt6
, kdePackages
, ffmpeg
, libheif
, libraw
, onnxruntime
, libva
, libGL
, libxkbcommon
, wayland
, noto-fonts-color-emoji
}:

let
  # The version is read from project(... VERSION x.y.z) in CMakeLists.txt, the single source.
  cmakeText = builtins.readFile (src + "/CMakeLists.txt");
  version = builtins.head (builtins.match "[\\s\\S]*project\\(\\$\\{APP_NAME\\} VERSION ([0-9.]+) [\\s\\S]*" cmakeText);
in
stdenv.mkDerivation {
  pname = "kinoklip";
  inherit version src;

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    python3 # translations: scripts/ts_fallback.py
    qt6.wrapQtAppsHook
    qt6.qttools # lrelease
  ];

  buildInputs = [
    qt6.qtbase
    qt6.qtmultimedia
    qt6.qtsvg
    qt6.qtwayland
    qt6.qtimageformats
    kdePackages.kcolorscheme
    kdePackages.kiconthemes
    kdePackages.breeze
    kdePackages.breeze-icons
    ffmpeg
    libheif
    libraw
    onnxruntime
    libva
    libGL
    libxkbcommon
    wayland
  ];

  cmakeFlags = [
    (lib.cmakeBool "KK_USE_KF6" true)
    (lib.cmakeBool "KK_WITH_ONNXRUNTIME" true)
    (lib.cmakeBool "KK_WITH_LIBHEIF" true)
    (lib.cmakeBool "KK_WITH_LIBRAW" true)
    (lib.cmakeFeature "CMAKE_BUILD_TYPE" "Release")
    # no ccache in the sandbox
    (lib.cmakeFeature "CMAKE_CXX_COMPILER_LAUNCHER" "")
  ];

  # Only the application: every tests/test_*.cpp is an executable of its own and the tests
  # need a display, so they are not built here (the repo's CI and docs/building.md run them).
  ninjaFlags = [ "kinoklip" ];
  doCheck = false;

  installPhase = ''
    runHook preInstall
    install -Dm755 kinoklip $out/bin/kinoklip
    install -Dm644 $src/packaging/linux/org.kinoklip.Kinoklip.desktop \
      $out/share/applications/org.kinoklip.Kinoklip.desktop
    install -Dm644 $src/packaging/linux/org.kinoklip.Kinoklip.xml \
      $out/share/mime/packages/org.kinoklip.Kinoklip.xml
    sed -e "s/@VERSION@/${version}/" -e "s/@DATE@/$(date -u +%Y-%m-%d -d @$SOURCE_DATE_EPOCH)/" \
      $src/packaging/linux/org.kinoklip.Kinoklip.metainfo.xml \
      > org.kinoklip.Kinoklip.metainfo.xml
    install -Dm644 org.kinoklip.Kinoklip.metainfo.xml \
      $out/share/metainfo/org.kinoklip.Kinoklip.metainfo.xml
    for size in 16 24 32 48 64 128 256 512; do
      install -Dm644 $src/resources/icons/app/kinoklip-$size.png \
        $out/share/icons/hicolor/''${size}x''${size}/apps/org.kinoklip.Kinoklip.png
    done
    runHook postInstall
  '';

  # The translations and the colour schemes are compiled into the binary (Qt resources).
  # AI models are downloaded on first use into the user's data directory. The emoji font is
  # looked up in ../share/kinoklip/fonts, so it is linked in. VAAPI and OpenGL drivers come
  # from the host: /run/opengl-driver on NixOS (hardware.graphics.enable = true).
  postInstall = ''
    mkdir -p $out/share/kinoklip/fonts
    ln -s "$(find ${noto-fonts-color-emoji} -name NotoColorEmoji.ttf | head -n1)" \
      $out/share/kinoklip/fonts/NotoColorEmoji.ttf
  '';

  qtWrapperArgs = [
    "--set-default LIBVA_DRIVERS_PATH /run/opengl-driver/lib/dri"
    "--suffix LD_LIBRARY_PATH : /run/opengl-driver/lib"
  ];

  meta = {
    description = "Video editor with the layout and shortcuts of DaVinci Resolve";
    homepage = "https://github.com/SeniorMatt/kinoklip";
    license = lib.licenses.gpl3Plus;
    platforms = [ "x86_64-linux" "aarch64-linux" ];
    mainProgram = "kinoklip";
  };
}
