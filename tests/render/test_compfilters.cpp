// SPDX-License-Identifier: GPL-3.0-or-later
// Pixel tests of the Compositing page nodes that look at neighbouring pixels (spec sections 113
// onwards): Soft Glow, Light Wrap, Grid Warp, Displace, Vector Distort and the keyer matte
// filters. Every case renders on the GPU and on the CPU and checks both against the expectation
// and against each other.
#include "RenderTestUtil.h"
#include "core/CompOps.h"
#include "core/NodeRegistry.h"
#include "render/CompFilterMath.h"
#include "render/CompWarpMath.h"
#include "render/Compositor.h"

#include <QTest>

#include <memory>

using namespace kk;
using namespace kk::test;
namespace co = kk::compops;
namespace compmath = kk::render::compmath;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

TimelineData compTimeline(const Composition& comp, QColor color = QColor(0, 0, 0))
{
    TimelineData tl = emptyTimeline(kW, kH);
    ClipData c = solidClip(tl, color, 0, 50);
    c.composition = comp;
    addClip(tl, TrackType::Video, 0, c);
    return tl;
}

/// A Background node of @p color feeding @p to's port @p port (through a Crop that keeps the
/// rectangle [x0, x1) x [y0, y1) of the 320 x 180 frame when @p crop is set).
quint64 colorSource(Composition& c, QColor color, bool crop, int x0 = 0, int y0 = 0, int x1 = kW,
    int y1 = kH)
{
    const quint64 bg = co::addNode(c, nodetypes::Background, {});
    c.node(bg)->params.setStatic(nodekeys::Color, color);
    if (!crop)
        return bg;
    const quint64 cr = co::addNode(c, nodetypes::Crop, {});
    c.node(cr)->params.setBase(nodekeys::Left, x0);
    c.node(cr)->params.setBase(nodekeys::Top, y0);
    c.node(cr)->params.setBase(nodekeys::Right, kW - x1);
    c.node(cr)->params.setBase(nodekeys::Bottom, kH - y1);
    co::connect(c, bg, 0, cr, nodeports::Input);
    return cr;
}

} // namespace

