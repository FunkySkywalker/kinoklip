// SPDX-License-Identifier: GPL-3.0-or-later
// Text+ controls of the Compositing page's Text node (spec section 116): Frame, Circle and Path
// layouts, tracking, line spacing, kerning, shading elements, the Follower, serialization.
#include "core/CompOps.h"
#include "core/Serialization.h"
#include "core/NodeRegistry.h"
#include "render/CompMath.h"
#include "render/TextPlus.h"
#include "render/TitleRenderer.h"

#include <QGuiApplication>
#include <QJsonObject>
#include <QTest>

#include <cmath>

using namespace kk;
using namespace kk::render;
namespace co = kk::compops;
namespace k = kk::nodekeys;

namespace {

constexpr QSize kCanvas(1280, 720);
constexpr QSizeF kFrame(1280, 720);

ParamSet textParams()
{
    ParamSet p = NodeRegistry::instance().defaultParams(nodetypes::Text);
    p.statics[k::Text] = QStringLiteral("Hello World");
    p.statics[k::Font] = QStringLiteral("DejaVu Sans");
    p.setBase(k::FontSize, 60);
    return p;
}

TitleStyle styleOf(const ParamSet& p, int64_t frame = 0, const AnimatedPath* path = nullptr)
{
    QPointF offset;
    return compmath::textStyle(p, frame, kCanvas, 1.0, kFrame, &offset, path);
}

QPointF middleOf(const GlyphPlace& g)
{
    return g.xf.map(QPointF(g.advance / 2, 0));
}

} // namespace

class TextPlusTest : public QObject
{
    Q_OBJECT
private slots:
    void defaultsChangeNothing()
    {
        // A Text node as old projects have it: the plain block layout, no Text+ work.
        const TitleStyle s = styleOf(textParams());
        QVERIFY(!s.plus.active());
        QCOMPARE(s.align, 1);
        QCOMPARE(s.tracking, 0.0);
        QCOMPARE(s.lineSpacing, 100.0);
        QCOMPARE(s.rotation, 0.0);
        ParamSet old;
        old.statics[k::Text] = QStringLiteral("x");
        QVERIFY(!styleOf(old).plus.active()); // no Text+ keys at all
    }

    void polylineSampling()
    {
        const std::vector<QPointF> pts {{0, 0}, {10, 0}, {10, 10}};
        QCOMPARE(polylineLength(pts), 20.0);
        PathSample a = samplePolyline(pts, 5);
        QCOMPARE(a.pos, QPointF(5, 0));
        QCOMPARE(a.dir, QPointF(1, 0));
        PathSample b = samplePolyline(pts, 15);
        QCOMPARE(b.pos, QPointF(10, 5));
        QCOMPARE(b.dir, QPointF(0, 1));
        // Beyond the ends the end segments continue.
        QCOMPARE(samplePolyline(pts, -3).pos, QPointF(-3, 0));
        QCOMPARE(samplePolyline(pts, 23).pos, QPointF(10, 13));
    }

    void circleLayoutPutsGlyphsOnTheCircle()
    {
        ParamSet p = textParams();
        p.statics[k::LayoutType] = textlayouts::Circle;
        p.setBase(k::CircleRadius, 0.2); // 256 px
        p.setBase(k::CircleAngle, 90);
        const TitleStyle s = styleOf(p);
        QCOMPARE(s.plus.layout, TextPlus::Layout::Circle);
        const TextPlusLayout layout = layoutTextPlus(s);
        QVERIFY(layout.glyphs.size() >= 11);
        const QPointF centre(kCanvas.width() / 2.0, kCanvas.height() / 2.0);
        const double r = 0.2 * kFrame.width();
        int checked = 0;
        for (const GlyphPlace& g : layout.glyphs) {
            if (g.text.trimmed().isEmpty())
                continue;
            // The middle of the glyph's baseline is on the circle (chord error: a few px).
            const QPointF m = middleOf(g) - centre;
            QVERIFY2(std::abs(std::hypot(m.x(), m.y()) - r) < 3.0, qPrintable(g.text));
            ++checked;
        }
        QVERIFY(checked >= 10);
        // Centre justified: the middle of the text is at the top (Circle Angle 90): the glyph
        // closest to the top has x near the centre and is upright.
        const GlyphPlace* top = nullptr;
        for (const GlyphPlace& g : layout.glyphs)
            if (!g.text.trimmed().isEmpty() && (!top || middleOf(g).y() < middleOf(*top).y()))
                top = &g;
        QVERIFY(top);
        QVERIFY(std::abs(middleOf(*top).x() - centre.x()) < 40);
        QVERIFY(std::abs(top->xf.m12()) < 0.25); // little rotation
        // The text runs clockwise: the first letter is left of the last one.
        QVERIFY(middleOf(layout.glyphs.front()).x() < middleOf(layout.glyphs.back()).x());
        // A glyph on the right side of the circle is turned about a quarter turn clockwise.
        // Start further round: Circle Angle 0 puts the text's middle at the right.
        p.setBase(k::CircleAngle, 0);
        const TextPlusLayout right = layoutTextPlus(styleOf(p));
        const GlyphPlace& mid = right.glyphs[right.glyphs.size() / 2];
        QVERIFY(middleOf(mid).x() > centre.x() + r * 0.9);
        const double turn = std::atan2(mid.xf.m12(), mid.xf.m11()) * 180.0 / M_PI;
        QVERIFY2(turn > 70 && turn < 110, qPrintable(QString::number(turn)));
    }

