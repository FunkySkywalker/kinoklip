// SPDX-License-Identifier: GPL-3.0-or-later
// TextPlus — see TextPlus.h. The idea (a glyph placed at an arc length of a path, turned to the
// path's tangent, with its baseline on the path) is the usual text-on-path layout, as in
// Glaxnimate's text-on-path (GPL-3.0-or-later); the code is our own.
#include "render/TextPlus.h"

#include "render/EmojiText.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

constexpr int kAaMargin = 2;
constexpr int kMaxCanvasMultiple = 3;
constexpr double kShadowOffsetEm = 0.05;
constexpr double kMinShadowPx = 2.0;
constexpr int kShadowAlpha = 150;

struct Line
{
    int offset = 0; ///< UTF-16 offset in the whole text
    QString text;
};

QPointF unit(QPointF v)
{
    const double l = std::hypot(v.x(), v.y());
    return l > 1e-12 ? QPointF(v.x() / l, v.y() / l) : QPointF(1, 0);
}

} // namespace

double polylineLength(const std::vector<QPointF>& pts)
{
    double len = 0;
    for (size_t i = 1; i < pts.size(); ++i)
        len += std::hypot(pts[i].x() - pts[i - 1].x(), pts[i].y() - pts[i - 1].y());
    return len;
}

PathSample samplePolyline(const std::vector<QPointF>& pts, double s)
{
    PathSample out;
    if (pts.size() < 2) {
        out.pos = pts.empty() ? QPointF() : pts.front();
        out.dir = QPointF(1, 0);
        return out;
    }
    double acc = 0;
    size_t firstSeg = 0;
    for (size_t i = 1; i < pts.size(); ++i) {
        if (std::hypot(pts[i].x() - pts[i - 1].x(), pts[i].y() - pts[i - 1].y()) > 0) {
            firstSeg = i;
            break;
        }
    }
    if (firstSeg == 0) {
        out.pos = pts.front();
        out.dir = QPointF(1, 0);
        return out;
    }
    if (s <= 0) {
        out.dir = unit(pts[firstSeg] - pts[firstSeg - 1]);
        out.pos = pts[0] + out.dir * s;
        return out;
    }
    for (size_t i = 1; i < pts.size(); ++i) {
        const QPointF d = pts[i] - pts[i - 1];
        const double l = std::hypot(d.x(), d.y());
        if (l <= 0)
            continue;
        out.dir = unit(d);
        out.pos = pts[i - 1] + out.dir * (s - acc);
        if (s <= acc + l)
            return out;
        acc += l;
    }
    return out; // past the end: the last segment continues
}

