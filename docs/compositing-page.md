# Compositing page (design)

Owner decision 2026-11-03: work on the Compositing page now (it was planned for v5). It is
Kinoklip's node-based compositing page, our own name for what a Resolve user knows as the page
between Edit and Color. Clean room ([clean-room.md](clean-room.md)): behaviour and layout from
general knowledge of node compositors, our own drawing, names and icons; never Resolve itself.

## What it is for the user
- Every video clip on the timeline can get a **composition**: a node graph whose result replaces
  the clip's picture. A new composition is `MediaIn1 -> MediaOut1` (the clip's own picture
  straight through), so opening a clip on the Compositing page changes nothing until you add
  nodes.
- Open: select a clip (or put the playhead over it) and switch to the Compositing page
  (page bar, Shift+5 in the Resolve keymap), or the timeline clip menu > Open in Compositing Page.
  Clips with a composition show a small badge on the Edit page timeline; Clip > Reset
  Composition removes it.
- The result plays in the Edit page and renders on the Deliver page exactly as on the
  Compositing page (one evaluation path).

## Page layout
- Top: **two viewers** side by side (left, right). Each shows the output of one node: select a
  node and press **1** (left) or **2** (right), or drag the node onto a viewer. Viewer header:
  node name, zoom, and the overlays of the selected node (transform handles for Transform and
  Merge, shape points for masks), reusing the Edit page viewer overlays and guides where they fit.
- Under the viewers: a **time ruler and transport** over the composition's frames (the clip's
  duration), with keyframe ticks of the selected node, and the comp frame / timeline timecode.
- Bottom: the **node editor**: pannable, zoomable canvas (middle-drag or Space-drag to pan,
  wheel to zoom, Fit with Shift+F), nodes as tiles with input ports on the left / top and the output
  on the right, coloured links by port kind (background, foreground, mask), selection and box
  selection, drag to move, drop a node onto a link to insert it, drag from a port to connect, drop an output on another
  node's output to put a Merge there (the dragged node is the foreground),
  drag a link end away to disconnect, Delete, Ctrl+C / Ctrl+V, Ctrl+Z, notes.
- Above the node editor: a **toolbar** with the common nodes (Background, Text, Merge,
  Transform, Rectangle, Ellipse, Polygon, Blur, Color Corrector, Chroma Key); clicking adds the node
  after the selected one. **Shift+Space** opens a searchable "Select Node" list of every node.
- Right: the **Inspector** for the selected node (same parameter rows and keyframe buttons as the
  Edit page Inspector).
