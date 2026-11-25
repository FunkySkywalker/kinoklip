# Installing and using plugins

A plugin adds effects, transitions, presets or scripts to Kinoklip. Installed plugins look and
work like the built-in ones. How to write one: [writing-plugins.md](writing-plugins.md).

## Install

Any of these:

- **Preferences > Plugins > Install...** and choose a `.kkplugin` file (or **Install Folder...** for
  an unpacked plugin folder). Workspace > Plugins... (View > Plugins... in the Streamlined menu layout) opens the same page.
- **Drop** a `.kkplugin` file onto the Kinoklip window.
- **Copy the plugin folder** into your plugin folder and press **Reload** (Open Plugin Folder
  shows it; on Linux it is `~/.local/share/Kinoklip/plugins`).
- On the command line: `kinoklip plugin install --path warm.kkplugin`.

Installing never runs anything from the plugin. Kinoklip checks the manifest and the files first
and tells you what is wrong (with the file and the line) instead of installing a broken plugin.
Installing a plugin with the same id replaces the old version; its effects in your projects keep
their settings.

## Use

- **Effects** and **transitions** are in the Effects library with the others (in the folder the
  author chose, or "Plugins"): drag them onto a clip or a cut, or double-click. They have
  parameters in the Inspector, can be keyframed, undone, saved in the project, rendered and used
  with proxies, and they appear on the Compositing page like the built-in effects.
- **Presets** are in Effects library > Toolbox > Presets, grouped by plugin. Drag one onto a clip,
  or select clips and double-click it. A preset can add effects and keyframes (animations such as
  "Fade In" or "Pop In"); it is one undo step.
- **Scripts** are in Workspace > Scripts (View > Scripts in the Streamlined layout). A script is a program that runs on your computer, so a
  plugin with scripts is installed **switched off**. Open Preferences > Plugins, select it and tick
  its box: Kinoklip shows what it asks to do, and you decide. Choosing a script opens a window with
  its output and a Stop button.

## Manage

Preferences > Plugins lists the installed plugins, bundled ones marked. Untick a plugin to switch
it off (its effects stay on your clips but are skipped, the Inspector says "Missing plugin"); tick
it again to bring it back. **Uninstall** deletes a plugin you installed. **Reload** reads the plugin
folders again: a plugin author's edited shader shows at once. A plugin with a problem is listed
with "(problem)"; select it to read what is wrong.

## OpenFX plugins

Kinoklip also runs OpenFX video effects (the free packs of Natron, such as openfx-misc): copy a
`*.ofx.bundle` folder into `~/.local/share/Kinoklip/ofx` (or `/usr/OFX/Plugins`) and press Reload.
They appear in the Effects library, folder OpenFX. How, which packs work and what happens with a
plugin that crashes: [openfx.md](openfx.md). Free LV2 and CLAP audio effects (LSP, x42, ZAM, Calf ...)
work the same way on the Audio page: [audio-plugins.md](audio-plugins.md).

## Safety

- Effects and transitions are shaders: they run on the graphics card and cannot read your files or
  use the network.
- Presets only call Kinoklip's own editing commands, like the command line does.
- Scripts run with your rights. Enable only plugins from people you trust, and read the
  permissions shown. An AI assistant connected through MCP can create plugins but cannot switch on
  a plugin with scripts: only you can.
- Kinoklip does not download plugins and does not look for updates; it sends nothing anywhere.

## Missing plugins

If you open a project on another computer that does not have a plugin the project uses, nothing is
lost: the effect stays on the clip with all its settings and keyframes, the Inspector says
"Missing plugin", and the picture is drawn without it. Install the plugin and the effect works
again.

## For an AI assistant

[llms.txt](llms.txt) is a short prompt you can give an assistant to write plugins for you. With
Local Control switched on (Preferences > Local Control) the assistant can create a plugin, check
it, look at a picture of the result and put it on a clip, all through the commands in
[writing-plugins.md](writing-plugins.md).
