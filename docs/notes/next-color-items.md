# Implementation notes: Curve window, RGB Mixer, Color Warper (1.10.0)

Research only, written 2026-11-17. Nothing here is implemented. Clean room rule: Resolve behaviour comes
only from Blackmagic's public manual and public tutorials; the open-source projects below are for maths
and structure. Manual quotes marked (verify) were written from memory of the public DaVinci Resolve 18/19
manual (chapters "Power Windows" / "Color Page Palettes") and must be checked against the text before the
spec rows are written; I could not fetch the PDF in this pass. Open-source file paths were checked on
2026-11-17 unless marked (unchecked).

Shared facts about the code (all three items touch these):
- Parameters of a node are `ParamSet` keys in `namespace gradekeys` (src/core/Grade.h), with defaults in
  `gradekeys::defaultValue`, the list in `allKeys()`, and `isNodeIdentity()` (Grade.cpp ~485) loops over
  `allKeys()`, so a new key list is picked up for identity and "has keyframes" automatically.
- Per-frame values reach both renderers through `grademath::XxxValues::at(node, frame)` (GradeMath.h),
  `PreparedGradeNode` (src/render/GradeEval.h, filled in `prepareGrade`, GradeEval.cpp ~80), the CPU
  `gradePixel` / `applyGrade` (GradeEval.cpp ~130 and ~370: two loops, one per entry point) and the GL
  shader in src/render/GlGrade.cpp (`drawCorrector`, main at ~360). Rule: same function on both sides.
- Project JSON: GradeSerialization.cpp (`windowToJson` ~168, node object ~265). Bump the project format
  (Serialization.h history; docs/color-page.md says format 16) once for the whole release.
- Palette pattern: `BlurPanel` (ColorPalettes.h/.cpp ~1167-1300) is a ValueField per key looped over
  `gradekeys::blurKeys()`; tabs are added in ColorPage.cpp `buildPalettes` (~384-430: `m_paletteStack`,
  `tabRow(...)` name list). Reset button resets "the shown palette's values" (ColorPage ~429): extend it.
- Docs to update when done: docs/color-page.md, docs/behavior-spec.md (new spec rows after 108),
  docs/color-audio-gaps.md (C6, C7, C21, C23), docs/roadmap.md, docs/menu-reference.md if a menu item appears.

---------------------------------------------------------------------------------------------------

## 1. Curve (Bezier) power window, with polygon and curve shape keyframes (gap C6, C7)

### (a) What Resolve does
- The Windows palette has Circle, Linear, Polygon, Curve (pen icon), Gradient buttons, plus Pen / PowerCurve
  cursors. Public manual (Power Windows, verify): "the Curve window ... lets you draw a curve by clicking
  to create a series of points, the points are smooth: drag to pull out Bezier handles"; closing: click the
  first point. Polygon: straight segments, "click to create points ... close by clicking the first point".
- Both have Softness values (Soft 1 inside / Soft 2 outside style, as our SoftIn / SoftOut), Opacity,
  Invert, per-window Delete. Points can be added by double clicking the outline (Polygon) and deleted
  (verify). Curve points can be turned between smooth and corner (verify).
- Animation (manual "Keyframing a Window" / tracker chapter, verify): a window's shape is animated with
  keyframes in the Keyframes editor; changing a point at another frame creates a key, shape interpolates
  between keys (Resolve default "Dynamic" interpolation; Linear and Smooth exist). Tracker moves windows
  (already done, spec 107).

### (b) Open source references
- Natron roto, GPL-2.0-or-later: Engine/Bezier.cpp (github NatronGitHub/Natron, branch RB-2.5).
  `BezierCP` holds position + left / right tangent points, each animated per time (`getPositionAtTime`,
  `getLeftBezierPointAtTime`, `getRightBezierPointAtTime`); `bezierSegmentEval()` (line 715) flattens one
  segment with a cubic: P(t) = (1-t)^3 P0 + 3(1-t)^2 t P1 + 3(1-t) t^2 P2 + t^3 P3, either at a fixed number
  of points or by an error-scale adaptive subdivision (`ROTO_BEZIER_EVAL_ITERATIVE`). Feather is a second
  outline of control points (`getFeatherDistance`). Works well (it is a production roto tool); our case is
  simpler: no separate feather outline, softness comes from a distance field like the existing polygon.
  Key lesson: every point is animated, so adding or removing a point adds it to every keyframe.