    void pathLayoutFollowsThePath()
    {
        ParamSet p = textParams();
        p.statics[k::LayoutType] = textlayouts::Path;
        // A straight 45 degree line through the centre (points are px around the Center).
        AnimatedPath path;
        MaskPath pts;
        MaskPoint a, b;
        a.x = -400;
        a.y = -400;
        b.x = 400;
        b.y = 400;
        pts << a << b;
        path.shapes = {pts};
        const TitleStyle s = styleOf(p, 0, &path);
        QCOMPARE(s.plus.layout, TextPlus::Layout::Path);
        QVERIFY(s.plus.pathPoints.size() >= 2);
        const TextPlusLayout layout = layoutTextPlus(s);
        const QPointF centre(kCanvas.width() / 2.0, kCanvas.height() / 2.0);
        for (const GlyphPlace& g : layout.glyphs) {
            if (g.text.trimmed().isEmpty())
                continue;
            const QPointF m = middleOf(g) - centre;
            QVERIFY2(std::abs(m.x() - m.y()) < 0.5, qPrintable(g.text)); // on the line y = x
            const double turn = std::atan2(g.xf.m12(), g.xf.m11()) * 180.0 / M_PI;
            QVERIFY(std::abs(turn - 45.0) < 0.1);
        }
        // Position on Path moves the text along the line.
        const QPointF before = middleOf(layout.glyphs.front());
        p.setBase(k::PathOffset, 0.1);
        const QPointF after = middleOf(layoutTextPlus(styleOf(p, 0, &path)).glyphs.front());
        const double moved = std::hypot(after.x() - before.x(), after.y() - before.y());
        QVERIFY(std::abs(moved - 0.1 * std::hypot(800.0, 800.0)) < 0.5);
        // Left justify starts at the beginning of the path.
        p.setBase(k::PathOffset, 0);
        p.statics[k::Justify] = justifies::Start;
        const QPointF first = layoutTextPlus(styleOf(p, 0, &path)).glyphs.front().xf.map(QPointF(0, 0));
        QVERIFY(std::abs(first.x() - (centre.x() - 400)) < 0.5);
    }

    void pathWithTangentsIsFlattenedAsACurve()
    {
        ParamSet p = textParams();
        p.statics[k::LayoutType] = textlayouts::Path;
        const AnimatedPath arc = co::defaultTextPath();
        const TitleStyle s = styleOf(p, 0, &arc);
        QVERIFY(s.plus.pathPoints.size() > 8); // curved: more than the 3 points
        // The middle point of the default arc sits above the ends (y down: smaller y).
        double minY = 1e9, endY = s.plus.pathPoints.front().y();
        for (const QPointF& pt : s.plus.pathPoints)
            minY = std::min(minY, pt.y());
        QVERIFY(minY < endY - 50);
    }

