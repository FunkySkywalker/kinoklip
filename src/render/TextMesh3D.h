// SPDX-License-Identifier: GPL-3.0-or-later
// TextMesh3D — the mesh of extruded, bevelled text for the Text3D node (spec 130, 1.13): glyph
// outlines (a QPainterPath) become polygons, the caps are triangulated with earcut (ISC licence,
// third_party/earcut), and the side walls and the bevel rings are built along the outlines the
// way three.js' ExtrudeGeometry and Blender's text extrude / bevel do it: every outline vertex is
// moved inwards along its miter vector for each step of the bevel profile.
#pragma once

#include "render/CompScene3D.h"

#include <QPainterPath>
#include <QTransform>

namespace kk::render::scene3d {

/// Profile of the bevel: Smooth is a quarter circle (rounded edge), Flat a single chamfer with
/// one constant normal.
enum class BevelStyle : quint8 { Smooth, Flat };

struct TextExtrusion
{
    double depth = 0; ///< total thickness, scene units; <= 0 makes no mesh (flat text)
    double bevelDepth = 0; ///< how far a bevel reaches into the thickness (at most half of depth)
    double bevelWidth = 0; ///< how far a bevel reaches into the letter face
    BevelStyle style = BevelStyle::Smooth;
    int segments = 4; ///< steps of a smooth bevel
};

/// The four surfaces of extruded text, so each can have its own material. All in scene space with
/// the front face at z = +depth / 2 and the back at z = -depth / 2, facing out.
struct ExtrudedText
{
    Mesh front; ///< the front cap, normal +Z
    Mesh back; ///< the back cap, normal -Z
    Mesh side; ///< the walls
    Mesh bevel; ///< the front and back bevel rings
    double bevelWidthUsed = 0; ///< the bevel width after it was reduced to fit thin strokes
    [[nodiscard]] bool isEmpty() const { return front.positions.isEmpty(); }
};

/// Extrudes @p outline (filled with the winding rule) after mapping its flattened outlines with
/// @p toScene (which must map to y up). Holes of letters (O, A, B ...) are found by nesting.
[[nodiscard]] ExtrudedText extrudeText(
    const QPainterPath& outline, const QTransform& toScene, const TextExtrusion& options);

} // namespace kk::render::scene3d