- darktable masks, GPL-3.0-or-later: src/develop/masks/path.c (bezier path mask): `_update_bezier_ctrl_points`
  (~479) modes NONE / SINGLE / SYMMETRIC / SING_SYMM = what happens to the opposite handle when one is
  dragged; matches our existing mask editor rules (behavior-spec 27.10: smooth point keeps the opposite
  handle collinear, Alt breaks).
- In-repo precedent (no licence issue): src/render/CompMath.cpp `flattenPath` (used by GlMasks.cpp, Bezier
  mask, tolerance `kFlattenTolerance`) and `bsplinePolygon`; mask path type `MaskPath` (CompMath / masks).
  Reuse the flattening, do not write a second one. Mask keyframes (`path.at(frame)`) are the model for
  shape keyframes.
- Kdenlive / MLT rotoscoping: MLT filter `rotoscoping` (src/modules/kdenlive/filter_rotoscoping.c, LGPL;
  path unchecked) interpolates spline points between keyframes. Not needed beyond confirming the idea.

### (c) How it fits the code
Model (src/core/Grade.h):
- `GradeWindowShape` gets `Curve` appended after `Gradient` (serialized by string "curve"; update
  `gradeWindowShapeToString/FromString`, `ColorPalettes.cpp` shape-name switch ~63, the 4-element
  `m_shapes[4]` button array in ColorPalettes.h -> 5).
- Replace the bare `QVector<QPointF> points` with a shape type shared by Polygon and Curve:
  `struct GradeWindowPoint { QPointF pos; QPointF inHandle; QPointF outHandle; bool smooth; }` (handles are
  offsets from `pos`, window units like `points` today; a Polygon ignores handles). Keep `points` readable
  by the old JSON ("points": [[x,y]] loads as corner points) so projects of format 16 still open.
- Shape keyframes: `QMap<int64_t, QVector<GradeWindowPoint>> shapeKeys` on `GradeWindow` plus the static
  base shape. `GradeWindow::shapeAt(int64_t localFrame)` returns the base shape when `shapeKeys` is empty,
  else holds before the first / after the last key and interpolates linearly between neighbours (positions
  and handles; all keys have the same point count by construction). Interpolation presets can come later
  (reuse `Interp` / `AnimatedValue` curves from Animation.h for the blend factor if cheap).
- Limits: `kMaxWindowPoints` (32) stays the number of editable points; the flattened outline needs its own
  larger bound, e.g. `kMaxWindowOutlinePoints = 256`, because `WindowValues::points` is the flattened outline.
