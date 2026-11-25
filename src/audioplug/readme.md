# src/audioplug

The LV2 and CLAP audio effect hosts (docs/plugins/audio-plugins.md, spec 159). Qt Core, dlopen and lilv only, no Kinoklip types, so the probe program needs no GUI.

| File | What |
|---|---|
| `AudioPluginInfo.*` | what a plugin is (id, channels, parameters, latency port, error) and its JSON (probe result, scan cache) |
| `AudioPluginHost.*` | `describeBundle()` and `Instance` (filters interleaved stereo in place), `ChannelAdapter` in `AudioPluginCore.h` fits mono / stereo / wider plugins |
| `Lv2Host.*` | LV2 through lilv: urid, options and buf-size features, control and atom ports, latency port; without lilv (`KK_HAVE_LV2` unset) it reports LV2 as unavailable |
| `ClapHost.*` | CLAP: audio ports, parameter events, latency and tail extensions |
| `probe_main.cpp` | `kinoklip-audio-probe <lv2|clap> <bundle> <result>`: loads one bundle in a process of its own |
