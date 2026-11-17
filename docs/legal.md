# Legal

- **Independent project.** Kinoklip is not affiliated with, sponsored by or endorsed by Blackmagic
  Design. DaVinci Resolve, Fusion, Fairlight and Blackmagic Design are trademarks or registered
  trademarks of Blackmagic Design Pty Ltd. They are named here only to describe the workflow and
  shortcuts Kinoklip follows. Other names (Qt, KDE, FFmpeg, YouTube, Vimeo and others) are
  trademarks of their owners.
- **Clean room.** Kinoklip is written from general knowledge of how Resolve behaves and from public,
  open specifications. It contains no Blackmagic code, icons, images, fonts, presets, LUTs or
  documentation text, and Resolve is never installed, run or decompiled to build it. The rules and
  their limits are in [docs/clean-room.md](docs/clean-room.md); every asset and its licence is in
  [attribution.md](attribution.md).
- **Privacy.** Kinoklip collects and sends nothing about you: no telemetry, analytics or automatic
  crash reports. A bug report shares system info only when you choose to, see
  [docs/privacy.md](docs/privacy.md).
- **No warranty.** Kinoklip is provided "as is", without any warranty, as stated in the GPL
  (sections 15 and 16 of [license.md](license.md)).
- **Codecs and patents.** The AppImage includes FFmpeg with open-source codec libraries, so common
  formats work without extra downloads: H.264 (x264, OpenH264), H.265 / HEVC (x265), AAC (FFmpeg's
  own encoder), AV1 (dav1d, libaom, SVT-AV1), VP8 / VP9 (libvpx), Opus, Vorbis and MP3 (LAME).
  FFmpeg is built without its "non-free" option, so everything in the bundle can be shared under the
  GPL. Some of these formats, mainly H.264, H.265 and AAC, are covered by patents in some countries
  (for example the United States). Whether you need a patent licence to use them depends on where
  you live and what you do with them (personal or commercial use), and is your responsibility. This
  is general information, not legal advice. The full list of bundled libraries and their licences
  is in [attribution.md](attribution.md).

## Licence

The code is GPL-3.0-or-later, see [license.md](license.md). The Kinoklip name, icon and logos are
not under the GPL: forks must replace them, see [logo-license.md](logo-license.md). Release bundles
include GPL FFmpeg components; third-party notices are inside the bundle under
`usr/share/doc/kinoklip/`.