    void frameLayoutWrapsAndJustifies()
    {
        ParamSet p = textParams();
        p.statics[k::Text] = QStringLiteral("one two three four five six seven eight");
        p.statics[k::LayoutType] = textlayouts::Frame;
        p.setBase(k::FrameWidth, 0.25); // 320 px
        p.setBase(k::FrameHeight, 0.5);
        const TitleStyle s = styleOf(p);
        const TextPlusLayout layout = layoutTextPlus(s);
        double top = 1e9, bottom = -1e9, left = 1e9, right = -1e9;
        for (const GlyphPlace& g : layout.glyphs) {
            if (g.text.trimmed().isEmpty())
                continue;
            const QPointF o = g.xf.map(QPointF(0, 0));
            const QPointF e = g.xf.map(QPointF(g.advance, 0));
            top = std::min(top, o.y());
            bottom = std::max(bottom, o.y());
            left = std::min({left, o.x(), e.x()});
            right = std::max({right, o.x(), e.x()});
        }
        QVERIFY(bottom - top > 100); // several lines
        const double centreX = kCanvas.width() / 2.0;
        QVERIFY(left >= centreX - 160 - 1 && right <= centreX + 160 + 1);
        // Top / Bottom justify move the block inside the frame.
        p.setBase(k::FrameHeight, 1.0);
        p.statics[k::JustifyV] = justifies::Start;
        const TextPlusLayout topL = layoutTextPlus(styleOf(p));
        p.statics[k::JustifyV] = justifies::End;
        const TextPlusLayout botL = layoutTextPlus(styleOf(p));
        QVERIFY(topL.glyphs.front().xf.map(QPointF(0, 0)).y()
            < botL.glyphs.front().xf.map(QPointF(0, 0)).y() - 20);
        // Left justify lines up at the frame's left edge.
        p.statics[k::Justify] = justifies::Start;
        const TextPlusLayout leftL = layoutTextPlus(styleOf(p));
        QVERIFY(std::abs(leftL.glyphs.front().xf.map(QPointF(0, 0)).x() - (centreX - 160)) < 1.0);
        // Without Frame the same text is one line.
        p.statics[k::LayoutType] = textlayouts::Point;
        QVERIFY(!styleOf(p).plus.active());
    }

    void kerningMovesTheNextCharacters()
    {
        ParamSet p = textParams();
        p.statics[k::Text] = QStringLiteral("AAAA");
        p.statics[k::LayoutType] = textlayouts::Frame; // any layout uses the per-glyph code
        p.setBase(k::FrameWidth, 5);
        const TextPlusLayout plain = layoutTextPlus(styleOf(p));
        p.statics[k::Kerning] = QStringLiteral("0, 50 0 0");
        const TitleStyle s = styleOf(p);
        QCOMPARE(s.plus.kerning.size(), size_t(4));
        const TextPlusLayout kerned = layoutTextPlus(s);
        QCOMPARE(kerned.glyphs.size(), plain.glyphs.size());
        // 50 % of a 60 px font = 30 px more between glyph 1 and 2; the line is wider by 30 px,
        // so the centred line starts 15 px further left.
        const auto x = [](const TextPlusLayout& l, int i) {
            return l.glyphs[size_t(i)].xf.map(QPointF(0, 0)).x();
        };
        QVERIFY(std::abs((x(kerned, 2) - x(kerned, 1)) - (x(plain, 2) - x(plain, 1)) - 30.0) < 0.5);
        QVERIFY(std::abs((x(kerned, 1) - x(kerned, 0)) - (x(plain, 1) - x(plain, 0))) < 0.5);
        QVERIFY(std::abs((x(kerned, 0) - x(plain, 0)) + 15.0) < 0.5);
    }

    void trackingAndLineSpacingReachThePlainRenderer()
    {
        ParamSet p = textParams();
        p.statics[k::Text] = QStringLiteral("AB\nCD");
        const TitleImage base = renderTitle(styleOf(p));
        p.setBase(k::LineSpacing, 200);
        p.setBase(k::TextTracking, 100);
        const TitleStyle s = styleOf(p);
        QVERIFY(!s.plus.active());
        QCOMPARE(s.lineSpacing, 200.0);
        QCOMPARE(s.tracking, 100.0);
        const TitleImage wide = renderTitle(s);
        QVERIFY(wide.image.width() > base.image.width() + 20);
        QVERIFY(wide.image.height() > base.image.height() + 20);
    }

