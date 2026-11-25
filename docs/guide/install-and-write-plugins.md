# Install and write plugins

Plugins add effects, transitions, presets and scripts to Kinoklip. Installed plugins work like the
built-in ones: they are in the Effects library, have parameters in the Inspector, can be
keyframed, undone, saved in the project and rendered.

## Install a plugin

1. Open **Workspace > Plugins...** (or Edit > Preferences > Plugins).
2. Press **Install...** and choose the `.kkplugin` file, or **Install Folder...** for an unpacked
   plugin. You can also drop a `.kkplugin` file onto the Kinoklip window.
3. The plugin's effects and transitions are now in the Effects library (the folder the author
   chose, or "Plugins"); its presets are under **Presets** at the top of the library; its scripts
   are in **Workspace > Scripts**.

Untick a plugin in the list to switch it off, **Uninstall** deletes it, **Reload** reads the plugin
folders again (a plugin author's edited shader shows at once), **Open Plugin Folder** shows where
plugins live (`~/.local/share/Kinoklip/plugins`). A project that uses a plugin you do not have
keeps the effect and its settings: the Inspector says "Missing plugin" and the picture is drawn
without it.

## Presets

Drag a preset onto a clip, or select clips and double-click the preset. A preset can add effects
and keyframes (animations such as "Fade In"), always as one undo step.

## Scripts

A script is a program that runs on your computer, so a plugin with scripts is installed switched
off. Select it in Preferences > Plugins, read what it asks to do and tick its box. Then choose the
script in Workspace > Scripts.

## Write your own

A plugin is a folder with a text file `plugin.json` and a GLSL shader. The quickest start:

```sh
kinoklip plugin create --id com.example.warm --name "Warm Look" --kind effect --entry color \
    --param "amount:number:0:100:50:Amount"
```

This writes a working plugin and installs it. Edit the `.glsl` file it names, press Reload, and
check it with `kinoklip plugin validate --target com.example.warm` and
`kinoklip plugin render_preview --plugin com.example.warm --file preview.png`. An AI assistant can
do the same through the MCP server
([Control Kinoklip from an AI assistant](control-kinoklip-from-an-ai-assistant.md)). The complete
reference, with examples, is [docs/plugins/writing-plugins.md](../plugins/writing-plugins.md); a
short prompt for an assistant is [docs/plugins/llms.txt](../plugins/llms.txt).
