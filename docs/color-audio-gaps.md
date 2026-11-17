# Color and Audio page gaps

Goal for 1.10.0: a user can follow any YouTube guide for DaVinci Resolve's Color page and Fairlight (Audio) page and it just works. This file lists the roughly 40 most common beginner and intermediate tutorial workflows for each page and whether Kinoklip can do them today, step by step. Checked on 2026-11-17 against the code, [color-page.md](color-page.md), [behavior-spec.md](behavior-spec.md) (sections 37, 44), [effects-library-status.md](effects-library-status.md) and [roadmap.md](roadmap.md).

Clean room ([clean-room.md](clean-room.md)): the names below are those of Resolve's public reference manual and of public tutorials; nothing was taken from Resolve itself.

- **works**: the tutorial can be followed with the same palette, button or menu names.
- **partly**: the main steps work; the named piece is missing.
- **missing**: a tutorial step fails at the named element.

Every missing piece has a line in [roadmap.md](roadmap.md) under "Next version (1.10.0)".

## Color page

| # | Workflow | Status | What is missing |
|---|---|---|---|
| C1 | Blur or censor a face with a tracked window | missing | Blur palette (Radius, Scaling), Tracker palette (window tracking: Pan / Tilt / Zoom / Rotate / 3D), Cloud tracker; windows cannot follow a subject |
| C2 | Blur a polygon or circle window (static) | missing | Blur palette on the Color page (the Edit page Blur effect and clip masks blur or cut the whole clip, not a node's window) |
| C3 | Sharpen part of the picture (Blur palette, Sharpen mode) | missing | Blur / Sharpen / Mist palette with its Radius R G B link and Scaling |
| C4 | Mist / soften skin or glow with the Mist mode | missing | Mist mode of the Blur palette |
| C5 | Power windows: Circle, Linear, Polygon, Gradient (softness, invert, opacity, keyframes) | works | |
| C6 | Curve (Bezier) power window | missing | Curve window button in the Windows palette; Bezier points and tangents on the viewer |
| C7 | Window follows the subject (animate or track a window) | missing | Tracker palette applied to windows; polygon shape keyframes |
| C8 | Tracker and Stabilizer palettes on the Color page | partly | Edit page Inspector has stabilization and the Tracker; the Color page has no Tracker or Stabilizer palette and nothing drives a window or node from a track |
| C9 | Qualifier HSL: pick colours, adjust ranges and softness, invert, Highlight | works | |
| C10 | Qualifier clean-up: Matte Finesse (Clean Black / White, Blur Radius, Shrink / Grow, In / Out Ratio, Denoise) | missing | Matte Finesse section of the Qualifier palette |
| C11 | Qualifier 3D and Luma modes, RGB mode | missing | 3D, RGB and Luma qualifier mode buttons; 3D picker with Pick / Pick + / Pick - in 3D space |
| C12 | Skin tone with a qualifier and a Hue vs Sat curve | partly | works except Matte Finesse (C10); the vectorscope skin tone line exists |
| C13 | Change the sky colour (qualifier plus gradient window) | works | |
| C14 | Sky replacement | missing | Sky Replacement effect (planned in effects-library-status.md) |
| C15 | Primaries wheels: Lift, Gamma, Gain, Offset, Contrast, Pivot, Sat, Hue, Lum Mix, Temp, Tint | works | |
| C16 | Primaries extras: Shad, Hi, Color Boost, Midtone Detail, Auto Color / Auto Balance buttons | missing | Those controls in the Primaries palette |
| C17 | Primaries bars mode | works | |
| C18 | Log wheels (Shadow, Midtone, Highlight, Offset with Low / High range) | missing | Log palette |
| C19 | Custom curves Y, R, G, B with the Link toggle | partly | Soft Clip and the eyedropper that adds curve points from the picture |
| C20 | Hue vs Hue, Hue vs Sat, Hue vs Lum, Lum vs Sat, Sat vs Sat | partly | Sat vs Lum curve; picking a colour from the viewer to add the three points |
| C21 | Color Warper (Hue - Sat grid, Chroma - Luma grid) | missing | Color Warper palette |
| C22 | HDR palette (zones, Global, Shadow, Light, Highlight, Specular) | missing | HDR palette |
| C23 | RGB Mixer | missing | RGB Mixer palette (Red / Green / Blue output sliders, Monochrome, Preserve Luminance) |
| C24 | Motion Effects: Temporal and Spatial Noise Reduction, Motion Estimation | partly | Edit page Noise Reduction effect (spatial); no Motion Effects palette, no temporal noise reduction, no Better / Faster modes |
| C25 | Magic Mask (draw a stroke on a person or object, it follows) | missing | Magic Mask palette; the optional Background Removal model only cuts a person out on the Edit page |
| C26 | Color match / Shot Match to another clip | missing | Color Match palette (Shot Match, Color Chart); Color Stabilizer is another effect |
| C27 | Apply a LUT to a node, a clip or the timeline (3D LUT browser) | works | |
| C28 | Export a LUT from a grade (Generate 3D LUT) | missing | Generate 3D LUT (CUBE) in the clip context menu |
| C29 | Node types: Serial, Parallel, Layer | works | |
| C30 | Node types: Outside, Key Mixer, Splitter / Combiner, compound node, typed node labels | missing | Add Outside Node (Alt+O), Add Key Mixer, Splitter / Combiner, Compound Node, Node Label field |
| C31 | Alpha output and key: node Key tab, Key Output, Alpha Output | partly | Key tab has gain, invert and matte; no Alpha Output node, no key input / output ports, a grade cannot make the clip transparent |
| C32 | Gallery stills: Grab Still, apply, wipe, rename, delete | works | |
| C33 | Gallery albums, Power Grade albums, Memories, import and export stills (DRX), export a still as an image | missing | Album list in the Gallery, Memories A to Z, Export Still / Import Still |
| C34 | Versions of a grade (Local / Remote, Create New Version) | missing | Versions menu on the clip |
| C35 | Groups with Pre-Clip and Post-Clip grades; Timeline grade | partly | Timeline grade works; Groups, Pre-Clip and Post-Clip grades, ColorTrace missing |
| C36 | Scopes: Waveform, RGB Waveform, Parade, Vectorscope, Histogram | works | |
| C37 | CIE chromaticity scope, scope gain / settings, HDR scales | missing | CIE Chromaticity scope; Waveform / Vectorscope settings (scale, gain) |
| C38 | Color space transform, ACES, color managed project, input colour space per clip | missing | Color Space Transform, ACES Transform, Gamut Mapping and Chromatic Adaptation effects (planned in effects-library-status.md); Color Management in Project Settings; Input Color Space in Clip Attributes |
| C39 | Film grain and film look (Film Grain, Halation, Vignette, Film Look) | partly | The Edit page effects exist; no OpenFX / Resolve FX palette to apply them to a node on the Color page |
| C40 | Face Refinement, Depth Map, Relight (Neural Engine tools) | missing | The three effects (planned) with optional local open models |
| C41 | Lens Blur, Glow and other effects on one node, limited by its window or qualifier | partly | Effects exist on the Edit page; a node cannot hold a Resolve FX, so a window or qualifier cannot limit them (OpenFX palette) |
| C42 | Vignette with an inverted circle window and Gain | works | |
| C43 | Copy a grade to other clips (Ctrl+C / Ctrl+V, middle-click, Apply Grade) | works | |
| C44 | Dynamic grade with keyframes | partly | Add / Remove Keyframe works; no Keyframes palette / strip on the Color page, no Dynamic interpolation |
| C45 | Sizing palette (Zoom, Pan, Tilt, Rotate, Flip, Input / Output sizing) | partly | Edit page Transform does it; no Sizing palette on the Color page |
| C46 | Compare shots: Gallery wipe, Split Screen, Lightbox, Highlight | partly | Wipe and Highlight work; no Split Screen modes (Selected Clips, Timeline), no Lightbox |
| C47 | Timeline grade and Adjustment clips | works | |
| C48 | Output blanking / letterbox bars | missing | Output Blanking in Project Settings and on the Color page |

## Audio page (Fairlight)

| # | Workflow | Status | What is missing |
|---|---|---|---|
| A1 | Per-track EQ (6 bands) with the curve editor | works | |
| A2 | EQ presets (voice, music, telephone) | missing | Presets menu in the EQ window |
| A3 | High-pass filter against rumble | works | |
| A4 | Compressor on a voice track | works | |
| A5 | Limiter on the master | works | |
| A6 | Noise gate and expander | missing | Gate and Expander sections in the Dynamics window (spec 37.20) |
| A7 | De-esser | missing | De-Esser effect (Frequency, Range, Reduction) |
| A8 | Duck music under a voice (sidechain compressor) | partly | Level lines and fader automation work by hand; Dynamics has no Sidechain input / Key listen, and no Ducking helper |
| A9 | Voice Isolation | missing | Voice Isolation button on the track and in the Inspector (local open model) |
| A10 | Noise Reduction (Fairlight FX, Learn noise print) | missing | Noise Reduction effect with a learned profile (roadmap: RNNoise / DeepFilterNet) |
| A11 | Dialogue Leveler / Dialogue Processor | missing | Dialogue Leveler and Dialogue Processor effects (spec 37.28) |
| A12 | Normalize Audio Levels on clips (Sample Peak, True Peak, ITU-R BS.1770) | works | |
| A13 | Normalize a whole track, bus or the main mix to a loudness target | missing | Normalize action for a track, bus or the main mix |
| A14 | Loudness meter in LUFS (momentary, short-term, integrated, true peak) | partly | Selectable standard / target (-14 LUFS for YouTube, -16, -23, ATSC), Loudness Range (LRA), history graph (spec 37.21) |
| A15 | Loudness normalisation when rendering | missing | Normalize to a loudness target in the Deliver page Audio settings |
| A16 | Buses: main bus and submix buses | partly | Bus-to-bus feeds, Main / Submix / Aux bus types (spec 37.32) |
| A17 | Aux sends (reverb or delay send with a level knob) | missing | Send slots with level and Pre / Post fader on every track strip (spec 37.22) |
| A18 | Effects Library with track effect slots (insert effects in the mixer) | missing | Effects Library and effect slots on the mixer strip (roadmap item exists) |
| A19 | Clip audio effects in the Inspector (Gain, Equalizer, Compressor, Limiter, filters, Delay, Reverb) | works | |
| A20 | Reverb with a room preset | partly | Reverb has Room Size, Damping and Mix only; no Pre-delay, Decay time, Presets, no send |
| A21 | Echo / Delay | works | |
| A22 | Pitch change of a clip | works | |
| A23 | Chorus, Flanger, Distortion, Stereo Width | missing | Those effects |
| A24 | Fader automation: Read, Touch, Write | partly | Latch and Trim modes; automating pan or plug-in parameters from the strip's Auto buttons works for fader and pan only |
| A25 | Edit automation curves on the timeline (automation lane) | missing | Automation lane under the track with Volume / Pan / effect parameter curves (spec 37.22) |
| A26 | Pan, including surround panner | partly | Stereo balance works; no Surround panner / 3D joystick, no bus pan |
| A27 | Fade handles and crossfades | works | |
| A28 | Clip volume with a level line and keyframes | works | |
| A29 | Record a voice-over on an armed track | works | |
| A30 | ADR (cue list, line-by-line recording) | missing | ADR panel (spec 37.24) |
| A31 | Mute, Solo, track mono / stereo format | works | |
| A32 | 5.1, 7.1 and adaptive track formats | missing | Track format choice beyond Mono / Stereo (spec 37.26) |
| A33 | Invert phase (polarity) on a track | missing | Phase invert button on the strip |
| A34 | Choose which source channel plays (Clip Attributes > Audio) | works | |
| A35 | Sync audio by waveform (dual-system sound) | works | Multicam sync by audio exists; also in the Media Pool |
| A36 | Bounce Mix to Track / Bounce Selected Tracks | missing | Both Timeline menu commands (roadmap item exists in Later) |
| A37 | Mixer track groups and VCAs (link faders) | missing | Track groups and VCA faders (spec 37.31) |
| A38 | Spectrum analyzer, phase / correlation meter | missing | Spectrum and Phase meters in the monitoring panel |
| A39 | Monitor output control (Dim, Mono check, speaker select) | partly | Output device in Preferences; no Dim / Mono / Mute monitor controls on the Audio page |
| A40 | Audio scrubbing and waveform view | works | |
| A41 | Dialogue / sound library search | missing | Sound Library panel (spec 37.27) |
| A42 | Elastic Wave (stretch audio to a clip's speed without moving pitch) | partly | Speed change with Pitch Correction works; Elastic Wave editing inside a clip is missing (spec 37.29) |
| A43 | Plug-ins (VST / AU / LV2 third-party effects) | missing | Plug-in host (spec 37.25) |