    void followerDelaysTheAnimationPerCharacter()
    {
        ParamSet p = textParams();
        p.statics[k::Text] = QStringLiteral("abcd");
        p.setBase(k::FollowDelay, 2);
        p.statics[k::FollowDelay] = 2;
        p.animated(k::FollowX, 0).setKey(0, 0);
        p.animated(k::FollowX, 0).setKey(10, 100);
        // Frame 10: character 0 is at its end value, character 1 two frames behind (80), ...
        const TitleStyle s = styleOf(p, 10);
        QVERIFY(s.plus.active());
        QCOMPARE(s.plus.chars.size(), size_t(4));
        QCOMPARE(s.plus.chars[0].dx, 100.0);
        QVERIFY(std::abs(s.plus.chars[1].dx - 80.0) < 1e-9);
        QVERIFY(std::abs(s.plus.chars[2].dx - 60.0) < 1e-9);
        QVERIFY(std::abs(s.plus.chars[3].dx - 40.0) < 1e-9);
        // The glyphs move by those amounts.
        const TextPlusLayout moved = layoutTextPlus(s);
        TitleStyle off = s;
        off.plus.chars.clear();
        off.plus.layout = TextPlus::Layout::Frame;
        off.plus.frameWidth = 4000;
        const TextPlusLayout still = layoutTextPlus(off);
        for (int i = 0; i < 4; ++i) {
            const double dx = moved.glyphs[size_t(i)].xf.map(QPointF(0, 0)).x()
                - still.glyphs[size_t(i)].xf.map(QPointF(0, 0)).x();
            QVERIFY2(std::abs(dx - s.plus.chars[size_t(i)].dx) < 0.01, qPrintable(QString::number(i)));
        }
        // Size and angle: the glyph turns about its middle (the middle stays put).
        ParamSet q = textParams();
        q.statics[k::Text] = QStringLiteral("a");
        q.setBase(k::FollowAngle, 90);
        q.setBase(k::FollowSize, 200);
        const TitleStyle t = styleOf(q);
        QCOMPARE(t.plus.chars.size(), size_t(1));
        QCOMPARE(t.plus.chars[0].size, 2.0);
        QCOMPARE(t.plus.chars[0].angle, -90.0); // counter-clockwise in the node
        const TextPlusLayout one = layoutTextPlus(t);
        QVERIFY(std::abs(std::atan2(one.glyphs[0].xf.m12(), one.glyphs[0].xf.m11()) * 180 / M_PI + 90) < 0.1);
        QVERIFY(std::abs(std::hypot(one.glyphs[0].xf.m11(), one.glyphs[0].xf.m12()) - 2.0) < 1e-6);
        // Opacity: a glyph with opacity 0 is invisible.
        q.setBase(k::FollowAngle, 0);
        q.setBase(k::FollowSize, 100);
        q.setBase(k::FollowOpacity, 0);
        const TitleImage clear = renderTitle(styleOf(q));
        bool ink = false;
        for (int y = 0; y < clear.image.height() && !ink; ++y)
            for (int x = 0; x < clear.image.width(); ++x)
                if (qAlpha(clear.image.pixel(x, y)) > 8)
                    ink = true;
        QVERIFY(!ink);
    }

    void shadingElementsDrawBehindTheMainLook()
    {
        ParamSet p = textParams();
        p.statics[k::Text] = QStringLiteral("I");
        p.statics[k::Color] = QColor(Qt::white);
        const TitleImage plain = renderTitle(styleOf(p));
        p.statics[k::shadeKey(2, "enable")] = true;
        p.statics[k::shadeKey(2, "color")] = QColor(255, 0, 0);
        p.setBase(k::shadeKey(2, "x"), 40);
        p.setBase(k::shadeKey(2, "y"), 0);
        const TitleStyle s = styleOf(p);
        QCOMPARE(s.plus.shades.size(), size_t(1));
        QVERIFY(s.plus.active());
        const TitleImage shaded = renderTitle(s);
        QVERIFY(shaded.image.width() >= plain.image.width() + 30);
        // Red pixels exist 40 px to the right of the white ones; the white letter stays white.
        bool red = false, white = false;
        for (int y = 0; y < shaded.image.height(); ++y) {
            for (int x = 0; x < shaded.image.width(); ++x) {
                const QRgb c = shaded.image.pixel(x, y);
                if (qAlpha(c) > 250 && qRed(c) > 240 && qGreen(c) < 20)
                    red = true;
                if (qAlpha(c) > 250 && qRed(c) > 240 && qGreen(c) > 240)
                    white = true;
            }
        }
        QVERIFY(red && white);
        // Outline type: a ring around the glyph, thickness in px.
        p.statics[k::shadeKey(2, "outline")] = true;
        p.setBase(k::shadeKey(2, "x"), 0);
        p.setBase(k::shadeKey(2, "thickness"), 20);
        const TitleImage ring = renderTitle(styleOf(p));
        QVERIFY(ring.image.width() > plain.image.width() + 15);
        // Different style, different texture-cache key.
        TitleStyle other = s;
        other.plus.shades[0].dx += 1;
        QVERIFY(qHash(other, 0) != qHash(s, 0));
        QVERIFY(!(other == s));
    }