TextPlusLayout layoutTextPlus(const TitleStyle& s)
{
    TextPlusLayout out;
    const TextPlus& x = s.plus;
    emoji::registerBundledFont();
    QFont font(s.font);
    const double px = s.size * s.scale;
    font.setPixelSize(std::max(1, int(std::lround(px))));
    font.setBold(s.bold);
    font.setItalic(s.italic);
    font.setLetterSpacing(QFont::PercentageSpacing, std::max(1.0, 100.0 + s.tracking));
    font.setHintingPreference(QFont::PreferNoHinting);
    QFont emojiFont = font;
    const bool hasEmoji = emoji::containsEmoji(s.text);
    if (hasEmoji)
        emojiFont.setFamilies(emoji::familiesWithEmoji(s.font));
    out.font = font;
    out.emojiFont = emojiFont;
    const QFontMetricsF fm(font);
    const QFontMetricsF em(emojiFont);
    const double ascent = fm.ascent();
    const double lineStep = fm.lineSpacing() * s.lineSpacing / 100.0;
    const double cx = s.posX * s.canvas.width();
    const double cy = (1.0 - s.posY) * s.canvas.height();

    // Index of the cluster that starts at a UTF-16 unit of the text (-1 inside a cluster or on
    // a line break); line breaks are not counted.
    QVector<int> clusterAt(int(s.text.size()) + 1, -1);
    {
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, s.text);
        int from = 0, count = 0;
        for (int to = finder.toNextBoundary(); to > 0; from = to, to = finder.toNextBoundary()) {
            const QString piece = s.text.mid(from, to - from);
            if (piece == QLatin1String("\n") || piece == QLatin1String("\r")
                || piece == QLatin1String("\r\n"))
                continue;
            clusterAt[from] = count++;
        }
    }
    const QBitArray shown = writeOnVisible(s.text, s.writeOnStart, s.writeOnEnd);
    const auto kern = [&](int idx) {
        return idx >= 0 && size_t(idx) < x.kerning.size() ? x.kerning[size_t(idx)] * px / 100.0
                                                           : 0.0;
    };

    // Lines: the paragraphs, wrapped at the frame width for the Frame layout.
    std::vector<Line> lines;
    const bool wrap = x.layout == TextPlus::Layout::Frame && x.frameWidth > 0;
    int base = 0;
    for (const QString& para : s.text.split(QLatin1Char('\n'))) {
        if (!wrap) {
            lines.push_back({base, para});
        } else {
            int start = 0, lastSpace = -1;
            for (int i = 0; i < para.size(); ++i) {
                if (para[i] == QLatin1Char(' '))
                    lastSpace = i;
                if (i > start && em.horizontalAdvance(para.mid(start, i - start + 1)) > x.frameWidth) {
                    if (lastSpace >= start) {
                        lines.push_back({base + start, para.mid(start, lastSpace - start)});
                        start = lastSpace + 1;
                    } else {
                        lines.push_back({base + start, para.mid(start, i - start)});
                        start = i;
                    }
                    lastSpace = -1;
                }
            }
            lines.push_back({base + start, para.mid(start)});
        }
        base += int(para.size()) + 1;
    }

    struct Cl
    {
        int from, len, idx;
        double x, w;
    };
    std::vector<std::vector<Cl>> lineClusters(lines.size());
    std::vector<double> widths(lines.size(), 0.0);
    for (size_t li = 0; li < lines.size(); ++li) {
        const Line& ln = lines[li];
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, ln.text);
        int from = 0;
        double cum = 0;
        for (int to = finder.toNextBoundary(); to > 0; from = to, to = finder.toNextBoundary()) {
            Cl c;
            c.from = from;
            c.len = to - from;
            c.idx = clusterAt[std::min(ln.offset + from, int(s.text.size()))];
            c.x = em.horizontalAdvance(ln.text.left(from)) + cum;
            c.w = em.horizontalAdvance(ln.text.mid(from, to - from));
            cum += kern(c.idx);
            lineClusters[li].push_back(c);
        }
        widths[li] = em.horizontalAdvance(ln.text) + cum;
    }

    const double blockHeight = ascent + fm.descent() + double(lines.size() - 1) * lineStep;
    const double frac = s.align == 0 ? 0.0 : s.align == 1 ? 0.5 : 1.0;
    TextPlus::Layout layout = x.layout;
    const double pathLen = polylineLength(x.pathPoints);
    if (layout == TextPlus::Layout::Path && (x.pathPoints.size() < 2 || pathLen <= 0))
        layout = TextPlus::Layout::Point;

    QTransform turn;
    turn.translate(cx, cy);
    turn.rotate(s.rotation);
    turn.translate(-cx, -cy);

    for (size_t li = 0; li < lines.size(); ++li) {
        const double lw = widths[li];
        const double row = double(li);
        for (const Cl& c : lineClusters[li]) {
            GlyphPlace g;
            g.text = lines[li].text.mid(c.from, c.len);
            g.index = c.idx;
            g.advance = c.w;
            const int unitAt = lines[li].offset + c.from;
            g.shown = unitAt >= shown.size() || shown.testBit(unitAt);
            g.emoji = hasEmoji && emoji::containsEmoji(g.text);
            QTransform t;
            switch (layout) {
            case TextPlus::Layout::Circle: {
                const double r = std::max(1.0, x.circleRadius - row * lineStep);
                const double arc = c.x + c.w * 0.5 - lw * frac;
                const double phi = x.circleAngle * M_PI / 180.0 - arc / r;
                const QPointF p(cx + r * std::cos(phi), cy - r * std::sin(phi));
                // Clockwise on screen: the tangent is (sin phi, cos phi).
                t.translate(p.x(), p.y());
                t.rotate(std::atan2(std::cos(phi), std::sin(phi)) * 180.0 / M_PI);
                t.translate(-c.w * 0.5, 0);
                break;
            }
            case TextPlus::Layout::Path: {
                const double at = (pathLen - lw) * frac + x.pathOffset * pathLen + c.x + c.w * 0.5;
                const PathSample ps = samplePolyline(x.pathPoints, at);
                t.translate(ps.pos.x(), ps.pos.y());
                t.rotate(std::atan2(ps.dir.y(), ps.dir.x()) * 180.0 / M_PI);
                t.translate(-c.w * 0.5, row * lineStep);
                break;
            }
            default: {
                double left, top;
                if (layout == TextPlus::Layout::Frame) {
                    const double fw = x.frameWidth > 0 ? x.frameWidth : lw;
                    const double fh = x.frameHeight > 0 ? x.frameHeight : blockHeight;
                    left = cx - fw / 2 + (fw - lw) * frac;
                    top = x.justifyV == 0 ? cy - fh / 2
                        : x.justifyV == 2 ? cy + fh / 2 - blockHeight
                                          : cy - blockHeight / 2;
                } else {
                    left = cx - lw * frac;
                    top = cy - blockHeight / 2;
                }
                t.translate(left + c.x, top + ascent + row * lineStep);
                break;
            }
            }
            // The Follower: move, turn and scale the character about its middle.
            if (c.idx >= 0 && size_t(c.idx) < x.chars.size()) {
                const TextPlus::CharXf& f = x.chars[size_t(c.idx)];
                g.opacity = std::clamp(f.opacity, 0.0, 1.0);
                const QPointF mid(c.w * 0.5, -ascent * 0.35);
                QTransform local;
                local.translate(mid.x(), mid.y());
                local.rotate(f.angle);
                local.scale(f.size, f.size);
                local.translate(-mid.x(), -mid.y());
                QTransform move;
                move.translate(f.dx * s.scale, f.dy * s.scale);
                t = local * t * move;
            }
            g.xf = t * turn;
            out.glyphs.push_back(std::move(g));
        }
    }
    return out;
}