- `GradeWindow::isRenderable()` (Grade.cpp): Curve needs 3 points. `GradeNode::isAnimated()` /
  `ClipGrade::isAnimated()` / `shiftKeys` (Grade.h 381-383) must count and shift `shapeKeys`.
  `addWindow(..., Curve)` seeds a default 4-point rounded shape (like the polygon's default).
Maths (src/core/GradeMath.cpp, h ~118-174):
- `evaluateWindows()` flattens the shape at the frame: Polygon -> points as now; Curve -> per span a cubic
  from `pos`, `pos+outHandle`, next `pos+inHandle`, next `pos` (flattened with the CompMath tolerance, in
  window pixels after the Width / Height scaling so the tolerance is in output pixels), into
  `WindowValues::points`. The distance field in `windowCoverage` (even-odd test + nearest segment) then
  works unchanged. Tracker (`trackedWindowGeometry`) needs no change: it moves, scales, rotates the whole
  window and the points are window-local.
GPU / CPU:
- GPU: no shader change. `GlGrade::uploadPoints` (GlGrade.cpp ~760) copies `WindowValues::points` into the
  `uPoints` RG32F texture; only raise the texture capacity (`m_pointsCapacity`, grows by itself) and check
  shader loop cost with 8 windows x 256 points (loop in GlGrade.cpp ~310; worst case ~2000 texel fetches per
  pixel; if slow, flatten coarser at preview scale or bound the number with a bounding-box early out).
- CPU: `windowCoverage` unchanged. Because both read the same flattened vector, parity is by construction.
Serialization (GradeSerialization.cpp `windowToJson` / `windowFromJson`):
- "points": [[x, y, inX, inY, outX, outY, smooth], ...]; accept 2-number entries; "shapeKeys":
  [{"frame": n, "points": [...]}]. Clamp counts on load (kMaxWindowPoints) and drop keys whose count differs
  from the base shape. Bump format.
UI:
- ColorSession (ColorSession.h ~137): replace `setWindowPoints(id, QVector<QPointF>, label)` by
  `setWindowShape(id, QVector<GradeWindowPoint>, label)`; it writes a key at `gradeFrame()` when the window
  already has `shapeKeys` or the new "Keyframe shape" toggle is on (a button in the Windows palette next
  to Invert, ValueField auto-key rules as for other params), else it edits the base shape. One undo step.
- ColorViewerOverlay.cpp: `windowPath` (~225) builds a QPainterPath with `cubicTo` for Curve (already
  returns QPainterPath: good); handle enum (`Handle::Point`, .h line ~54) gets `TangentIn`, `TangentOut`;
  `hitTest` / `handlePos` (~290-350) return the tangent ends; `paintWindow` (~405-470) draws the handle
  lines; `applyDrag` (~562) turns a tangent (smooth point mirrors the opposite handle, Alt breaks, as in
  behavior-spec 27.10); double-click on the outline inserts a point (existing code ~710; for a curve split
  the cubic with de Casteljau so the shape stays); double-click on a point toggles smooth / corner;
  right-click deletes (keeps 3).
- Drawing a new Curve window: the Windows palette Curve button creates a default shape (like Polygon
  today); a click-to-draw pen mode is optional, not needed for the first cut.
- Shape keyframe feedback: a small diamond / marker in the Windows list row of a window with `shapeKeys`;
  "Remove shape keyframe" / "Clear shape keyframes" buttons. (The full Keyframes palette is a separate
  roadmap item.)

### (d) Tests
- tests/core/test_grade.cpp (or a new test_curvewindow.cpp, add to tests/groups.cmake core group): JSON round
  trip of Curve and of a window with `shapeKeys`; a format-16 polygon file loads as corner points; counts
  are clamped; `shapeAt` holds before the first key, is exact on keys, midway between two keys is the mean.
- GradeMath: flattened outline of a 4-point smooth circle approximation stays within 0.5 px of a true
  circle; handles of zero length give the same coverage as the polygon of the same points; soft edge
  values equal the polygon case; tracker offset moves a curve window like a polygon.
- tests/render/test_graderender.cpp: GPU against CPU (the existing parity harness) for Curve and for a
  keyframed polygon at two frames.
- tests/app/test_colorpageui.cpp: Curve button adds a window; dragging a tangent is one undo step and the
  opposite handle follows; double-click inserts a point; keying at a second frame creates `shapeKeys` and
  moving the playhead changes the shape; delete point removes it from every key.

---------------------------------------------------------------------------------------------------

## 2. RGB Mixer palette (gap C23)

### (a) What Resolve does
- Manual, Color page palettes, "RGB Mixer" (verify): "The RGB Mixer lets you mix the red, green and blue
  channels ... each output channel is made from the Red, Green and Blue input". UI: three tabs Red / Green /
  Blue (the output channel); each has three sliders Red, Green, Blue (how much of each input goes into that
  output). Defaults: identity (Red output = 1.0, 0.0, 0.0; Green = 0.0, 1.0, 0.0; Blue = 0.0, 0.0, 1.0).
  Range about -2.0 to 2.0 (verify). "Monochrome" checkbox: the picture becomes grey, the three sliders then
  weight R, G, B into the grey (defaults about 0.3 / 0.59 / 0.11, verify). "Preserve Luminance" checkbox:
  keeps the luminance of the picture when the mix changes it (verify). Reset button; keyframeable; applies
  per node, limited by the node's key like any correction.
- Public tutorial use: black and white conversion with a channel bias, Red-channel-to-grey for skin, teal and
  orange cross mixing.

### (b) Open source references
- darktable channelmixer.c, GPL-3.0-or-later: src/iop/channelmixer.c. `process_rgb` is the 3x3 matrix
  (red / green / blue output rows); `process_gray` (line 343) is the Monochrome path:
  gray = max(m[0] r + m[1] g + m[2] b, 0), written to all three channels. Works well and is the same
  model as Resolve's Monochrome. (Also its HSL modes: not needed.)
- darktable channelmixerrgb.c, GPL-3.0-or-later: src/iop/channelmixerrgb.c has `normalize_R/G/B/sat/light/
  grey` options: a row normalization so a mix does not change brightness. Closest to "Preserve Luminance":
  normalization divides a row by its sum, or rescales to the input luminance.
- GEGL channel-mixer.c (GIMP, LGPL-3.0-or-later; operations/common/channel-mixer.c) has `monochrome` and
  `preserve-luminosity` properties and a 3x3 gain matrix. Path unchecked: the raw URL tried (GNOME/gegl
  master) returned 404 on 2026-11-17; locate it in the GEGL tree before citing in code comments.

### (c) How it fits the code
Keys (Grade.h `gradekeys`): 9 matrix keys `mixRR, mixRG, mixRB, mixGR, mixGG, mixGB, mixBR, mixBG, mixBB`
(defaults identity; name = output then input), 3 grey keys `monoR, monoG, monoB` (0.3, 0.59, 0.11), and two
toggles stored as 0/1 params so they keyframe and undo like other fields: `mixMono`, `mixPreserveLum`
(default 0). Add `rgbMixerKeys()` / `isRgbMixerKey()` like `blurKeys()`; extend `defaultValue`, `allKeys()`.
Because they are in `allKeys()`, `isNodeIdentity` sees them; Pivot/LumMix-style exceptions are not needed
(the grey weights only matter when `mixMono` is on, so list `monoR/G/B` as "ignored unless mixMono", the
same trick as `Pivot`).
Maths (GradeMath.h): `struct MixerValues { float m[9]; bool mono; float grey[3]; bool preserveLum; static
MixerValues at(const GradeNode&, int64_t); bool isIdentity() const; };` and `void applyMixer(const
MixerValues&, float rgb[3])`: out = M in (or grey = w . in replicated); if `preserveLum`: Yin = luma709(in),
Yout = luma709(out), scale out by Yin / Yout when Yout > 1e-5 (else leave it); no clamping, like the
primaries. (The exact Resolve rule is not public; this is the simplest rule that holds luminance; record it
in the spec row as "our rule".)
Pipeline position: inside the corrector after `applyPrimaries` and before `applyCurves` (verify against the
manual's node-order text; if unspecified keep this and document). `PreparedGradeNode` gets `MixerValues
mixer; bool mixes;` and `corrects` must include `mixes` (GradeEval.cpp prepare ~80-110); both CPU loops
call `applyMixer`.
GPU (GlGrade.cpp `drawCorrector`, main ~364): uniforms `uniform mat3 uMixM; uniform vec3 uMixGrey; uniform
int uMixFlags;` plus a `mixes` skip; shader helper identical to `applyMixer` (float, same luma weights,
same epsilon). Add the uniform locations next to `m_u.winPoints` (~644).
UI: new `RgbMixerPanel` in ColorPalettes.h/.cpp (copy BlurPanel structure): output tab buttons Red / Green /
Blue, three ValueFields each (-2..2, step 0.01, keyframe diamond via the existing ValueField), Monochrome
and Preserve Luminance check buttons (when Monochrome is on the Red / Green / Blue tabs collapse to one
"Gray" row), Reset. Add to ColorPage `buildPalettes` stack and tab list ("RGB Mixer"); extend palette
reset. Spec row in docs/behavior-spec.md; menu-reference not needed (no menu item).

### (d) Tests
- tests/core (new test_rgbmixer.cpp or inside test_grade.cpp): default node is identity; swapping R and B
  rows swaps channels; Monochrome with (0.3, 0.59, 0.11) equals Rec. 709 luma on pure primaries; Preserve
  Luminance keeps luma709 within 1e-4 on 100 random colours for an arbitrary matrix; negative weights
  allowed; keyframed `mixRG` interpolates between frames.
- JSON round trip (the keys are plain params, so only the "mixMono" 0/1 value needs a check).
- tests/render/test_graderender.cpp: GPU vs CPU parity on a gradient image for one non-trivial matrix,
  for Monochrome, and for Preserve Luminance (same tolerance as the existing primaries parity).
- tests/app/test_colorpageui.cpp: tab shows nine fields in three tabs; typing a value is one undo step;
  Reset restores identity; Monochrome shows the grey row.

---------------------------------------------------------------------------------------------------

## 3. Color Warper palette (gap C21)

### (a) What Resolve does
- Manual (Color Warper, verify): the palette has two modes, "Hue - Saturation" and "Chroma - Luminance".
  Each shows a grid of control points ("mesh") over a colour chart; dragging a point moves the colours at
  that place to the new place: in Hue - Sat the horizontal axis is hue and the vertical axis is saturation,
  in Chroma - Luma the horizontal axis is chroma (colourfulness) and the vertical axis luminance. Points
  can be picked from the picture by clicking a colour in the viewer, "lock" points keep others fixed
  (verify), the grid size ("Points": a few preset densities, verify), and Reset (verify). Public tutorials
  use it to shift a hue (turn skin more orange), desaturate one colour only and brighten a colour range
  smoothly, "like many HSL curves in one".
- Numbers I am unsure of (default grid size, a Hue / Sat / Luma "strength" or Limit sliders): leave them
  out of the first cut or check them before building; list them in the spec row as "not copied".

### (b) Open source references
- No open-source tool copies the 2D mesh exactly (honest answer). Closest:
  - darktable colorzones.c, GPL-3.0-or-later: src/iop/colorzones.c. One spline per axis "select by"
    lightness / chroma / hue (enum at line 61), curve points edited per channel, applied in LCh with a
    pre-baked LUT per axis (`dt_draw_curve_calc_value`). It is the 1D version of what the Warper does and
    confirms the bake-to-table approach and periodic hue handling; works well in daily use.
  - darktable colorbalancergb.c, GPL-3.0-or-later (src/iop/colorbalancergb.c): JzCzhz chroma / hue / lum
    handling and gamut compression, useful for deciding the colour space of the chroma-luma axes.
  - In-repo precedent: `GradeCurves` HueHue / HueSat / HueLum / LumSat / SatSat (Grade.h 49-80), baked to
    `CurveTables` and applied in `applyCurves` (GradeMath.cpp ~150-200, periodic hue spline via
    `evaluateCurve`). The warper is a 2D generalization: same bake-then-lookup structure.
  - Mesh warp maths: a displacement field on a regular grid interpolated with Catmull-Rom / bilinear is the
    standard (Natron / OpenFX GridWarp-style; GPL-compatible examples exist but nothing worth copying);
    we write it ourselves, it is a few lines.

### (c) How it fits the code
Model: new `struct GradeWarper` in Grade.h, member `GradeWarper warper;` of `GradeNode` (next to `curves`,
`qualifier`, `track`; included in `operator==`): two meshes, `hueSat` and `chromaLuma`, each
`{int cols, rows; QVector<QPointF> offsets;}` where an offset is the displacement of that grid point in
chart units (default all zero = identity; grid size default 12 x 5 for Hue - Sat with hue periodic, 5 x 5
for Chroma - Luma; the grid size is fixed per mesh in version 1 so no re-sampling code). `isIdentity()` =
every offset zero; `GradeNode::hasKey` unaffected; `isNodeIdentity` (Grade.cpp ~485) adds
`!node.warper.isIdentity()`; `resetNode` clears it. v1 is static (like the qualifier): no keyframes
(Resolve keys it; list as "not done" in the roadmap line and the gaps table).
Maths (GradeMath.h): `struct WarperTables { std::array<float,2*W*H> hueSat; std::array<...> chromaLuma;
bool active; static WarperTables bake(const GradeWarper&); }` baked on the CPU once per frame from the mesh
(Catmull-Rom over the grid, hue axis wraps) to a 2D table, e.g. 64 x 32 RG floats, like `CurveTables::bake`.
`void applyWarper(const WarperTables&, float rgb[3])`: (1) Hue - Sat: take hue / saturation from the
existing `rgbToHsl` (hue in turns, lightness untouched), sample the table at (h, s) with bilinear
filtering, h' = h + dh, s' = clamp(s + ds), back to RGB with the existing HSL to RGB; (2) Chroma - Luma:
Y = Rec. 709 luma, Cb / Cr relative to Y, chroma c = hypot(Cb, Cr) normalised to 0..1 (c / 0.5, clamped),
sample at (c, Y), c' = c + dc, Y' = Y + dl, rebuild with the same hue angle. Both take their inputs from the
colour entering the stage (as `applyCurves` does) and never clamp RGB. Pin the outer ring: displacement
fades to zero at saturation 0 (grey stays grey in Hue - Sat) and at the chart edges so highlights do not
jump; write that rule in the spec.
Pipeline position: after `applyCurves`, before the LUT (document; Resolve's exact order is not public).
GPU: bake-to-texture like the curves: two `GL_RG32F` textures (`uWarpHS`, `uWarpCL`, 64 x 32) uploaded when
the mesh changes (same pattern as `m_lut1d` upload in GlGrade.cpp ~750 and the curve tables), `uniform int
uWarpMask`, GLSL `applyWarper` using `texture()` with linear filtering and wrap on S for hue; CPU does the
bilinear itself with the same texel-centre convention, as `CurveTables::sample` does ("the GPU fetches the
two samples and interpolates the same way"). The `PreparedGradeNode` gets `std::shared_ptr<const
WarperTables> warper` like `curves`; `corrects` includes it.
Serialization: GradeSerialization.cpp node object: "warper": {"hueSat": {"cols","rows","offsets":[dx,dy,...]},
"chromaLuma": {...}} written only when not identity; clamp cols / rows to sane limits (2..32) and offsets
to +-1 on load, ignore a mesh whose offsets count does not match.
UI: new `ColorWarperPanel` (ColorPalettes) with a mode switch (Hue - Sat / Chroma - Luma), a custom
`WarperChart` widget (new, src/ui/color/): paints the chart background (hue gradient x saturation, or
chroma x luma) with the warped mesh lines and draggable points; drag = one undo step labelled "Move
Warper Point"; double-click a point resets it; Reset button; optional "pick from viewer" using the existing
`QualifierPick` pipeline (QualifierPick.h gives picks from the viewer; map the picked colour to the nearest
grid point and select it). Add to ColorPage palette stack and tab list ("Color Warper") and to the palette
reset. Keep it to one mesh density in v1.

### (d) Tests
- tests/core (new test_colorwarper.cpp, add to groups.cmake): default node identity; mesh JSON round trip and
  clamping of bad files; zero offsets leave a random colour set within 1e-5; a single Hue - Sat point moved by
  +0.05 hue shifts a colour at that point by about 0.05 turns and leaves colours two grid cells away within
  1e-3; grey stays grey; hue wrap: a point on the seam moves colours on both sides; Chroma - Luma +dl on
  the middle point raises luma709 of mid colours and keeps highlights near 1.
- Table bake: bilinear lookup of the baked table equals direct Catmull-Rom evaluation within 1e-3.
- tests/render/test_graderender.cpp: GPU vs CPU parity on a hue-sweep image with a non-trivial mesh in both
  modes; a node with only a warper counts as `corrects` and is skipped when identity.
- tests/app/test_colorpageui.cpp: mode switch, dragging a point is one undo step, Reset clears both
  meshes, chart repaints, node shows the "graded" dot in the node editor after a warp.

---------------------------------------------------------------------------------------------------

## Suggested order and sizes
1. RGB Mixer first (smallest: keys + one matrix, mirrors the Blur palette wiring). Roughly a day of agent work.
2. Curve window: flatten in `evaluateWindows` makes GPU / CPU free; the work is model + shape keyframes +
   overlay tangent editing. Two to three days; do the model and tests before the overlay.
3. Color Warper last (new model, bake, textures, a custom chart widget). Two to three days.
One project format bump covers all three (warper node member, window point struct and shapeKeys; mixer keys
are plain params). Do each as its own commit with its spec row; keep builds to max two parallel agents.
Open questions to settle against the manual text before coding: exact Preserve Luminance rule, default
Monochrome weights, Warper grid defaults and extra controls, whether Curve window points have the same
Softness fields as Polygon, node-order position of Mixer and Warper.
