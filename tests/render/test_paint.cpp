// SPDX-License-Identifier: GPL-3.0-or-later
// Spec 131: the Paint node of the Compositing page. The brush is tested as a pure function
// (render/CompPaint); the node on the GPU and the CPU renderer, the saved strokes and the stroke
// duration go through the real graph.
#include "RenderTestUtil.h"
#include "core/CompOps.h"
#include "core/EditOps.h"
#include "core/NodeRegistry.h"
#include "core/PaintStrokes.h"
#include "core/Serialization.h"
#include "render/CompPaint.h"
#include "render/Compositor.h"
#include "render/TimelineEval.h"

#include <QTest>

#include <cmath>
#include <memory>

using namespace kk;
using namespace kk::test;
namespace co = kk::compops;
namespace rp = kk::render::paint;
namespace k = kk::nodekeys;
using kk::paint::Stroke;
using kk::paint::Tool;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

QImage solid(QColor c, QSize size = QSize(kW, kH))
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(c);
    return img;
}

/// A stroke of @p tool through the pixel points @p pts (x, y down) of a kW x kH picture.
Stroke strokeOf(Tool tool, std::initializer_list<QPointF> pts, double size, double softness = 0.0,
    QColor color = QColor(255, 0, 0), double opacity = 1.0)
{
    Stroke s;
    s.tool = tool;
    s.size = size;
    s.softness = softness;
    s.opacity = opacity;
    s.color = color;
    s.first = 0;
    s.last = 0;
    for (const QPointF& p : pts)
        s.points.append({float(p.x() / kW), float(1.0 - p.y() / kH), 1.0f});
    return s;
}

QImage draw(const QVector<Stroke>& strokes, const QImage& in, int64_t frame = 0)
{
    return rp::drawStrokes(strokes, frame, in.size(), in);
}

int worstDifference(const QImage& a, const QImage& b)
{
    int worst = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const Px p = px(a, x, y), q = px(b, x, y);
            worst = std::max({worst, std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b),
                std::abs(p.a - q.a)});
        }
    return worst;
}

TimelineData compTimeline(const Composition& comp)
{
    TimelineData tl = emptyTimeline(kW, kH);
    ClipData c = solidClip(tl, QColor(0, 0, 0), 0, 50);
    c.composition = comp;
    addClip(tl, TrackType::Video, 0, c);
    return tl;
}

/// Background (a solid colour) -> Paint -> MediaOut.
quint64 addPaint(Composition& c, QColor background, const QVector<Stroke>& strokes)
{
    const quint64 bg = co::addNode(c, nodetypes::Background, {});
    c.node(bg)->params.setStatic(k::Color, background);
    const quint64 id = co::addNode(c, nodetypes::Paint, {});
    co::connect(c, bg, 0, id, nodeports::Input);
    co::connect(c, id, 0, c.mediaOutId, nodeports::Input);
    kk::paint::setStrokes(c.node(id)->params, strokes);
    return id;
}

} // namespace

class TestPaint : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;

    QVector<Compositor*> backends() { return {m_gpu.get(), m_cpu.get()}; }

    QImage nodeImage(Compositor* c, const TimelineData& tl, quint64 id, int64_t frame)
    {
        const RenderedFramePtr f = c->renderCompositionNode(snap(tl), tl.videoTracks[0].clips[0].id,
                                        id, frame, {}, Quality::Full, OutputImage, nullptr, nullptr, 0)
                                       .get();
        return f && f->ok ? f->image : QImage();
    }