TitleImage renderTextPlus(const TitleStyle& s)
{
    TitleImage result;
    if (s.text.isEmpty() || s.canvas.isEmpty())
        return result;
    const TextPlusLayout layout = layoutTextPlus(s);
    struct Drawn
    {
        QPainterPath path;
        double opacity;
    };
    std::vector<Drawn> drawn;
    std::vector<const GlyphPlace*> emojis;
    QRectF bounds;
    const double px = s.size * s.scale;
    const QFontMetricsF emojiMetrics(layout.emojiFont);
    for (const GlyphPlace& g : layout.glyphs) {
        if (!g.shown || g.text.trimmed().isEmpty())
            continue;
        if (g.emoji) {
            bounds = bounds.united(g.xf.mapRect(QRectF(
                0, -emojiMetrics.ascent(), g.advance, emojiMetrics.ascent() + emojiMetrics.descent())));
            emojis.push_back(&g);
            continue;
        }
        QPainterPath p;
        p.addText(0, 0, layout.font, g.text);
        p = g.xf.map(p);
        bounds = bounds.united(p.boundingRect());
        drawn.push_back({std::move(p), g.opacity});
    }
    if (drawn.empty() && emojis.empty())
        return result;

    double grow = std::max(0.0, s.outlineWidth * s.scale);
    for (const TextPlus::Shade& sh : s.plus.shades) {
        grow = std::max(
            grow, std::abs(sh.dx * s.scale) + std::abs(sh.dy * s.scale) + sh.thickness * s.scale);
    }
    const double shadowOffset = s.shadow ? std::max(kMinShadowPx, px * kShadowOffsetEm) : 0.0;
    bounds.adjust(-grow - kAaMargin, -grow - kAaMargin, grow + shadowOffset + kAaMargin,
        grow + shadowOffset + kAaMargin);
    const QRectF limit(-s.canvas.width() * (kMaxCanvasMultiple - 1) * 0.5,
        -s.canvas.height() * (kMaxCanvasMultiple - 1) * 0.5,
        s.canvas.width() * double(kMaxCanvasMultiple),
        s.canvas.height() * double(kMaxCanvasMultiple));
    const QRect pixelRect = bounds.intersected(limit).toAlignedRect();
    if (pixelRect.isEmpty())
        return result;

    QImage image(pixelRect.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    painter.translate(-pixelRect.topLeft());

    const auto strokeAll = [&](const QPen& pen, double dx, double dy, double alpha) {
        for (const Drawn& d : drawn) {
            painter.setOpacity(alpha * d.opacity);
            painter.strokePath(d.path.translated(dx, dy), pen);
        }
    };
    const auto fillAll = [&](const QColor& color, double dx, double dy, double alpha) {
        for (const Drawn& d : drawn) {
            painter.setOpacity(alpha * d.opacity);
            painter.fillPath(d.path.translated(dx, dy), color);
        }
    };
    for (const TextPlus::Shade& sh : s.plus.shades) {
        const double dx = sh.dx * s.scale, dy = sh.dy * s.scale;
        if (sh.outline) {
            strokeAll(QPen(sh.color, std::max(0.0, sh.thickness * s.scale), Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin),
                dx, dy, sh.opacity);
        } else {
            fillAll(sh.color, dx, dy, sh.opacity);
        }
    }
    const double outline = s.outlineWidth * s.scale;
    if (s.shadow) {
        const QColor shadowColor(0, 0, 0, kShadowAlpha);
        if (outline > 0)
            strokeAll(QPen(shadowColor, 2.0 * outline, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin),
                shadowOffset, shadowOffset, 1.0);
        fillAll(shadowColor, shadowOffset, shadowOffset, 1.0);
    }
    if (outline > 0)
        strokeAll(QPen(s.outlineColor, 2.0 * outline, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin),
            0, 0, 1.0);
    fillAll(s.color, 0, 0, 1.0);
    for (const GlyphPlace* g : emojis) {
        painter.save();
        painter.setOpacity(g->opacity);
        painter.setWorldTransform(
            g->xf * QTransform::fromTranslate(-pixelRect.x(), -pixelRect.y()));
        painter.setFont(layout.emojiFont);
        painter.setPen(s.color);
        painter.drawText(QPointF(0, 0), g->text);
        painter.restore();
    }
    painter.end();
    result.image = std::move(image);
    result.rect = QRectF(pixelRect);
    return result;
}

} // namespace kk::render
