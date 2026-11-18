// SPDX-License-Identifier: GPL-3.0-or-later
// TextPlus — the per-glyph text layout of the Compositing page's Text node (spec section 116):
// Frame, Circle and Path layouts, kerning, the Follower's per-character transforms and extra
// shading elements. TitleRenderer calls it when TitleStyle::plus.active().
#pragma once

#include "render/TitleRenderer.h"

#include <QFont>
#include <QTransform>

#include <vector>

namespace kk::render {

/// One drawn character (a grapheme cluster) of the laid-out text.
struct GlyphPlace
{
    QString text; ///< the cluster
    int index = 0; ///< index among the clusters (line breaks not counted): kerning / Follower
    bool shown = true; ///< false when Write On hides it (it still takes its place)
    bool emoji = false; ///< drawn as a colour glyph, without the shading elements
    double advance = 0; ///< width of the glyph in output pixels
    double opacity = 1.0; ///< the Follower's opacity
    /// Maps glyph space (origin at the left end of the baseline, y down) onto the canvas
    /// (output pixels), including the block rotation and the Follower's transform.
    QTransform xf;
};

struct TextPlusLayout
{
    QFont font;
    QFont emojiFont;
    std::vector<GlyphPlace> glyphs;
};

/// Lays out @p s.text for the layout in @p s.plus. Pure: the same style gives the same layout.
[[nodiscard]] TextPlusLayout layoutTextPlus(const TitleStyle& s);

/// Rasterises @p s with the layout above (shading elements back to front, then the main
/// look: shadow, outline, fill). Same result type as renderTitle().
[[nodiscard]] TitleImage renderTextPlus(const TitleStyle& s);

/// The point and direction (unit vector) at arc length @p s along the polyline @p pts (canvas
/// pixels); beyond either end the end segments continue straight.
struct PathSample
{
    QPointF pos;
    QPointF dir;
};
[[nodiscard]] PathSample samplePolyline(const std::vector<QPointF>& pts, double s);
[[nodiscard]] double polylineLength(const std::vector<QPointF>& pts);

} // namespace kk::render