    void writeOnStillWorksInPerGlyphLayout()
    {
        ParamSet p = textParams();
        p.statics[k::LayoutType] = textlayouts::Circle;
        p.setBase(k::WriteOnEnd, 0.5);
        const TextPlusLayout layout = layoutTextPlus(styleOf(p));
        int shown = 0, hidden = 0;
        for (const GlyphPlace& g : layout.glyphs)
            (g.shown ? shown : hidden)++;
        QVERIFY(shown > 0 && hidden > 0);
        QVERIFY(std::abs(shown - hidden) <= 1);
    }

    void angleTurnsTheWholeBlock()
    {
        ParamSet p = textParams();
        p.statics[k::LayoutType] = textlayouts::Frame;
        p.setBase(k::FrameWidth, 5);
        const TextPlusLayout flat = layoutTextPlus(styleOf(p));
        p.setBase(k::Angle, 90); // counter-clockwise
        const TextPlusLayout turned = layoutTextPlus(styleOf(p));
        // The first glyph (left end) of a flat line ends up below the centre when it turns
        // counter-clockwise by a quarter: left end -> bottom.
        const QPointF c(kCanvas.width() / 2.0, kCanvas.height() / 2.0);
        QVERIFY(flat.glyphs.front().xf.map(QPointF(0, 0)).x() < c.x());
        QVERIFY(turned.glyphs.front().xf.map(QPointF(0, 0)).y() > c.y());
    }

    void textNodeRoundTripsThroughTheProject()
    {
        Composition comp = co::defaultComposition();
        const quint64 id = co::addNode(comp, nodetypes::Text, {});
        CompNode* n = comp.node(id);
        QVERIFY(n);
        QVERIFY(n->path.pointCount() >= 2); // the default arc
        n->params.statics[k::LayoutType] = textlayouts::Path;
        n->params.statics[k::Kerning] = QStringLiteral("1 2 3");
        n->params.statics[k::shadeKey(3, "enable")] = true;
        n->params.setBase(k::FollowAngle, 30);
        n->path.shapes[0][1].y = -222;
        const QJsonObject json = serialization::compositionToJson(comp);
        const Composition back = serialization::compositionFromJson(json);
        const CompNode* b = back.node(id);
        QVERIFY(b);
        QCOMPARE(b->params.staticValue(k::LayoutType).toString(), textlayouts::Path);
        QCOMPARE(b->params.staticValue(k::Kerning).toString(), QStringLiteral("1 2 3"));
        QVERIFY(b->params.staticValue(k::shadeKey(3, "enable")).toBool());
        QCOMPARE(b->params.value(k::FollowAngle, 0, 0), 30.0);
        QCOMPARE(b->path.shapes[0][1].y, -222.0);
        // A node of an older project has none of the new keys and still renders as before.
        QJsonObject old = json;
        QVERIFY(!old.isEmpty());
    }

    void registryHasTheControls()
    {
        const NodeDescriptor* d = NodeRegistry::instance().find(nodetypes::Text);
        QVERIFY(d);
        for (const QString& key : {k::LayoutType, k::Justify, k::JustifyV, k::FrameWidth,
                 k::FrameHeight, k::CircleRadius, k::CircleAngle, k::PathOffset, k::TextTracking,
                 k::LineSpacing, k::Kerning, k::FollowDelay, k::FollowX, k::FollowY, k::FollowSize,
                 k::FollowAngle, k::FollowOpacity, k::Angle, k::WriteOnStart, k::WriteOnEnd})
            QVERIFY2(d->param(key), qPrintable(key));
        for (int n = 2; n < 2 + k::kShadeElements; ++n)
            for (const char* f : {"enable", "outline", "color", "opacity", "thickness", "x", "y"})
                QVERIFY(d->param(k::shadeKey(n, f)));
        QCOMPARE(d->controls, ViewerControls::TextPath);
    }
};

QTEST_MAIN(TextPlusTest)
#include "test_textplus.moc"