private slots:
    void initTestCase()
    {
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
    }
    void cleanupTestCase()
    {
        m_gpu.reset();
        m_cpu.reset();
    }

    // ---- The brush ----

    void brushWeightFallsOffWithSoftness()
    {
        QCOMPARE(rp::brushWeight(0, 20, 0.5), 1.0f);
        QCOMPARE(rp::brushWeight(25, 20, 0.5), 0.0f);
        // A hard brush has an edge about one pixel wide; a soft one fades over the radius.
        QVERIFY(rp::brushWeight(18, 20, 0.0) > 0.99f);
        QVERIFY(rp::brushWeight(20.5f, 20, 0.0) < 0.01f);
        QVERIFY(rp::brushWeight(10, 20, 1.0) > 0.45f && rp::brushWeight(10, 20, 1.0) < 0.55f);
        QVERIFY(rp::brushWeight(5, 20, 1.0) > rp::brushWeight(10, 20, 1.0));
        QVERIFY(rp::brushWeight(10, 20, 1.0) > rp::brushWeight(15, 20, 1.0));
    }

    void strokePaintsTheColourWithABrushEdge()
    {
        const QImage in = solid(Qt::blue);
        // size 0.1 = 32 px across, radius 16; a hard brush.
        const QImage hard = draw({strokeOf(Tool::Stroke, {{160, 90}}, 0.1)}, in);
        QVERIFY2(near(px(hard, 160, 90), 255, 0, 0, 255, 1), describe(px(hard, 160, 90)));
        QVERIFY(near(px(hard, 160 + 13, 90), 255, 0, 0, 255, 1));
        QVERIFY(near(px(hard, 160 + 19, 90), 0, 0, 255, 255, 1));
        QVERIFY(near(px(hard, 10, 10), 0, 0, 255, 255, 0));
        // A soft brush is only half strong half way out.
        const QImage soft = draw({strokeOf(Tool::Stroke, {{160, 90}}, 0.1, 1.0)}, in);
        QVERIFY(near(px(soft, 160, 90), 255, 0, 0, 255, 1));
        const Px half = px(soft, 160 + 8, 90);
        QVERIFY2(half.r > 100 && half.r < 160, describe(half));
        QVERIFY(px(soft, 160 + 14, 90).r < px(soft, 160 + 8, 90).r);
    }

    void aStrokeIsOneEvenLayerAtItsOpacity()
    {
        const QImage in = solid(Qt::black);
        // Going back and forth over the same line does not build the colour up.
        const QImage out = draw(
            {strokeOf(Tool::Stroke, {{100, 90}, {200, 90}, {100, 90}, {200, 90}}, 0.08, 0.0,
                QColor(255, 255, 255), 0.5)},
            in);
        const Px mid = px(out, 150, 90);
        QVERIFY2(std::abs(mid.r - 128) <= 2, describe(mid));
        // A stroke along a line is as strong at its ends as in the middle.
        QVERIFY(std::abs(px(out, 110, 90).r - mid.r) <= 2);
    }

    void pressureScalesTheBrush()
    {
        Stroke s = strokeOf(Tool::Stroke, {{60, 90}, {260, 90}}, 0.1);
        s.points.first().pressure = 0.25f;
        const QImage out = draw({s}, solid(Qt::blue));
        QVERIFY(px(out, 60, 90).r > 250);
        QVERIFY(px(out, 60, 90 - 8).r < 5); // the thin start: radius 4
        QVERIFY(px(out, 260, 90 - 12).r > 250); // the full end: radius 16
    }

    void cloneCopiesTheOffsetRegion()
    {
        QImage in = solid(Qt::black);
        for (int y = 80; y < 90; ++y)
            for (int x = 55; x < 65; ++x)
                in.setPixelColor(x, y, QColor(255, 255, 255));
        // The brush is at x 110; it copies from 50 px to the left: the white square.
        Stroke s = strokeOf(Tool::Clone, {{110, 85}}, 0.06);
        s.cloneX = -50.0 / kW;
        s.cloneY = 0.0;
        const QImage out = draw({s}, in);
        QVERIFY2(near(px(out, 110, 85), 255, 255, 255, 255, 1), describe(px(out, 110, 85)));
        QVERIFY(near(px(out, 107, 82), 255, 255, 255, 255, 1)); // the square's pixels move 50 px right
        QVERIFY(near(px(out, 117, 85), 0, 0, 0, 255, 1)); // past the 10 px square
        QVERIFY(near(px(out, 200, 85), 0, 0, 0, 255, 0));
        QVERIFY(near(px(out, 60, 85), 255, 255, 255, 255, 0)); // the source stays
        // A vertical offset: positive is up.
        Stroke up = strokeOf(Tool::Clone, {{60, 100}}, 0.06);
        up.cloneX = 0.0;
        up.cloneY = 15.0 / kH; // 15 px above the brush: y 85
        QVERIFY(near(px(draw({up}, in), 60, 100), 255, 255, 255, 255, 1));
    }

    void eraserBringsTheInputBack()
    {
        const QImage in = solid(QColor(10, 120, 200));
        const QImage painted = draw({strokeOf(Tool::Stroke, {{100, 90}, {220, 90}}, 0.08)}, in);
        QVERIFY(worstDifference(in, painted) > 100);
        const QImage erased = draw({strokeOf(Tool::Stroke, {{100, 90}, {220, 90}}, 0.08),
                                       strokeOf(Tool::Eraser, {{90, 90}, {230, 90}}, 0.2)},
            in);
        QCOMPARE(worstDifference(in, erased), 0);
        // A half-strength eraser brings half of it back.
        const QImage half = draw({strokeOf(Tool::Stroke, {{160, 90}}, 0.08, 0.0, Qt::white),
                                     strokeOf(Tool::Eraser, {{160, 90}}, 0.2, 0.0, Qt::black, 0.5)},
            solid(Qt::black));
        QVERIFY(std::abs(px(half, 160, 90).r - 128) <= 2);
    }

    void smearDragsPixelsAlongTheStroke()
    {
        QImage in = solid(Qt::black);
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < 160; ++x)
                in.setPixelColor(x, y, QColor(255, 0, 0));
        const QImage out = draw({strokeOf(Tool::Smear, {{120, 90}, {200, 90}}, 0.1, 0.2)}, in);
        // Red is pulled into the black half along the stroke ...
        QVERIFY2(px(out, 175, 90).r > 120, describe(px(out, 175, 90)));
        // ... and not sideways, nor behind the start of the stroke, nor in front of its end.
        QVERIFY(px(out, 175, 30).r < 5);
        QVERIFY(near(px(out, 100, 90), 255, 0, 0, 255, 2));
        QVERIFY(px(out, 230, 90).r < 5);
        // Smearing the other way pulls black into the red.
        const QImage back = draw({strokeOf(Tool::Smear, {{200, 90}, {120, 90}}, 0.1, 0.2)}, in);
        QVERIFY(px(back, 150, 90).r < 100);
    }

    void aStrokeIsResolutionIndependent()
    {
        const Stroke s = strokeOf(Tool::Stroke, {{100, 60}, {220, 120}}, 0.07, 0.4);
        const QImage small = rp::drawStrokes({s}, 0, QSize(kW, kH), solid(Qt::blue));
        const QImage big = rp::drawStrokes({s}, 0, QSize(kW * 2, kH * 2), solid(Qt::blue, QSize(kW * 2, kH * 2)));
        const QImage scaled = big.scaled(kW, kH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        // The same picture within edge rounding: the mean difference is tiny.
        double sum = 0;
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
                sum += std::abs(px(small, x, y).r - px(scaled, x, y).r);
        QVERIFY2(sum / (kW * kH) < 1.0, qPrintable(QString::number(sum / (kW * kH))));
        QVERIFY(px(big, 320, 180).r > 100); // the middle of the stroke
    }

    // ---- Stroke duration ----

    void aStrokeIsOnlySeenInItsFrameRange()
    {
        Stroke s = strokeOf(Tool::Stroke, {{160, 90}}, 0.1);
        s.first = 5;
        s.last = 7;
        const QImage in = solid(Qt::blue);
        QVERIFY(rp::drawStrokes({s}, 4, in.size(), in).isNull());
        for (int f : {5, 6, 7})
            QVERIFY(near(px(draw({s}, in, f), 160, 90), 255, 0, 0, 255, 1));
        QVERIFY(rp::drawStrokes({s}, 8, in.size(), in).isNull());
    }

    void strokeDurationControlSetsTheRangeOfNewStrokes()
    {
        const NodeDescriptor* d = NodeRegistry::instance().find(nodetypes::Paint);
        QVERIFY(d);
        ParamSet p = NodeRegistry::instance().defaultParams(nodetypes::Paint);
        Stroke one = kk::paint::newStroke(p, 12);
        QCOMPARE(one.first, int64_t(12));
        QCOMPARE(one.last, int64_t(12));
        p.setStatic(k::StrokeDuration, paintdurations::Whole);
        const Stroke whole = kk::paint::newStroke(p, 12);
        QVERIFY(whole.visibleAt(0) && whole.visibleAt(12) && whole.visibleAt(100000));
        p.setStatic(k::StrokeDuration, paintdurations::Frames);
        p.setBase(k::StrokeFrames, 4);
        const Stroke some = kk::paint::newStroke(p, 12);
        QVERIFY(!some.visibleAt(11) && some.visibleAt(12) && some.visibleAt(15) && !some.visibleAt(16));
    }

    void newStrokeTakesTheBrushOfTheNode()
    {
        ParamSet p = NodeRegistry::instance().defaultParams(nodetypes::Paint);
        p.setStatic(k::PaintTool, painttools::Smear);
        p.setBase(k::BrushSize, 0.2);
        p.setBase(k::BrushSoftness, 0.7);
        p.setBase(k::BrushOpacity, 0.4);
        p.setStatic(k::Color, QColor(1, 2, 3));
        p.setBase(k::CloneOffsetX, -0.25);
        const Stroke s = kk::paint::newStroke(p, 3);
        QCOMPARE(int(s.tool), int(Tool::Smear));
        QCOMPARE(s.size, 0.2);
        QCOMPARE(s.softness, 0.7);
        QCOMPARE(s.opacity, 0.4);
        QCOMPARE(s.color, QColor(1, 2, 3));
        QCOMPARE(s.cloneX, -0.25);
    }

    // ---- The node on both renderers ----

    void paintNodeDrawsTheSameOnBothRenderers()
    {
        Stroke a = strokeOf(Tool::Stroke, {{80, 60}, {160, 120}, {240, 70}}, 0.08, 0.5);
        a.first = 0;
        a.last = 10;
        Stroke b = strokeOf(Tool::Smear, {{60, 100}, {140, 100}}, 0.1, 0.3);
        b.first = 0;
        b.last = 10;
        Composition c = co::defaultComposition();
        const quint64 id = addPaint(c, QColor(20, 90, 200), {a, b});
        const TimelineData tl = compTimeline(c);
        QImage first;
        for (Compositor* backend : backends()) {
            const QString who = backend == m_gpu.get() ? "GPU" : "CPU";
            const QImage img = nodeImage(backend, tl, id, 3);
            QVERIFY2(!img.isNull(), qPrintable(who));
            QVERIFY2(near(px(img, 160, 120), 255, 0, 0, 255, 6), qPrintable(who + describe(px(img, 160, 120))));
            QVERIFY2(near(px(img, 5, 5), 20, 90, 200, 255, 2), qPrintable(who + describe(px(img, 5, 5))));
            // A frame outside the range is the input unchanged.
            const QImage plain = nodeImage(backend, tl, id, 20);
            QVERIFY2(near(px(plain, 160, 120), 20, 90, 200, 255, 2), qPrintable(who));
            if (first.isNull()) {
                first = img;
            } else {
                const int worst = worstDifference(first, img);
                QVERIFY2(worst <= 2, qPrintable(QString::number(worst)));
            }
        }
    }

    void paintOnNothingPaintsOnTransparent()
    {
        const Stroke s = strokeOf(Tool::Stroke, {{160, 90}}, 0.1);
        const QImage out = rp::drawStrokes({s}, 0, QSize(kW, kH), QImage());
        QVERIFY(near(px(out, 160, 90), 255, 0, 0, 255, 1));
        QCOMPARE(px(out, 10, 10).a, 0);
    }

    // ---- Project ----

    void strokesSurviveSaveAndLoad()
    {
        Stroke a = strokeOf(Tool::Clone, {{80, 60}, {160, 120}}, 0.08, 0.5, QColor(10, 20, 30, 200), 0.6);
        a.points[1].pressure = 0.4f;
        a.cloneX = -0.2;
        a.cloneY = 0.05;
        a.first = 3;
        a.last = 9;
        Stroke b = strokeOf(Tool::Eraser, {{10, 10}}, 0.02);
        b.first = -kk::paint::kForever;
        b.last = kk::paint::kForever;
        Composition c = co::defaultComposition();
        const quint64 id = addPaint(c, Qt::blue, {a, b});
        c.node(id)->params.setStatic(k::PaintTool, painttools::Smear);
        const Composition back = serialization::compositionFromJson(serialization::compositionToJson(c));
        const CompNode* n = back.node(id);
        QVERIFY(n);
        const QVector<Stroke> strokes = kk::paint::strokesOf(n->params);
        QCOMPARE(strokes.size(), 2);
        QCOMPARE(strokes[0].points.size(), 2);
        QCOMPARE(strokes[0].points[1].pressure, 0.4f);
        QCOMPARE(strokes[0].color, QColor(10, 20, 30, 200));
        QCOMPARE(strokes[0].first, int64_t(3));
        QCOMPARE(strokes[0].cloneX, -0.2);
        QCOMPARE(int(strokes[1].tool), int(Tool::Eraser));
        QVERIFY(strokes[1].visibleAt(-5000));
        QCOMPARE(n->params.staticValue(k::PaintTool).toString(), painttools::Smear);
        // The same strokes draw the same picture after loading.
        const QImage in = solid(Qt::blue);
        QCOMPARE(worstDifference(draw({a, b}, in, 5), draw(strokes, in, 5)), 0);
        QVERIFY(kk::paint::fromText(QStringLiteral("not json")).isEmpty());
    }

    void everyControlHasATooltip()
    {
        const NodeDescriptor* d = NodeRegistry::instance().find(nodetypes::Paint);
        QVERIFY(d);
        QVERIFY(!d->description.isEmpty());
        QVERIFY(d->category == NodeCategory::Paint);
        for (const ParamDescriptor& p : d->params) {
            if (p.hidden)
                continue;
            QVERIFY2(!p.tooltip.isEmpty(), qPrintable(p.key));
        }
        QVERIFY(NodeRegistry::instance().hasCommonControls(nodetypes::Paint));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestPaint>(argc, argv);
}
#include "test_paint.moc"