- Left (toggle): the **Nodes library** (every node by category; drag onto the node editor).
- Interface bar toggles like the other pages (Nodes, Spline, Keyframes, Inspector). **Spline**
  and **Keyframes** open `KeyframesPanel` in composition mode beside the node editor (the Curves
  view and the dope sheet of the selected nodes' animated parameters); see
  src/ui/keyframes/readme.md.
- Keys: the node editor claims its own keys while it has focus (Delete, Ctrl+C / V / D, F2,
  Ctrl+P, Ctrl+T, 1 / 2, Shift+Space); Keyboard Customization treats them as a separate scope,
  so they never conflict with the Edit page's. The Edit page timeline is hidden here, so its
  editing commands are disabled while the page is shown; transport, undo and the page keys stay
  (TimelineCommandSuspension, shared with the Media and Deliver pages). The Select Node list
  opens at the mouse but stays inside the window like a menu.

## Nodes (first release)
- **I/O**: MediaIn (the clip, or any media pool item), MediaOut.
- **Composite**: Merge (background, foreground, mask; blend mode (Normal, Screen, Multiply,
  Add, Overlay, Darken, Lighten, Difference, Subtract), opacity, foreground centre / pivot / size / angle, flip horizontal / vertical, edges, filter method, invert transform, alpha gain, burn in, subtractive / additive, reference size; 33 apply modes and the Normal operators Over / In / Held Out / Atop / XOr: spec section 88),
  Dissolve.
- **Transform**: Transform (centre, size, angle, pivot, flip), Crop, Resize, Letterbox.
- **Generate**: Background (solid colour or 2/4-colour gradient), Text (font, size, colour,
  outline, shadow, position, and the Text+ controls: Point / Frame / Circle / Path layout with an editable
  Bezier path, tracking, line spacing, kerning, extra shading elements, a Follower for per-character animation and
  Write On; spec section 116), Noise / Fast Noise.
- **Masks**: Rectangle (with Corner Radius), Ellipse, Polygon, B-Spline (points with weights, animated as one
  shape); each with soft edge, invert, level, paint mode, centre / size / angle; masks plug into any node's mask
  input (the Paint Mode says how a mask joins the one plugged into it). Spec section 92. **Bezier**: a closed curve with
  tangent handles on every point, Roto Assist (points snap to picture edges), double-click to add or smooth a point;
  masks and shapes can follow a track with position, size, angle or their points (Mask Tracker, Point Follower; also
  on the Edit page through a Match Move effect). Spec section 117, `src/compositing/tracking/TrackFollow.*`.
- **Matte**: Luma Keyer (channel, low / high, soft range, invert), Delta Keyer (screen colour key with gain,
  balance, pre-matte soft range, clean background / foreground, matte thresholds and despill; pre-matte blur and
  erode, matte blur, erode / dilate, restore fringe, spill method, fringe controls, replace mode, garbage and
  solid mattes and a clean plate input), Ultra Keyer and Matte Control; spec section 115. Maths
  `src/render/CompMatteMath.*`, GPU `src/render/GlCompMatte.*`.
- **Filter**: Soft Glow (threshold, gain, filter, glow size X / Y, colour scale and channels) and Light Wrap
  (the background's light spilling over the foreground's edges; Foreground and Background inputs); spec sections 113 and 122. The float reference maths is
  `src/render/CompFilterMath.*`, the GPU passes `src/render/GlCompFilters.*`.
- **Warp**: Grid Warp (a grid of draggable points, 2 to 9 per side), Displace (a second picture moves the
  pixels: X / Y or Radial) and Vector Distort (vectors in a second picture's red and green); spec section 114.
  Maths `src/render/CompWarpMath.*`, GPU `src/render/GlCompWarp.*`.
- **Corner Positioner** (Transform): four corner points and a perspective warp, with draggable corner handles in
  the viewer.
- **Effects**: every Edit page video effect (62 today: keys, colour, blur, light, stylize,
  distort, time) as a node with one image input and a mask input, generated from the
  EffectRegistry (type `fx:<id>`), so new effects appear on both pages.
- **Color**: Color Corrector (the Color Balance / Levels / Saturation set in one node),
  Channel Booleans (copy / swap channels, alpha from luma), Brightness Contrast, Alpha Multiply, Alpha Divide, Channel Mapper (a 4 x 4 channel mix),
  Color Curves, Hue Curves and the Custom Tool (a formula per pixel, GLSL generated from `core/Expression.*`); spec section 124.
  Maths `src/render/CompColorMath.*`, GPU `src/render/GlCompColor.*`. **Vector Motion Blur** (Filter) blurs along the vectors of a second picture.
- **I/O and Time** (spec 128): **Loader** (an image, an image sequence by any one of its files, or a movie; Global In, Trim In / Out,
  Hold First / Last Frame, Loop, Reverse), **Saver** (writes PNG or TIFF files when the composition is rendered with the page's
  Render Savers button; never while playing), **Time Speed** (Speed, Delay, Nearest / Blend) and **Time Stretcher** (an animatable Source Time).
  The time nodes pull their input again at the mapped frame (`GlComposition::timeNode()`, `Evaluator::timeNode()`);
  `compops::evaluationOrder()` leaves their input out of the order. Frame maths `src/render/CompTimeMath.*`, files `CompLoader.*`, `CompSaver.*`.
- **Transitions** (spec 128): a transition of type `fusion_composition` stores a graph in `TransitionData::composition` with
  MediaIn1 / MediaIn2 (`CompNode::transitionSide`); "Open in Compositing Page" on a timeline transition opens it
  (`CompSession::openTransition()`); comp frame 0 is the first frame of the transition window.
- **Tracking**: Tracker (point tracker, see "Tracker" below), Planar Tracker.
- **Particles** (spec 129): **pEmitter**, **pGravity**, **pTurbulence**, **pMerge** (stream nodes; the new port kind
  `PortKind::Particles` connects only to particle inputs) and **pRender** (draws the stream as a picture). The stream at a frame is
  a pure function of the graph, seed and frame (`src/render/CompParticles.*`): every batch has its own random sequence and a
  particle's place is the integral of the forces over its life, so scrubbing equals playing and nothing is cached. Both renderers
  call `particles::renderNode()` (the GPU uploads the CPU picture, like the Loader).
- **3D** (spec 130): **Image Plane 3D**, **Shape3D**, **Text3D** (flat, or extruded with a bevel: Extrusion Depth, Bevel Depth / Width / Style / Segments, Separate Colors; `src/render/TextMesh3D.*`), **Merge3D**, **Camera3D**, four lights and **Renderer3D**
  on a new port kind `PortKind::Scene3D` (connects only to 3D inputs); Renderer3D turns the scene into a picture. Scene building,
  matrices and a small software rasterizer are in `src/render/CompScene3D.*`; both renderers call `scene3d::renderNode()` (the GPU
  reads the texture pictures back and uploads the result). A 3D node in a viewer shows an orbit view with a grid (`CompViewer`
  event filter; the orbit is `scene3d::viewerOrbit()`).
- **Paint** (spec 131): the **Paint** node; strokes are `paint::Stroke` values (`src/core/PaintStrokes.*`: tool, brush, frame
  range, points with pressure) stored as JSON text in the hidden string parameter `paintStrokes`, so they are saved, copied,
  instanced and undone like any parameter. `src/render/CompPaint.*` draws them (one even coverage layer per stroke; Clone,
  Eraser, Smear); both renderers call `paint::drawStrokes()` (the GPU reads the input back and uploads the result, and passes
  the input through on frames with no stroke). `CompViewerOverlay` turns drags into strokes (`paintBegin/Move/End`, one undo step each).
- Later: (the older lines of the roadmap, from the Spline panel to the node editor extras, are all made: spec 123).

## Data model and evaluation
- `src/core/Composition.h`: value types (Composition, CompNode, NodeLink, CompNote). A clip
  holds `std::optional<Composition> composition` in ClipData. Project format bump (15) with
  migration (older files: no compositions).
- `src/core/NodeRegistry.*`: node descriptors (id, name, category, input ports with kind and
  required flag, outputs, parameters reusing `ParamDescriptor`); effects as nodes come from the
  EffectRegistry.
- `src/core/CompOps.*`: pure operations on a Composition (default composition, add / remove /
  connect / disconnect / move / rename / insert-into-link / paste, unique names, cycle check)
  and one undoable Project call that replaces a clip's composition ("Add Node", "Connect", ...).
- `src/render`: GPU evaluation of a composition for a comp frame: pull from MediaOut (or a
  viewer's node), each node one pass over textures in the timeline resolution with a domain of
  definition where cheap; per-frame texture cache; MediaIn decodes through the existing decoder
  pool; effect nodes reuse the GlEffects passes. The clip's composition output feeds the
  existing clip pipeline (effects, masks, transform, compositing), so the Edit page preview,
  playback and the render all use it. Viewer API: render the image of any node at a comp frame.
- Undo: every graph or parameter change is one undo step in the project's history.

## Order of work
1. Core (model in ClipData, registry, ops, serialization, evaluation, render integration) and
   the page UI (layout, node editor, viewers, inspector hookup) in parallel against
   `Composition.h` and this document.
2. Integration, end-to-end tests (comp in the render), release.
3. More nodes and tools (planar tracker, paint, particles, 3D).

## Tracker (point tracker, issue #3)

**What the user does.** Add a Tracker node (Transform folder) after the picture that should
move. Select it: the viewer shows the tracked point with its **pattern box** (the detail to
follow, solid) and **search box** (how far it may move between two frames, dashed); drag the
cross to place the point, the corners to resize the boxes. Put the playhead on the first frame
and press **Track Forward** (or **Track Backward** to go the other way) in the Inspector; the
playhead follows, a progress bar shows how far it is and **Stop** ends it early (what is
tracked so far is kept). The result is stored as keyframes on the node's Center X / Center Y,
in one undo step; **Clear** removes them. A track stops where the pattern is lost, with a
message.

**What the node outputs.** The Tracker passes its *Input* picture through, moved by the track
relative to the **Reference Frame** (the frame the first track started on): *Match Move* moves
the input with the tracked point (an overlay stuck to a moving object); *Stabilize* moves it
the opposite way (the shaky shot held steady). The picture that is analysed is the optional
**Tracking Source** input if it is connected (typically the plate), otherwise the Input. The
Tracking Source port is only read by the tracking, never drawn, so it costs nothing at render
time. Because the track is plain keyframed X / Y, it can be edited in the Keyframes panel or
by dragging the point on a frame, and (later) other nodes can link to it.

**How it works** (`src/compositing/tracking`, clean-room maths, no OpenCV).
`PointTracker` takes a grey picture, builds a half-size pyramid (the coarsest level keeps the
pattern at least about 9 px wide) and matches the pattern with normalised cross-correlation:
exhaustively over the search box on the coarsest level, then a few pixels around the
scaled-up result on each finer level. A Gauss-Newton step on bicubic samples then refines the
position to a fraction of a pixel. Normalised correlation ignores brightness and contrast
changes. The pattern is taken from the first frame and re-taken from the current frame when
the correlation falls below 0.85, so a slowly changing target stays followed; below 0.6 the
point is lost. `TrackerRunner` feeds it one frame at a time, off the UI thread: the pictures
are rendered by `PlaybackEngine::renderCompositionNode()` of the tracking source (the same
path as the viewers, so decoding, proxies and quality are not duplicated), the next picture
already rendering while the current one is matched. `storeTrack()` writes the result to the node.

**Several patterns (spec 118).** A Tracker holds up to four patterns (Patterns, Active Pattern). Pattern 1
is the node's Center; patterns 2 to 4 are hidden keyframed positions (`nodekeys::patternKey()`). The Track
buttons act on the active pattern, the viewer shows all boxes. With more than one pattern the output is the
least-squares move, turn and scale of the group about its middle at the reference frame
(`compmath::trackerMatrix()`, Match Position / Rotation / Scale), and a node that follows the track gets the
same size and angle (`tracking::sourceMotion()`).

## Planar Tracker and track links (1.2, issue #3)

**What the user does.** Add a Planar Tracker node (Transform folder) after the picture to pin or
to move, and connect the plate to its Tracking Source (or leave the Input as the plate). The viewer
shows a quad with four corner handles: drag the corners onto a flat, textured, fully visible
surface (a screen, a wall, a poster) on the first frame, or drag inside to move the whole quad.
**Track Forward / Backward / Stop / Clear** and the progress bar work as on the Tracker; the
corners are stored as keyframes of the eight corner parameters, in one undo step. **Pattern Size**
is the size of the patches that are followed, **Search Size** how far a patch may move between
two frames. A track stops where the surface is lost (hidden, blurred, out of the picture).

**What the node outputs** (Output): *Corner Pin* draws the Input with its four corners on the
tracked corners (perspective), over transparency, so a Merge puts it onto the plate; *Unsteady*
moves, turns and scales the Input the way the plane did since the **Reference Frame**; *Steady*
does the opposite (the plane held still). Unsteady / Steady use the best-fit similarity of the
four corners, so they are the position, rotation and scale of the plane without the perspective.

**How it works** (`src/compositing/tracking`). `PlanarTracker` lays a grid of 7 x 7 patches on the
region (spread over the surface through the homography of the unit square, so a region in
perspective gets an even spread) and keeps those with enough contrast. For every new frame the
patches are cut from the start frame warped by the previous frame's homography (so rotation, scale
and perspective do not spoil the match), searched with the point tracker's matcher (normalised
cross-correlation, a half-size search first, then full size, then the sub-pixel Gauss-Newton step;
`PatchMatch.h`), and `estimateHomography()` (`Homography.h`: RANSAC over 4-point samples, normalised
DLT, refit on the inliers) turns the matches into the plane's homography. A second pass with the
patches re-warped by that result, a small radius and a stricter correlation limit refines it and
drops patches that are partly covered by something in front of the plane. The patches come from the
start frame (no drift) and are re-cut from the current frame when the mean correlation falls
below 0.85. The corners are the homography applied to the start corners; a result that is not a
convex quad, or whose area changed more than 40 times, or with too few agreeing patches, is a loss.
`TrackerRunner` runs it exactly as it runs the point tracker (same thread, same pictures, same
progress and stop); a job with `planar` set carries the four corners.

**Track Mask (spec 118.1).** A Mask-kind input, analysis only like Tracking Source: the mask node's picture
is rendered for each frame next to the plane's, and `PlanarTracker::reset()` / `track()` take it as a
`GrayImage`; patches under black (below 0.5) are not placed / not used.

**Linking other nodes to a track.** (Since spec 119 there are live parameter links, Connect To; a track is still
copied, not linked.) The mechanism for tracks is *Connect to Tracker n*: nodes with a Center
(Transform, Merge, Text, the shape masks, and so on) have a "Follow Track" row in the Inspector;
Connect copies the chosen track (a Tracker's point, or the centre of a Planar Tracker's region)
into the node's Center X / Y keyframes at the frames of the track, with a fixed offset that keeps
the node where it is at the playhead, in one undo step (`tracking::linkToTrack()`). It is a copy,
not a live link: after tracking again, press Connect again. Because the result is plain keyframes
it can be edited in the Keyframes panel and needs no change to the data model, the project file or
the renderers.

**Not done yet.** Polygon regions (a bounding plane from more than four points), motion
prediction for very fast movement, per-frame manual correction with re-tracking from the
corrected frame, and a live (re-resolving) link. Size, angle and shape points can be
followed (`TrackFollow`, spec 117).

## Modifiers, links and expressions (1.11, spec 119)

A number parameter of a node can carry modifiers: `AnimatedValue::modifiers` is a list of `ParamModifier`
(`core/ParamModifier.h`: kind, id, formula text, link target, on / off). A modifier's own numbers are normal
parameters of the same node under `~<id>.<name>` (`modifiers::controlKey()`; Perturb: period, strength, seed; Wiggle:
wavelength, amplitude, octaves, seed; Offset: offset, scale, timeOffset; Expression: n1 to n3 and the text on the
modifier; Cycle: start, end, mode (a static choice), repeats; Path: path), so rows, keyframes, undo, the Curves view,
save and load come for free. `ParamSet::value()` and `valueAtTime()` call `modifiers::evaluate()` when a value has
modifiers: it folds the stack from the keyframed value up, each modifier able to read the value under it at another
time (Offset's Time Offset, Cycle). Nothing else needs to know: the renderers, the viewer overlays and the Inspector
rows all read through `ParamSet`.

Links (Connect To, and `Node.Parameter` references in a formula) need other nodes, which a `ParamSet` cannot see.
`complinks::resolved(comp, frame)` returns a copy of the composition with their values filled into
`ParamSet::bound` (runtime only: not saved, ignored by `operator==`); `GlRenderer::compositionTarget()` and the CPU
renderer's `compositionImage()` call it once per frame when `complinks::hasLinks()` says there is something to
resolve, so both renderers see the same numbers. Cycles are cut where they close (the reference stays unfilled and the
parameter keeps its own value); `connectParam()` and the Expression entry refuse to make one (`dependsOn()`).

`core/Expression.*` is the formula language: a recursive-descent parser to a small tree (cached by text), evaluation
without loops or calls, numbers only, a hard limit on length and depth. A formula's references are returned by
`Program::references()` and their values passed in that order.

Inspector (`CompInspectorModifiers.cpp`): `ParamRow` asks its `ParamEditHost` (`supportsModifiers()`, `fillParamMenu()`,
`setParamExpression()`, `paramModifierNote()`) for the right-click menu, the typed `= formula` and the "fx" mark, so the
Edit page Inspector is untouched. The Modifiers sections are built from `modifiers::controls()` with ordinary rows, and
the Inspector rebuilds when the set of modifiers changes. `CompSession::addModifier()` is the one command behind the menu
and the Modifiers folder of the Nodes library.

## Groups, macros, instances and underlays (1.11, spec 120)

A Group is a container, not a sub-graph: `CompNode::group` says which Group node a node is in, `collapsed` says whether
the editor draws the group as one tile. Evaluation never looks at it (the Group node has no ports and nothing reads it),
so rendering, links and undo are unchanged and a grouped composition renders exactly like the ungrouped one. The editor
decides everything else: `compgroups::hiddenBy()` is the outermost collapsed group of a node, `NodeEditor::tileRect()`
gives a hidden node its group's tile (so links end there), `hitTest()` and box select skip hidden nodes, and
`CompSession::moveSelection()` moves `compgroups::withMembers()` (and the nodes on a selected Underlay).
`compops::copyNodes()` / `pasteNodes()` / `removeNodes()` know about groups (members copied with the group, `group`
remapped, members freed when the group is deleted).

A Macro is a Group whose `controls` (`MacroControl`: key, label, member node, parameter) are numbers on the macro node
(`ParamSet` keys "c1", "c2", ...); `compgroups::makeMacro()` connects the inner parameter to the control with a Connect To
modifier, so everything in spec 119 (keyframes on the control, formulas, resolving per frame) applies, and a control
needs no renderer support. `complinks::connectParam()` and `findReference()` accept macro controls although they have no
node descriptor; the Inspector builds their rows from `compgroups::controlDescriptors()` (the inner descriptor under the
control's key and label). A macro saved as a title is `macrofiles::MacroFile` (the group node first, then its members; ids
inside the file are kept and remapped by `compops::pasteNodes()` on insert); `titleComposition()` builds the default
composition with the macro inserted and its output node connected to MediaOut, and `effectactions::makeGeneratorClip()` turns
a "macro:<name>" id of the Effects Library into a transparent Solid clip with that composition.

An Instance has `instanceOf`; `compgroups::applyInstances()` (called first by `complinks::resolved()`, and `hasLinks()` is true
when an instance exists) copies the original's params, path and media into it on the render copy. An Underlay is a `CompNote`
with `underlay` set; the editor draws it first and hit-tests only its title strip.

## Viewer tools: onion skin, region, proxy and cache (1.11, spec 121)

`CompViewer::requestRender()` is the one place that turns the viewer's switches into a render. `needsFx()` is true when any of
them differs from "plain" (channel, checker, split, onion skin, region, Proxy / Auto Proxy, HiQ or MB off, disk cache): the viewer
then renders its node itself (`PlaybackEngine::renderCompositionNode()` with `OutputImage`) instead of showing the engine's
picture, and the pictures go through `viewerfx` in a fixed order: split wipe, onion skin (`onionSkin()` over the frames around the
current one, each a `Pending` render), channel, region (`applyRegion()`), checker. `CompViewOptions` (a process-wide object like
`ViewerOverlaySettings`, settings `compositing/view/...`) holds HiQ, MB, Proxy, its ratio, Auto Proxy and the cache switch; its
`renderSize()` gives the proxy size and `viewerTimeline()` a draft copy of the snapshot (`draftComposition()`: `ccMotionBlur` off,
Filter Method nearest on Merge / Transform), a copy that shares everything except the clip's composition; the real timeline is
never touched. Auto Proxy needs to know when something is going on: `CompViewer::noteInteraction()` (playhead, graph change)
restarts a 500 ms window and arms a 600 ms idle timer that renders the full picture again.

The disk cache (`ViewerCache`) stores the raw picture of a node render under `makeKey()` = SHA-1 of `cacheContent()` (the
composition's picture JSON, the clip's source, the timeline format and the files read), the node, frame, size and the HiQ / MB
bits. A hit builds a `RenderedFrame` from the file and skips the render; a miss stores the result on the worker thread; the folder
is trimmed to its limit by modification time (`load()` touches the file). Rulers and Safe Area reuse `ViewerRuler`,
`GuideOverlay` and `ViewerOverlaySettings` of the Edit page, so the viewer tool bar entries change the shared View options. The
Region of Interest is `ViewerRegion`, an overlay like `CompViewerOverlay` (fractions of the picture, handles, double-click resets).

## Motion Blur (1.7, spec 94)

Common Controls of Merge and Transform only (`NodeRegistry::commonControls()`): `ccMotionBlur`
(static bool), `ccMbQuality` (int, default 2), `ccMbShutter` (degrees, default 100),
`ccMbBias` (-1..1), `ccMbSpread` (0..2). Keys are written only when set. Public Fusion manuals
only (clean room). `compmath::motionBlurSamples()` gives 2q + 1 frame offsets across the
shutter (angle / 360 frames, centred, moved by Center Bias) and normalised weights
(exp((spread - 1) * 3 * u^2)). The node's matrix is evaluated at each fractional frame
(`transformMatrix()` / `mergeForegroundMatrix()` take a double frame; `ParamSet::valueAtTime()`
-> `AnimatedValue::valueAtTime()`), the input is resampled for each (per-pixel resampler on
both paths) and the pictures are averaged: GPU `GlComposition::blurredResample()` (running
weighted mean with the mix pass), CPU `weightedMean()` in `CpuComposition`. For Merge only the
foreground transform is blurred; Blend, mode and mask apply to the averaged foreground.
Tracker, Planar Tracker and Corner Positioner do not blur yet.

## Node editor extras (1.7, spec 93)

Insert on links (Shift-drag of a connected tile; library drops already did), tile pictures
(`CompNode::thumbnail`, rendered by a page-supplied `NodeEditor::ThumbnailSource` through
`PlaybackEngine::renderCompositionNode()`, throttled), hover tooltip, Replace With
(`compops::replaceType()`), Find (Ctrl+F, panel-local), Set Color (`CompNode::color`), Paste
Settings (Ctrl+Shift+V, panel-local) and Shift-drag on a link to disconnect it. Colour and
picture flag are written only when set; the project format stays 19. Fit Nodes to View is
Shift+F now (Ctrl+F is Find).
