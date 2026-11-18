// SPDX-License-Identifier: GPL-3.0-or-later
// TitleRenderer — rasterises a Title clip's text (outline, shadow, background box, alignment,
// size/tracking/line spacing) with QPainter into a tightly cropped QImage.
#pragma once

#include "core/TimelineData.h"

#include <QBitArray>
#include <QColor>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>

#include <QPointF>

#include <cstdint>
#include <vector>

namespace kk::render {

/// render::fonts::fontGeneration() (render/FontReload.h).
[[nodiscard]] int currentFontGeneration();

/// The extra controls of the Compositing page's Text node (spec section 116): a layout other
/// than a plain block (frame, circle, path), per-character kerning, extra shading elements and
/// the Follower's per-character transforms. A default value changes nothing; `active()` tells
/// the renderer to use the per-glyph layout of render/TextPlus.h instead of the plain one.
struct TextPlus
{
    enum class Layout : quint8 {
        Point,
        Frame,
        Circle,
        Path
    };
    /// One extra shading element: a fill or an outline of the glyphs, drawn behind the main
    /// look (the style's colour, outline and shadow).
    struct Shade
    {
        bool outline = false;
        QColor color;
        double opacity = 1.0; ///< 0..1
        double thickness = 2.0; ///< outline width in px at timeline resolution
        double dx = 0, dy = 0; ///< offset in px at timeline resolution, y down
        friend bool operator==(const Shade&, const Shade&) = default;
    };
    /// The Follower's transform of one character (index = grapheme cluster, line breaks not
    /// counted): offset in px at timeline resolution (y down), size factor, angle in degrees
    /// clockwise, opacity 0..1.
    struct CharXf
    {
        double dx = 0, dy = 0, size = 1.0, angle = 0.0, opacity = 1.0;
        friend bool operator==(const CharXf&, const CharXf&) = default;
    };

    Layout layout = Layout::Point;
    int justifyV = 1; ///< Frame: 0 = top, 1 = centre, 2 = bottom
    double frameWidth = 0, frameHeight = 0; ///< Frame, output pixels
    double circleRadius = 0; ///< Circle, output pixels
    double circleAngle = 90; ///< Circle: degrees counter-clockwise from the right; 90 = the top
    double pathOffset = 0; ///< Path: shift along the path, fraction of its length
    /// Path: the flattened path, canvas pixels (the title canvas, y down).
    std::vector<QPointF> pathPoints;
    std::vector<double> kerning; ///< percent of the font size added after each character
    std::vector<Shade> shades; ///< back to front
    std::vector<CharXf> chars; ///< empty = no Follower

    /// True if the plain block layout cannot draw this.
    [[nodiscard]] bool active() const
    {
        return layout != Layout::Point || !kerning.empty() || !shades.empty() || !chars.empty();
    }
    friend bool operator==(const TextPlus&, const TextPlus&) = default;
};

/// Everything that influences a title's pixels at one frame, resolved from ClipData::title.
/// Used as the title texture cache key: equal styles produce identical images, so a static
/// title is rasterised once and then costs nothing per frame.
struct TitleStyle
{
    /// What the layer draws: its text (default) or a shape (spec section 68).
    enum class Kind {
        Text,
        Shape
    };
    /// Shape layers: the outline drawn.
    enum class Shape {
        Rectangle,
        Rounded,
        Ellipse,
        Line
    };

