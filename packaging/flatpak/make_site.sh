#!/usr/bin/env bash
# Writes the static files of the Flatpak site (GitHub Pages, SeniorMatt/kinoklip-flatpak) into a
# directory: .flatpakrepo and .flatpakref files, the icon and index.html. Run by
# .github/workflows/flatpak.yml (publish job); the public GPG key is read from the key ring in
# $GNUPGHOME.
#   make_site.sh SITE_DIR FINGERPRINT
set -euo pipefail
site="$1"
fpr="$2"
root="$(cd "$(dirname "$0")/../.." && pwd)"
base="https://seniormatt.github.io/kinoklip-flatpak"
runtime_repo="https://dl.flathub.org/repo/flathub.flatpakrepo"
key="$(gpg --export "$fpr" | base64 -w0)"
[ -n "$key" ] || { echo "no public key for $fpr" >&2; exit 1; }
install -m 644 "$root/resources/icons/app/kinoklip-128.png" "$site/kinoklip.png"
touch "$site/.nojekyll"

repo_file() { # file title branch
    cat >"$site/$1" <<EOF
[Flatpak Repo]
Title=$2
Url=$base/repo/
Homepage=https://github.com/SeniorMatt/kinoklip
Comment=Kinoklip, a non-linear video editor
Description=Kinoklip, a non-linear video editor with the layout and shortcuts of DaVinci Resolve's Edit page ($3 branch)
Icon=$base/kinoklip.png
DefaultBranch=$3
GPGKey=$key
EOF
}
ref_file() { # file branch title remote
    cat >"$site/$1" <<EOF
[Flatpak Ref]
Title=$3
Name=org.kinoklip.Kinoklip
Branch=$2
Url=$base/repo/
SuggestRemoteName=$4
Homepage=https://github.com/SeniorMatt/kinoklip
Icon=$base/kinoklip.png
IsRuntime=false
RuntimeRepo=$runtime_repo
GPGKey=$key
EOF
}
repo_file kinoklip.flatpakrepo "Kinoklip" stable
repo_file kinoklip-nightly.flatpakrepo "Kinoklip nightly" nightly
ref_file org.kinoklip.Kinoklip.flatpakref stable "Kinoklip" kinoklip
ref_file org.kinoklip.Kinoklip-nightly.flatpakref nightly "Kinoklip (nightly)" kinoklip-nightly

cat >"$site/index.html" <<EOF
<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Kinoklip Flatpak</title>
<style>body{font:16px/1.5 sans-serif;max-width:42em;margin:2em auto;padding:0 1em}pre{background:#8882;padding:.6em;overflow:auto}</style>
</head><body>
<h1>Kinoklip Flatpak</h1>
<p>Unofficial Flatpak repository of <a href="https://github.com/SeniorMatt/kinoklip">Kinoklip</a>
(x86_64 and aarch64). Signed with GPG key <code>$fpr</code>.</p>
<h2>Stable (releases)</h2>
<pre>flatpak remote-add --user --if-not-exists kinoklip $base/kinoklip.flatpakrepo
flatpak install --user kinoklip org.kinoklip.Kinoklip</pre>
<p>Or open <a href="org.kinoklip.Kinoklip.flatpakref">org.kinoklip.Kinoklip.flatpakref</a> with your software centre.</p>
<h2>Nightly (development builds, untested)</h2>
<pre>flatpak remote-add --user --if-not-exists kinoklip-nightly $base/kinoklip-nightly.flatpakrepo
flatpak install --user kinoklip-nightly org.kinoklip.Kinoklip//nightly</pre>
<p>Or <a href="org.kinoklip.Kinoklip-nightly.flatpakref">org.kinoklip.Kinoklip-nightly.flatpakref</a>.</p>
<h2>Discover / GNOME Software</h2>
<p>Download <a href="kinoklip.flatpakrepo">kinoklip.flatpakrepo</a> (or
<a href="kinoklip-nightly.flatpakrepo">kinoklip-nightly.flatpakrepo</a>) and open it with the software centre
(Discover: Settings, Add Source), or use the commands above.</p>
<p>Start it with <code>flatpak run org.kinoklip.Kinoklip</code>.</p>
</body></html>
EOF