class TestCompFilters : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;

    /// The image of node @p id on the GPU and on the CPU.
    QVector<QImage> nodeImages(const TimelineData& tl, quint64 id)
    {
        const auto clipId = tl.videoTracks[0].clips[0].id;
        QVector<QImage> out;
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            const RenderedFramePtr f
                = comp->renderCompositionNode(
                          snap(tl), clipId, id, 0, {}, Quality::Full, OutputImage)
                      .get();
            out.append(f && f->ok ? f->image : QImage());
        }
        return out;
    }

    /// Largest channel difference between the two images, and the count of pixels above @p tol.
    static int differing(const QImage& a, const QImage& b, int tol, int* worst = nullptr)
    {
        int count = 0, big = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                const Px p = px(a, x, y), q = px(b, x, y);
                const int d = std::max({std::abs(p.r - q.r), std::abs(p.g - q.g),
                    std::abs(p.b - q.b), std::abs(p.a - q.a)});
                big = std::max(big, d);
                count += d > tol;
            }
        }
        if (worst)
            *worst = big;
        return count;
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

    void softGlowSpreadsBrightAreas()
    {
        // A white box in the middle of a transparent frame, glowing.
        Composition c = co::defaultComposition();
        const quint64 box = colorSource(c, Qt::white, true, 120, 60, 200, 120);
        const quint64 glow = co::addNode(c, nodetypes::SoftGlow, {});
        co::connect(c, box, 0, glow, nodeports::Input);
        co::connect(c, glow, 0, c.mediaOutId, nodeports::Input);
        c.node(glow)->params.setBase(nodekeys::GlowSize, 24.0);
        c.node(glow)->params.setBase(nodekeys::GlowThreshold, 0.5);
        const TimelineData tl = compTimeline(c);
        const QVector<QImage> imgs = nodeImages(tl, glow);
        for (const QImage& img : imgs) {
            QVERIFY(!img.isNull());
            QCOMPARE(px(img, 160, 90).a, 255); // the box itself stays
            const Px near = px(img, 112, 90); // 8 px outside the box
            QVERIFY2(near.a > 20 && near.r > 20, describe(near));
            const Px far = px(img, 20, 20);
            QVERIFY2(far.a < 4, describe(far));
            QVERIFY(px(img, 104, 90).a < near.a); // falls off with distance
        }
        // GPU and CPU agree.
        int worst = 0;
        const int bad = differing(imgs[0], imgs[1], 6, &worst);
        QVERIFY2(bad < 200, qPrintable(QStringLiteral("%1 pixels differ, worst %2").arg(bad).arg(worst)));
    }

    void softGlowThresholdAndChannels()
    {
        Composition c = co::defaultComposition();
        const quint64 box = colorSource(c, QColor(100, 100, 100), true, 120, 60, 200, 120);
        const quint64 glow = co::addNode(c, nodetypes::SoftGlow, {});
        co::connect(c, box, 0, glow, nodeports::Input);
        co::connect(c, glow, 0, c.mediaOutId, nodeports::Input);
        c.node(glow)->params.setBase(nodekeys::GlowSize, 24.0);
        // Grey 100 (0.39) is below a threshold of 0.5: nothing glows.
        c.node(glow)->params.setBase(nodekeys::GlowThreshold, 0.5);
        for (const QImage& img : nodeImages(compTimeline(c), glow))
            QVERIFY2(px(img, 112, 90).a < 3, describe(px(img, 112, 90)));
        // Under it the glow appears; with Glow Alpha and Red / Green off only blue is added.
        c.node(glow)->params.setBase(nodekeys::GlowThreshold, 0.1);
        c.node(glow)->params.setBase(nodekeys::Gain, 3.0);
        c.node(glow)->params.setStatic(nodekeys::GlowRed, false);
        c.node(glow)->params.setStatic(nodekeys::GlowGreen, false);
        c.node(glow)->params.setStatic(nodekeys::GlowAlpha, false);
        for (const QImage& img : nodeImages(compTimeline(c), glow)) {
            const Px p = px(img, 112, 90);
            QVERIFY2(p.a < 3, describe(p)); // no alpha: the glow cannot show outside the box
            QCOMPARE(px(img, 160, 90).b > 100, true);
        }
    }

    void softGlowFilters()
    {
        // Every filter keeps the glow's reach in the same order of size, and GPU = CPU.
        for (const QString& filter : blurfilters::ids()) {
            Composition c = co::defaultComposition();
            const quint64 box = colorSource(c, Qt::white, true, 120, 60, 200, 120);
            const quint64 glow = co::addNode(c, nodetypes::SoftGlow, {});
            co::connect(c, box, 0, glow, nodeports::Input);
            co::connect(c, glow, 0, c.mediaOutId, nodeports::Input);
            c.node(glow)->params.setStatic(nodekeys::BlurFilter, filter);
            c.node(glow)->params.setBase(nodekeys::GlowSize, 16.0);
            const QVector<QImage> imgs = nodeImages(compTimeline(c), glow);
            QVERIFY2(px(imgs[0], 114, 90).a > 20, qPrintable(filter));
            QVERIFY2(px(imgs[1], 114, 90).a > 20, qPrintable(filter));
            QVERIFY2(px(imgs[0], 40, 90).a < 4, qPrintable(filter));
            int worst = 0;
            const int bad = differing(imgs[0], imgs[1], 10, &worst);
            QVERIFY2(bad < 300, qPrintable(QStringLiteral("%1: %2 pixels differ, worst %3")
                                               .arg(filter).arg(bad).arg(worst)));
        }
        // Unlocked: a wide, flat glow reaches further along x than along y.
        Composition c = co::defaultComposition();
        const quint64 box = colorSource(c, Qt::white, true, 140, 70, 180, 110);
        const quint64 glow = co::addNode(c, nodetypes::SoftGlow, {});
        co::connect(c, box, 0, glow, nodeports::Input);
        co::connect(c, glow, 0, c.mediaOutId, nodeports::Input);
        c.node(glow)->params.setStatic(nodekeys::LockXY, false);
        c.node(glow)->params.setBase(nodekeys::GlowSize, 60.0);
        c.node(glow)->params.setBase(nodekeys::GlowSizeY, 6.0);
        for (const QImage& img : nodeImages(compTimeline(c), glow)) {
            const int side = px(img, 140 - 25, 90).a; // 25 px left of the box
            const int above = px(img, 160, 70 - 25).a; // 25 px above it
            QVERIFY2(side > above + 8, qPrintable(QStringLiteral("%1 %2").arg(side).arg(above)));
        }
    }

    void lightWrapTintsTheEdges()
    {
        // A grey box (foreground) in front of an orange background: the wrap tints its edges.
        Composition c = co::defaultComposition();
        const quint64 fg = colorSource(c, QColor(90, 90, 90), true, 100, 40, 220, 140);
        const quint64 bg = colorSource(c, QColor(255, 140, 0), false);
        const quint64 lw = co::addNode(c, nodetypes::LightWrap, {});
        co::connect(c, fg, 0, lw, nodeports::Input);
        co::connect(c, bg, 0, lw, nodeports::Background);
        co::connect(c, lw, 0, c.mediaOutId, nodeports::Input);
        c.node(lw)->params.setBase(nodekeys::WrapIntensity, 1.0);
        c.node(lw)->params.setBase(nodekeys::WrapDiffuse, 8.0);
        c.node(lw)->params.setBase(nodekeys::WrapFgBlur, 12.0);
        const QVector<QImage> imgs = nodeImages(compTimeline(c), lw);
        for (const QImage& img : imgs) {
            const Px edge = px(img, 104, 90); // just inside the left edge
            const Px mid = px(img, 160, 90);
            QVERIFY2(near(mid, 90, 90, 90, 255, 2), describe(mid)); // the centre is untouched
            QVERIFY2(edge.r > edge.b + 25 && edge.r > 100, describe(edge)); // orange spill
            QCOMPARE(edge.a, 255);
            QVERIFY2(px(img, 40, 90).a < 3, describe(px(img, 40, 90))); // nothing outside
        }
        int worst = 0;
        const int bad = differing(imgs[0], imgs[1], 8, &worst);
        QVERIFY2(bad < 150, qPrintable(QStringLiteral("%1 pixels differ, worst %2").arg(bad).arg(worst)));

        // Enable Glow: the wrap also lights the area just outside the foreground.
        c.node(lw)->params.setStatic(nodekeys::WrapGlow, true);
        for (const QImage& img : nodeImages(compTimeline(c), lw))
            QVERIFY2(px(img, 94, 90).a > 10, describe(px(img, 94, 90)));
        // Intensity 0: the foreground as it was.
        c.node(lw)->params.setBase(nodekeys::WrapIntensity, 0.0);
        c.node(lw)->params.setStatic(nodekeys::WrapGlow, false);
        for (const QImage& img : nodeImages(compTimeline(c), lw))
            QVERIFY2(near(px(img, 104, 90), 90, 90, 90, 255, 2), describe(px(img, 104, 90)));
        // Generate Wrap Only: the wrap alone, alpha only at the edge.
        c.node(lw)->params.setBase(nodekeys::WrapIntensity, 1.0);
        c.node(lw)->params.setStatic(nodekeys::WrapOnly, true);
        for (const QImage& img : nodeImages(compTimeline(c), lw)) {
            QVERIFY2(px(img, 160, 90).a < 3, describe(px(img, 160, 90)));
            QVERIFY2(px(img, 104, 90).a > 20, describe(px(img, 104, 90)));
        }
    }

    void lightWrapNeedsABackground()
    {
        // Without a background the node passes the foreground through.
        Composition c = co::defaultComposition();
        const quint64 fg = colorSource(c, QColor(90, 90, 90), true, 100, 40, 220, 140);
        const quint64 lw = co::addNode(c, nodetypes::LightWrap, {});
        co::connect(c, fg, 0, lw, nodeports::Input);
        co::connect(c, lw, 0, c.mediaOutId, nodeports::Input);
        for (const QImage& img : nodeImages(compTimeline(c), lw))
            QVERIFY2(near(px(img, 104, 90), 90, 90, 90, 255, 2), describe(px(img, 104, 90)));
    }

    /// A white box [120, 200) x [60, 120) as the picture, @p type's node fed by it and by @p map.
    Composition warpSetup(const QString& type, QColor mapColor, quint64* node)
    {
        Composition c = co::defaultComposition();
        const quint64 box = colorSource(c, Qt::white, true, 120, 60, 200, 120);
        *node = co::addNode(c, type, {});
        co::connect(c, box, 0, *node, nodeports::Input);
        if (mapColor.isValid()) {
            const quint64 map = colorSource(c, mapColor, false);
            co::connect(c, map, 0, *node, nodeports::Map);
        }
        co::connect(c, *node, 0, c.mediaOutId, nodeports::Input);
        return c;
    }

    void displaceXY()
    {
        quint64 d = 0;
        Composition c = warpSetup(nodetypes::Displace, QColor(200, 128, 0), &d);
        // Red 200 / 255 = 0.784: 0.284 * 20 px = 5.7 px to the right; green 0.502: nothing.
        const QVector<QImage> imgs = nodeImages(compTimeline(c), d);
        for (const QImage& img : imgs) {
            QVERIFY2(px(img, 122, 90).a < 40, describe(px(img, 122, 90))); // the old left edge is empty
            QVERIFY2(px(img, 128, 90).a > 215, describe(px(img, 128, 90)));
            QVERIFY2(px(img, 203, 90).a > 215, describe(px(img, 203, 90))); // the right edge moved out
            QVERIFY2(px(img, 210, 90).a < 40, describe(px(img, 210, 90)));
        }
        QVERIFY(differing(imgs[0], imgs[1], 8) < 200);
        // A map value equal to Offset moves nothing; Edges Canvas keeps the outside transparent.
        c.node(d)->params.setBase(nodekeys::DisplaceOffset, 0.784);
        for (const QImage& img : nodeImages(compTimeline(c), d)) {
            QVERIFY2(px(img, 122, 90).a > 215, describe(px(img, 122, 90)));
            QVERIFY2(px(img, 118, 90).a < 40, describe(px(img, 118, 90)));
        }
    }

    void displaceRadialAndLight()
    {
        quint64 d = 0;
        Composition c = warpSetup(nodetypes::Displace, QColor(255, 255, 255), &d);
        c.node(d)->params.setStatic(nodekeys::DisplaceType, displacetypes::Radial);
        // White = 1: 0.5 * 20 px = 10 px away from the centre, so the box grows by 10 px.
        for (const QImage& img : nodeImages(compTimeline(c), d)) {
            QVERIFY2(px(img, 113, 90).a > 215, describe(px(img, 113, 90)));
            QVERIFY2(px(img, 106, 90).a < 40, describe(px(img, 106, 90)));
            QVERIFY2(px(img, 206, 90).a > 215, describe(px(img, 206, 90)));
        }
        // Light Power shades by the map's slope: a flat map leaves the colour alone.
        c.node(d)->params.setBase(nodekeys::DisplaceLightPower, 1.0);
        for (const QImage& img : nodeImages(compTimeline(c), d))
            QVERIFY2(near(px(img, 160, 90), 255, 255, 255, 255, 2), describe(px(img, 160, 90)));
    }

    void displaceNeedsAMap()
    {
        quint64 d = 0;
        Composition c = warpSetup(nodetypes::Displace, QColor(), &d);
        for (const QImage& img : nodeImages(compTimeline(c), d))
            QVERIFY2(near(px(img, 160, 90), 255, 255, 255, 255, 1), describe(px(img, 160, 90)));
    }

    void vectorDistortMovesAlongVectors()
    {
        quint64 v = 0;
        Composition c = warpSetup(nodetypes::VectorDistort, QColor(191, 128, 0), &v);
        // Red 0.749: (0.749 - 0.5) * 2 * 32 = 16 px to the right.
        const QVector<QImage> imgs = nodeImages(compTimeline(c), v);
        for (const QImage& img : imgs) {
            QVERIFY2(px(img, 130, 90).a < 40, describe(px(img, 130, 90)));
            QVERIFY2(px(img, 142, 90).a > 215, describe(px(img, 142, 90)));
            QVERIFY2(px(img, 210, 90).a > 215, describe(px(img, 210, 90)));
            QVERIFY2(px(img, 222, 90).a < 40, describe(px(img, 222, 90)));
        }
        QVERIFY(differing(imgs[0], imgs[1], 8) < 200);
        // A negative Multiplier goes the other way; steps keep a constant field's result.
        c.node(v)->params.setBase(nodekeys::VectorMultiplier, -1.0);
        c.node(v)->params.setBase(nodekeys::VectorSteps, 4.0);
        for (const QImage& img : nodeImages(compTimeline(c), v)) {
            QVERIFY2(px(img, 108, 90).a > 215, describe(px(img, 108, 90)));
            QVERIFY2(px(img, 190, 90).a < 40, describe(px(img, 190, 90)));
        }
    }

    void gridWarpBendsThePicture()
    {
        quint64 g = 0;
        Composition c = warpSetup(nodetypes::GridWarp, QColor(), &g);
        c.node(g)->params.setBase(nodekeys::Columns, 3.0);
        c.node(g)->params.setBase(nodekeys::Rows, 3.0);
        // Untouched grid: the picture as it is.
        for (const QImage& img : nodeImages(compTimeline(c), g))
            QVERIFY2(near(px(img, 160, 90), 255, 255, 255, 255, 1), describe(px(img, 160, 90)));
        // The middle point 60 px to the right drags the content next to it along.
        c.node(g)->params.setBase(nodekeys::gridKey(1, 1, 0), 60.0);
        const QVector<QImage> imgs = nodeImages(compTimeline(c), g);
        for (const QImage& img : imgs) {
            QVERIFY2(px(img, 215, 90).a > 200, describe(px(img, 215, 90))); // was empty
            QVERIFY2(px(img, 125, 90).a < 100, describe(px(img, 125, 90))); // left edge moved right
            QVERIFY2(px(img, 20, 20).a < 3, describe(px(img, 20, 20))); // far corners stay put
        }
        int worst = 0;
        const int bad = differing(imgs[0], imgs[1], 10, &worst);
        QVERIFY2(bad < 300, qPrintable(QStringLiteral("%1 pixels differ, worst %2").arg(bad).arg(worst)));
        // The maths on its own: a zero grid is the identity, a displaced point moves its own place.
        compmath::GridWarpParams grid;
        grid.cols = grid.rows = 3;
        grid.d.assign(18, 0.0f);
        QVERIFY(grid.isIdentity());
        grid.d[(1 * 3 + 1) * 2] = 0.2f;
        const double t[2] = {0.7, 0.5}; // where the moved centre point arrives
        double src[2];
        compmath::gridWarpSource(grid, t, src);
        QVERIFY2(std::abs(src[0] - 0.5) < 0.02, qPrintable(QString::number(src[0])));
    }

    void floatMathMatchesDefinitions()
    {
        // The reference maths on its own: a blur keeps the total, a glow adds only above the threshold.
        compmath::FImage img(9, 9);
        img.at(4, 4)[0] = img.at(4, 4)[1] = img.at(4, 4)[2] = img.at(4, 4)[3] = 1.0f;
        for (int kind : {0, 1, 2, 3}) {
            const compmath::FImage b = compmath::blurByKind(img, kind, 4.0, 4.0, 3);
            double sum = 0;
            for (int i = 0; i < 9 * 9; ++i)
                sum += b.d[size_t(i) * 4 + 3];
            QVERIFY2(std::abs(sum - 1.0) < 0.05, qPrintable(QString::number(kind)));
            QVERIFY(b.at(4, 4)[3] < 1.0f);
        }
        compmath::SoftGlowParams g;
        g.threshold = 0.5f;
        const float dark[4] = {0.3f, 0.3f, 0.3f, 1.0f};
        float h[4];
        compmath::softGlowHighlight(g, dark, h);
        QCOMPARE(h[0], 0.0f);
        QCOMPARE(h[3], 0.0f);
        const float bright[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        compmath::softGlowHighlight(g, bright, h);
        QCOMPARE(h[0], 0.5f);
        QCOMPARE(h[3], 1.0f);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestCompFilters>(argc, argv);
}
#include "test_compfilters.moc"
