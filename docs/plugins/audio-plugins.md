# LV2 and CLAP audio plugins

Kinoklip is a host for free audio effect plugins in two formats: [LV2](https://lv2plug.in) and
[CLAP](https://cleveraudio.org). They work like the built-in audio effects: they show up in the
Effects library (Audio FX, folders **LV2** and **CLAP**) and in the **Add effect** menu of a mixer
track on the Audio page, have their parameters in the Inspector, are saved in the project, can be
bypassed, moved and removed with undo, are heard while playing, and are part of every export and
mixdown. (OpenFX video effects: [openfx.md](openfx.md); Kinoklip's own plugin format:
[installing-plugins.md](installing-plugins.md).)

## Install a plugin

| Format | What it is | Folders Kinoklip looks in |
|---|---|---|
| LV2 | a folder ending in `.lv2` (with `manifest.ttl`) | `~/.local/share/Kinoklip/lv2` (Preferences > Plugins > Open LV2 Folder), `~/.lv2`, `/usr/lib/lv2`, `/usr/local/lib/lv2`, every folder in `LV2_PATH`, and `share/kinoklip/lv2` next to the program (what a build ships) |
| CLAP | a file ending in `.clap` | `~/.local/share/Kinoklip/clap` (Open CLAP Folder), `~/.clap`, `/usr/lib/clap`, `/usr/local/lib/clap`, every folder in `CLAP_PATH`, and `share/kinoklip/clap` next to the program |

Copy the folder or file into one of them (or install the package of your distribution: Arch
`lsp-plugins-lv2`, `lsp-plugins-clap`, `x42-plugins-lv2`, `zam-plugins-lv2`, `calf`; Debian and Ubuntu
`lsp-plugins-lv2`, `lsp-plugins-clap`, `x42-plugins`, `zam-plugins`, `calf-plugins`) and press
**Reload** in Preferences > Plugins. Folders one level down are scanned too, so a pack unpacked into a
folder of its own works. Kinoklip has no download button and never downloads plugins on its own.

**Flatpak**: the sandbox does not see `/usr/lib/lv2` of your system. Put plugins in
`~/.var/app/org.kinoklip.Kinoklip/data/Kinoklip/lv2` (or `.../clap`) or in `~/.lv2` / `~/.clap`
(the Flatpak can read your home folder). **AppImage**: it carries its own system libraries; a plugin
built against newer libraries than the AppImage has will not load and shows up as a bad plugin with
the reason.

## Which free plugins work

Checked 2026-11-25 on Arch Linux with the packages named below (the plugin binaries are separate
programs loaded through the plugin interface; Kinoklip does not link them). Free plugins of other
makers follow the same interfaces; Kinoklip does not check licences of what you install.

| Pack | Licence | LV2 | CLAP | Result |
|---|---|---|---|---|
| [LSP plugins](https://lsp-plug.in) 1.2.37 | LGPL-3.0-or-later | yes | yes | works: see the numbers below |
| [x42-plugins](https://x42-plugins.com) 20260908 | GPL-2.0-or-later / GPL-3.0-or-later | yes | no | works for its effects (fil4 equalizer, dpl limiter, ...); its meters pass the sound through and are offered too, its MIDI plugins and generators are listed as "not used" |
| [ZAM plugins](https://www.zamaudio.com) 4.5 | GPL-2.0-or-later | yes | yes | works |
| [Calf](https://calf-studio-gear.org) 0.90.9 | LGPL-2.0-or-later (some GPL-2.0-or-later) | yes | no | 48 of its 52 plugins instantiate (the others are instruments or analysers); not run as sound in the tests |
| [Airwindows](https://www.airwindows.com) ports | MIT | | | not tested (the Arch repositories have no Airwindows LV2 or CLAP package); any port that is an LV2 or CLAP effect follows the same rules |

Results of the checks are in "Tested plugins" at the end of this page.

## What you see

- **Effects library > Audio FX > LV2 / CLAP**: every usable plugin. Double-click or drag onto an
  audio clip; on the Audio page use a track's **Add effect** menu.
- **Inspector**: the plugin's own parameters: numbers with their range and unit (dB, Hz, ms, ...),
  switches, lists of choices, integers. Parameters of CLAP plugins have the names and groups the
  plugin gives them. A plugin's own window is not shown (see "Not supported").
- **Automation**: on a track, every number parameter of a plugin has an automation lane (Read,
  Touch, Latch, Trim) like the built-in effects. Clip effects take one value per block.
- **Preferences > Plugins**: each LV2 folder and CLAP file with its plugins and state, **Retry**
  for a bad plugin, **Use LV2 plugins** / **Use CLAP plugins** to switch a format off, **Open LV2
  Folder**, **Open CLAP Folder** and **Get Free Audio Plugins** (this page).
- A project that uses a plugin that is not installed on this computer keeps the effect with all its
  settings, the Inspector says "Missing plugin", and the sound goes through it unchanged (bypass).
  Install the plugin and it works again. The effect id is `audio_lv2:<URI>` or `audio_clap:<id>`,
  for example `audio_lv2:http://lsp-plug.in/plugins/lv2/compressor_stereo`.

## How a plugin is run

- Sound is 48 kHz stereo float. A **stereo** plugin gets both channels; a **mono** plugin is run
  twice (left, right); a mono-in / stereo-out plugin gets the sum of both channels; a stereo-in /
  mono-out plugin's output goes to both channels. Plugins with more than two channels get the first
  two and silence on the others; **side-chain** inputs get silence (the side chain of the
  built-in Compressor is a separate feature). Surround plugins are not supported.
- Plugin parameters change at the start of each block; a changing parameter (automation) makes
  Kinoklip hand the plugin 256 frame chunks. LV2: control ports are written into the port memory;
  CLAP: parameter value events. The plugin's own smoothing makes the changes click-free.
- **Latency**: LV2 plugins that report their latency (`lv2:reportsLatency` port) and CLAP plugins
  with the latency extension are compensated like the built-in Noise Reduction: the sound is read that
  many frames ahead, so the output lines up with the input. The first block after a seek feeds the
  plugin the lead-in first.
- **Tails**: reverbs and delays go on sounding after the clip ends. CLAP plugins tell their tail; for
  LV2 (which has no such information) Kinoklip assumes four seconds for plugins of the Reverb class
  and three for Delay and Echo plugins, none for others.
- **Real-time safety**: the host adds no allocation and takes no lock while it processes (the test
  `processingAllocatesNothing` counts the allocations of the host code for both formats). A plugin
  is created on the audio thread when its effect first plays or is edited, which loads its library
  (that allocates); a plugin's own allocations while it runs are its authors' business. Reset after
  a seek deactivates and activates the plugin, which some plugins do slowly.
- **Offline**: an export or mixdown runs the same code in blocks, so a range always sounds the same
  from the same start; the tests compare the output with a reference calculation for several block
  sizes.

## Bad plugins

A plugin is a program that runs inside Kinoklip, so a broken one could crash the editor. Kinoklip
protects you in two steps:

1. **At scan time** every LV2 folder and CLAP file is loaded, each plugin created and one block run
   by a separate program, `kinoklip-audio-probe`, in its own process (up to three at once). If that
   process crashes, hangs (60 s limit) or fails, the bundle goes on the bad-plugin list with the
   reason and Kinoklip never loads it itself. The result is cached (`~/.cache/Kinoklip/
   audio-plugin-scan.json`, keyed by the size and date of the files), so later starts are quick and only
   new or changed bundles are probed again.
2. **At play time** Kinoklip writes a mark before it loads a plugin for the first time and removes it
   after the plugin's first block. If Kinoklip died in between, the next start lists that bundle as
   bad ("Kinoklip crashed the last time this plugin was loaded").

Press **Retry** after updating a plugin (or **Reload** to look at new bundles only).

## Scanning in the background

Kinoklip starts with the plugins of the cache and probes new or changed bundles in the background
(OpenFX, LV2 and CLAP, up to three probes at a time), so no plugin makes the first window wait.
The Effects library updates when the answers come; Preferences > Plugins says "Looking at new
plugins in the background...". A project opened before that keeps its plugin effects (they go through
unchanged and start to work when the scan is done).

## Not supported (yet)

- Plugin windows (LV2 UIs, CLAP GUIs): every parameter has the same generic Kinoklip control.
- MIDI and instrument plugins (they have no audio input and are listed as "not used"), note ports,
  transport and tempo information, LV2 patch properties (parameters that are not control ports),
  the LV2 worker and state extensions, CLAP state and presets: a plugin that needs a host feature
  that Kinoklip does not offer is listed as "not used" with the feature's name.
- Side-chain inputs and outputs, surround channel layouts, more than one audio output bus.
- Windows and macOS builds of the formats (the hosts load Linux plugins).

## Build notes

LV2 needs [lilv](https://drobilla.net/software/lilv) (ISC) with serd, sord, sratom, zix and the LV2
headers (ISC / MIT-style, all small): Arch `lilv lv2`, Debian `liblilv-dev lv2-dev`. Without them
`KK_WITH_LV2` finds nothing and the build has CLAP only (the Plugins page says so). CLAP needs only
its headers, which are in `third_party/clap` (MIT). The AppImage carries lilv and a copy of the
LV2 specifications (`share/kinoklip/lv2-specs`); the Flatpak builds them as modules.

## For plugin developers

`tests/audioplug/kk_test_lv2.cpp` and `kk_test_clap.cpp` are complete small plugins (a gain with a
switch, one that aborts while loading, a delay that reports its latency) and a good starting point
to test a plugin against Kinoklip. `kinoklip-audio-probe lv2|clap <bundle> out.json` shows what the
scan sees.

## Tested plugins

See spec 159 in [behavior-spec.md](../behavior-spec.md) for the checks that run in the tests, and:

Probed with `kinoklip-audio-probe` on 2026-11-25 (every bundle of the Arch packages; "usable" = created,
activated and one block run without a crash; the rest are listed on the Plugins page as "not used"):

| Pack (package version) | Licence | Size installed (download) | LV2 | CLAP |
|---|---|---|---|---|
| LSP plugins 1.2.37 | LGPL-3.0-or-later | LV2 40 MB (4.8 MB), CLAP 14 MB (3.8 MB) | 198 of 198 plugins usable | 198 of 198 usable |
| ZAM plugins 4.5 | GPL-2.0-or-later | LV2 35 MB (8.1 MB), CLAP 34 MB (8.2 MB), 19 plugins | 17 usable; ZamVerb and ZamHeadX2 need the system library `libzita-convolver` (not in the test container) | the same two need it |
| x42-plugins 20260908 | GPL-2.0-or-later and GPL-3.0-or-later | LV2 5.5 MB (1.2 MB) | fil4 (equalizer), dpl (limiter), darc, fat1, nodelay, phaserotate, stereoroute, xfade, tuna, balance and the meter plugins usable; the MIDI plugins and generators are "not used: no audio input and output"; `mixtri` needs `libltc`; `zeroconvo` and `midimap` need the LV2 worker / state | none |
| Calf 0.90.9 | LGPL-2.0-or-later, GPL-2.0-or-later | LV2 20 MB (12.7 MB), needs `libfluidsynth` | 48 of 52 usable (the other 4 are instruments or analysers) | none |

What ran as a test (`tests/audioplug/test_audioplug.cpp`, `theFreePacksWork`): LSP Compressor, Limiter
and Parametric Equalizer plugins (LV2 and CLAP), ZAM Compressor and Tube (LV2 and CLAP) and x42 fil4 and
dpl (LV2) were processed with noise in 512 frame blocks at their defaults and with every parameter at its
maximum: finite, non-silent output. ZamEQ2 (LV2 and CLAP) produced silence in that check (its own
defaults and the maximum settings); the reason was not investigated and the plugin is not in the test.

**Bundling in the dev AppImage**: not done. The LSP pack alone is 38 MB for all formats in its upstream
release (4 MB packed for the CLAP file of the Arch package, 14 MB installed), more than the modest size
asked for, and users install these packs through their distribution anyway. `scripts/package_linux.sh`
takes `KK_LV2_BUNDLE_DIR` / `KK_CLAP_BUNDLE_DIR` (folders with `*.lv2` / `*.clap`) and
`KK_AUDIO_PLUGIN_LICENSES` (licence texts, copied to `doc/kinoklip/licenses`) if you want a build that
carries a pack; LSP's LGPL-3.0-or-later and the GPL-2.0-or-later packs are compatible with Kinoklip's
GPL-3.0-or-later as separate binaries loaded through the plugin interfaces.
