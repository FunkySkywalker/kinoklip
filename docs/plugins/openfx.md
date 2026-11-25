# OpenFX plugins

Kinoklip is an [OpenFX](https://openfx.readthedocs.io) host for video effects. Free OpenFX packs such
as [openfx-misc](https://github.com/NatronGitHub/openfx-misc) (the plugins of Natron) show up in the
Effects library, folder **OpenFX**, and work like built-in effects: Inspector, keyframes, undo,
project files, proxies, export, the Compositing page. (Native Kinoklip plugins are a separate thing:
[installing-plugins.md](installing-plugins.md).)

## Install a pack

An OpenFX plugin is a folder ending in `.ofx.bundle` with a binary at
`Contents/Linux-x86-64/<name>.ofx` (`Contents/Linux-arm-64/` on ARM64). Kinoklip looks for bundles in:

| Folder | Used for |
|---|---|
| `~/.local/share/Kinoklip/ofx` | your plugins (Preferences > Plugins > Open OpenFX Folder) |
| `/usr/OFX/Plugins` | the standard system folder (what a distribution package of a pack uses) |
| `~/.OFX/Plugins` | another common per-user folder |
| every folder in the environment variable `OFX_PLUGIN_PATH` (separated by `:`) | anything else |
| `share/kinoklip/ofx` next to the program | bundles shipped with Kinoklip (none yet) |

Copy the bundle folder into one of them and press **Reload** in Preferences > Plugins. Distributions
package some packs; on Arch Linux and Debian-based systems look for `openfx-misc`. Kinoklip has no
download button for packs and never downloads plugins on its own.

Folders one level down are scanned too, so a pack unpacked into a folder of its own works.

## Which free packs work

| Pack | Licence | Status in Kinoklip |
|---|---|---|
| [openfx-misc](https://github.com/NatronGitHub/openfx-misc) (about 100 filters, keyers, transforms, colour tools) | GPL-2.0-or-later | the first target; see "Tested" below for what was checked |
| [openfx-gmic](https://github.com/NatronGitHub/openfx-gmic) (G'MIC filters, 1200 plugins in one bundle) | dual: CeCILL-C or CeCILL v2.0 (the CeCILL v2.0 option is GPL-compatible); some community filters inside carry other licences | loads like any bundle; a Filter-context plugin works. Not tested here (its build takes G'MIC's source and a long compile) |
| openfx-arena (text, particles, ...) | GPL-2.0-or-later | not tested; plugins that need more than one input or the OpenGL suite do not run |
| Natron's other free packs, commercial plugins you own | their own | any plugin that follows the rules below can work; Kinoklip does not check licences of what you install |

Only the **Filter** context (one input, one output) and the **General** context where the only
required input is "Source" are used. Generators, transitions, retimers, paint plugins, plugins with
several required inputs, plugins that need OpenGL rendering or on-screen overlays are listed in
Preferences > Plugins with the reason ("not used: ...") and are not offered as effects.

## What you see

- **Effects library > OpenFX**: every usable plugin of every bundle.
- **Inspector**: the plugin's parameters, with keyframes. Numbers, integers, checkboxes, choices,
  colours (RGB / RGBA), points and sizes as X / Y numbers, text. A point or size is stored as a
  fraction of the clip size (0.5 / 0.5 is the centre) and given to the plugin in pixels. Groups,
  pages, buttons and custom parameters are not shown.
- **Preferences > Plugins**: each bundle with its plugins, versions and state. A bundle that is a
  **bad plugin** is listed with the reason and a **Retry** button. **Get Free Plugin Packs** opens
  this page.
- A project that uses a plugin which is not installed on this computer keeps the effect with all
  its settings and keyframes, the Inspector says "Missing plugin", and the picture is drawn
  without it. Install the plugin and the effect works again (the effect id is `ofx:<plugin
  identifier>`, e.g. `ofx:net.sf.openfx.GainPlugin`).

## Bad plugins

A plugin is a program that runs inside Kinoklip, so a broken one could crash the editor. Kinoklip
protects you in two steps:

1. **At scan time** every bundle is loaded and described by a separate program,
   `kinoklip-ofx-probe`, in its own process. If that process crashes, hangs (30 s limit) or fails,
   the bundle goes on the bad-plugin list with the reason and Kinoklip never loads it itself. The
   result of every scan is cached (`~/.cache/Kinoklip/ofx-scan.json`, keyed by the binary's size and
   date), so later starts are quick and only new or changed bundles are probed again.
2. **At render time** Kinoklip writes a mark before it loads a plugin for the first time and removes
   it after the plugin's first frame. If Kinoklip died in between, the next start lists that bundle
   as bad ("Kinoklip crashed the last time this plugin was loaded").

Press **Retry** after updating a plugin (or **Reload** to look at new bundles only).

## How effects are rendered

The picture of the clip (premultiplied, with the effects above in the stack applied) is read back
from the GPU as float RGBA, given to the plugin with OpenFX's bottom-left origin and premultiplied
alpha, rendered on the CPU, and uploaded again. Plugins that cannot do float pixels get 8 bit.

- Time is the clip-local frame; the frame rate is the timeline's. Keyframed parameters are read at
  the time the plugin asks for.
- **Render scale**: previews and proxies render smaller; Kinoklip passes the scale (OpenFX
  `renderScale`), the plugin scales its pixel values. A plugin that does not support
  multi-resolution may look different in a small preview than in an export.
- **Threads**: the plugin's own thread-safety flag is obeyed (unsafe plugins render one at a time,
  instance-safe ones one frame per instance, fully safe ones in parallel) and the OpenFX
  multi-thread suite splits the work over the CPU cores.
- Rendering happens on the render thread, never on the interface thread.
- A frame that fails is shown without the effect (bypass); the reason is in Preferences > Plugins.

## Not supported (yet)

- Transition, generator, retimer and paint contexts; several required inputs; mask inputs.
- On-screen controls (overlays) in the viewer; keyframe editing by the plugin; custom and
  parametric parameters; string-choice parameters.
- OpenGL rendering and OpenCL / CUDA / Metal suites (everything runs on the CPU).
- Tiled rendering and a region of definition larger than the clip: effects that grow the picture
  (blurs, glows) are clipped at the edge of the clip; effects that read other frames (temporal
  clip access) get the current frame.
- Windows and macOS binaries: the host loads Linux bundles (`Linux-x86-64`, `Linux-arm-64`).

## Build openfx-misc yourself

```
git clone --recurse-submodules https://github.com/NatronGitHub/openfx-misc.git
cd openfx-misc
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_CIMG_PLUGINS=OFF -DBUILD_SHADERTOY=OFF
cmake --build build -j4
cp -r build/Plugins/Misc.ofx.bundle ~/.local/share/Kinoklip/ofx/
```

## For plugin developers

The host is `src/ofx/OfxHost.*` (OpenFX 1.4 / 1.5 headers, `third_party/openfx`, BSD-3-Clause);
`tests/ofx/kk_test_ofx.cpp` is a complete small plugin (gain, invert, choice, point, colour, integer,
text) written against the C API only, a good starting point to test a plugin against Kinoklip.
Messages a plugin posts through the OpenFX message suite go to the Kinoklip log (category
`kk.ofx`; `QT_LOGGING_RULES="kk.ofx*=true"`).
