# src/plugins - the plugin system

Plugins add effects, transitions, presets and scripts. User documentation: [docs/plugins](../../docs/plugins/installing-plugins.md);
how to write one: [writing-plugins.md](../../docs/plugins/writing-plugins.md); decisions and the plan for OpenFX / LV2 / CLAP:
[design.md](../../docs/plugins/design.md); behaviour: docs/behavior-spec.md section 156.

## Files

| File | What |
|---|---|
| `PluginManifest.*` | `plugin.json` as types (`PluginManifest`, `PluginEffectDef`, `PluginParam`, ...), `parseManifest()` (every problem with its path, JSON errors with the line), id and path rules, the permissions and their words |
| `PluginRegistry.*` | the singleton: scans the bundled and user folders, validates, installs (folder or `.kkplugin`, replacing, staging folder + rename), uninstalls, enables and disables (QSettings `plugins/enabled/<id>`; a plugin with scripts defaults to off), reloads (hot reload: registers descriptors in `EffectRegistry` and `NodeRegistry`, publishes immutable `PluginShaderSpec`s for the render thread, bumps `generation()` and `contentKey()`), lists presets and scripts of enabled plugins |
| `ZipArchive.*` | dependency-free zip reader (stored and deflate; names, sizes and checksums checked) and writer (stored) for `.kkplugin` files |
| `PresetRecipe.*` | a preset's recipe (list of command calls) with `$clip` / `$start` / `$end` / `$duration` / `$playhead` / `$last_effect` expanded for one target; the application runs the steps (`CommandRegistry::executeLiveBatch()`) |
| `ScriptRunner.*` | runs a plugin script as `python3 -I` with the bridge module `resources/plugin-sdk/kinoklip.py`; JSON lines over stdin / stdout to a command executor supplied by the application (which applies the script's permissions) |

The renderer side is in `src/render/GlPluginEffect.*` and `PluginShader.*`; the window side in
`src/app/PluginHost.*` and `src/ui/dialogs/PluginsPage.*`; the commands in `src/app/commands/PluginCommands.cpp`.

## Data flow

`PluginRegistry::reload()` -> for every enabled, valid plugin an `EffectDescriptor` (id `plugin:<plugin>/<item>`) goes into
`EffectRegistry` (a descriptor of a plugin that went away is marked `unavailable`, never removed, so projects keep their effects)
-> the Inspector, Effects library, keyframes, undo and project files work on descriptors -> `GlRenderer::effectFor()` finds no
built-in with that id and asks `makePluginEffect()` -> `PluginShaderEffect` compiles `PluginShader::composeEffect()` and sets one
uniform per parameter each frame.

## How to extend

- **A new parameter type**: add it to `ParamKind`, `parseParam()`, `toDescriptor()` (PluginRegistry.cpp), `setParam()` (GlPluginEffect.cpp),
  `paramUniforms()` (PluginShader.cpp), the schema file and docs/plugins/writing-plugins.md, and a case to tests/plugins/test_plugins.cpp.
- **A new kind of plugin item** (a new list in the manifest): add its def struct and `parseList` call, load it in `PluginRegistry`, show it in
  `PluginsPage::detailsHtml()`, and describe it in the docs. Bump `kApiVersion` only for changes old plugins would not survive.
- **A new standard shader uniform**: declare it in the prelude (PluginShader.cpp), set it in `PluginShaderEffect::apply()`, list it in the
  lint's reserved names and in the docs.