    Kind kind = Kind::Text;
    /// render::fonts::fontGeneration() when the style was made: a title cached before a font
    /// was installed is drawn again with it.
    int fontGeneration = currentFontGeneration();
    QString text;
    QString font;
    bool bold = false;
    bool italic = false;
    int align = 1; ///< 0 = left, 1 = centre, 2 = right (lines inside the text block)
    QColor color;
    QColor outlineColor;
    QColor bgColor;
    bool bgEnabled = false;
    bool shadow = false;
    double size = 72; ///< font pixel size at timeline resolution
    double tracking = 0; ///< percent letter spacing added (0 = normal)
    double lineSpacing = 100; ///< percent of the font's natural line spacing
    double writeOnStart = 0; ///< Write On: fraction of the characters hidden at the start
    double writeOnEnd = 1; ///< Write On: characters after this fraction are hidden
    double outlineWidth = 0; ///< px at timeline resolution, drawn outside the glyphs
    double bgOpacity = 60; ///< percent
    double posX = 0.5; ///< anchor point of the text block, 0..1 of the canvas from the left
    double posY = 0.5; ///< centre of the text block, 0..1 of the canvas from the bottom
    int anchor = 1; ///< 0 = block's left edge at posX, 1 = its centre, 2 = its right edge
    bool scroll = false; ///< roll up through the canvas (posY unused)
    double scrollProgress = 0; ///< 0 = just below the bottom edge, 1 = just above the top
    QSize canvas; ///< size of the full title frame in OUTPUT pixels (timeline size × scale)
    double scale = 1.0; ///< output pixels per timeline pixel
    double rotation = 0; ///< degrees clockwise around the layer's centre
    double opacity = 100; ///< percent, of the whole layer
    // Shape layers: the centre is posX / posY, the fill colour `color`, the stroke
    // `outlineColor` / `outlineWidth`.
    Shape shape = Shape::Rectangle;
    double shapeWidth = 400; ///< px at timeline resolution
    double shapeHeight = 200;
    double cornerRadius = 0; ///< px at timeline resolution (rounded rectangles)
    double fillOpacity = 100; ///< percent
    /// A title with several layers: the shown layers bottom to top (each a plain Text or Shape
    /// style on the same canvas and scale). When not empty the fields above are not used.
    std::vector<TitleStyle> stack;
    /// Compositing page Text node only (spec section 116).
    TextPlus plus;

    friend bool operator==(const TitleStyle&, const TitleStyle&) = default;
};

/// Write On: which UTF-16 units of @p text are drawn when the range @p start..@p end (0..1) of
/// its characters is shown. Characters are grapheme clusters (an emoji, a letter with its
/// combining marks), spaces count, line breaks do not. Cluster i of n is shown when
/// round(start * n) <= i < round(end * n). Hidden characters keep their place in the layout.
[[nodiscard]] QBitArray writeOnVisible(const QString& text, double start, double end);

/// Hash of a TitleStyle (for the texture cache).
[[nodiscard]] size_t qHash(const TitleStyle& style, size_t seed = 0);

/// Resolves the title parameters of @p clip at clip-local frame @p localFrame (defaults from
/// kk::keys / the text_title descriptor) for a canvas of @p canvas output pixels at @p scale
/// output pixels per timeline pixel. Cheap: no allocation (QString copies are ref-counted).
[[nodiscard]] TitleStyle resolveTitleStyle(
    const ClipData& clip, int64_t localFrame, QSize canvas, double scale);

/// A rasterised title: `image` (ARGB32_Premultiplied) covers `rect` of the title canvas.
/// The text block sits at posX / posY of the canvas (centred by default) or, for a scroll, at
/// its scrollProgress; the clip transform positions the whole canvas from there.
/// `image` is null when there is nothing to draw (empty text).
struct TitleImage
{
    QImage image;
    QRectF rect; ///< position of `image` inside the canvas, output pixels
};

/// Resolves one layer: the parameters @p p (a Title clip's first layer, or an extra layer of
/// kind @p kind) at clip-local frame @p localFrame.
[[nodiscard]] TitleStyle resolveLayerStyle(const ParamSet& p, TitleLayerKind kind,
    const ClipData& clip, int64_t localFrame, QSize canvas, double scale);

/// Rasterises @p style. Thread-safe (QPainter on a QImage); takes ~1-5 ms at 1080p.
[[nodiscard]] TitleImage renderTitle(const TitleStyle& style);

} // namespace kk::render
