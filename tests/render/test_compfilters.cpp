// SPDX-License-Identifier: GPL-3.0-or-later
// Pixel tests of the Compositing page nodes that look at neighbouring pixels (spec sections 113
// onwards): Soft Glow, Light Wrap, Grid Warp, Displace, Vector Distort and the keyer matte
// filters. Every case renders on the GPU and on the CPU and checks both against the expectation
// and against each other.
#include "RenderTestUtil.h"
#include "core/CompLinks.h"
#include "core/CompOps.h"
#include "core/Modifiers.h"
#include "core/NodeRegistry.h"
#include "render/CompFilterMath.h"
#include "render/CompMatteMath.h"
#include "render/CompMath.h"
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
    QVector<QImage> nodeImages(const TimelineData& tl, quint64 id, int64_t frame = 0)
    {
        const auto clipId = tl.videoTracks[0].clips[0].id;
        QVector<QImage> out;
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            const RenderedFramePtr f
                = comp->renderCompositionNode(
                          snap(tl), clipId, id, frame, {}, Quality::Full, OutputImage)
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

    static double alphaSum(const QImage& a)
    {
        double sum = 0;
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x)
                sum += px(a, x, y).a;
        return sum / 255.0;
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

    /// A Merge node of @p fg over @p bg, wired port by port (mergeOnto() would also move the
    /// other consumers of @p bg to the Merge).
    static quint64 plainMerge(Composition& c, quint64 fg, quint64 bg)
    {
        const quint64 m = co::addNode(c, nodetypes::Merge, {});
        co::connect(c, bg, 0, m, nodeports::Background);
        co::connect(c, fg, 0, m, nodeports::Foreground);
        return m;
    }

    void lightWrapInKeyedComposite()
    {
        // The 1.10.0 report: Delta Keyer -> Light Wrap -> Merge came out black. The graph that
        // works as documented: a green-screen shot is keyed, the keyed subject goes into Light
        // Wrap's Input, the new background plate goes to Light Wrap's Background AND to the
        // Merge's Background, and the Light Wrap output is the Merge's Foreground. Sharing the
        // plate (or the shot) between several nodes must not matter.
        for (int variant = 0; variant < 2; ++variant) {
            Composition c = co::defaultComposition();
            const quint64 green = colorSource(c, QColor(0, 200, 0), false);
            const quint64 subject = colorSource(c, QColor(90, 90, 90), true, 100, 40, 220, 140);
            const quint64 shot = co::mergeOnto(c, subject, green);
            const quint64 key = co::addNode(c, nodetypes::DeltaKeyer, {});
            co::connect(c, shot, 0, key, nodeports::Input);
            // 0: the plate feeds Light Wrap's Background and the Merge; 1: Light Wrap has no
            // Background (it passes the keyed subject through, never black).
            const quint64 plate = colorSource(c, QColor(255, 140, 0), false);
            const quint64 lw = co::addNode(c, nodetypes::LightWrap, {});
            co::connect(c, key, 0, lw, nodeports::Input);
            if (variant == 0)
                co::connect(c, plate, 0, lw, nodeports::Background);
            const quint64 m = plainMerge(c, lw, plate);
            co::connect(c, m, 0, c.mediaOutId, nodeports::Input);
            c.node(lw)->params.setBase(nodekeys::WrapIntensity, 1.0);
            const TimelineData tl = compTimeline(c);
            for (const quint64 id : {lw, m}) {
                const QVector<QImage> imgs = nodeImages(tl, id);
                for (const QImage& img : imgs) {
                    const Px mid = px(img, 160, 90);
                    QVERIFY2(near(mid, 90, 90, 90, 255, 4), describe(mid)); // the subject, not black
                    const Px edge = px(img, 104, 90);
                    if (variant == 0)
                        QVERIFY2(edge.r > edge.b + 20 && edge.a == 255, describe(edge)); // wrapped
                    if (id == m) {
                        const Px out = px(img, 40, 90);
                        QVERIFY2(near(out, 255, 140, 0, 255, 3), describe(out)); // the plate
                    }
                }
            }
        }
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

    /// A green screen with a red box [120, 200) x [60, 120) on it, and @p type's node after it.
    Composition keySetup(const QString& type, quint64* node, quint64* screen = nullptr)
    {
        Composition c = co::defaultComposition();
        const quint64 green = colorSource(c, QColor(0, 200, 0), false);
        const quint64 box = colorSource(c, QColor(255, 0, 0), true, 120, 60, 200, 120);
        const quint64 m = co::mergeOnto(c, box, green);
        *node = co::addNode(c, type, {});
        co::connect(c, m, 0, *node, nodeports::Input);
        co::connect(c, *node, 0, c.mediaOutId, nodeports::Input);
        if (screen)
            *screen = green;
        return c;
    }

    void deltaKeyerMatteFilters()
    {
        quint64 k = 0;
        Composition c = keySetup(nodetypes::DeltaKeyer, &k);
        auto& p = c.node(k)->params;
        const QVector<QImage> plain = nodeImages(compTimeline(c), k);
        for (const QImage& img : plain) {
            QCOMPARE(px(img, 160, 90).a, 255);
            QVERIFY2(px(img, 40, 40).a < 3, describe(px(img, 40, 40)));
            QCOMPARE(px(img, 121, 90).a, 255); // a hard edge
        }
        // Matte Blur softens the edge: partly transparent at the box edge, GPU = CPU.
        p.setBase(nodekeys::MatteBlur, 10.0);
        const QVector<QImage> blurred = nodeImages(compTimeline(c), k);
        for (const QImage& img : blurred) {
            const int edge = px(img, 120, 90).a;
            QVERIFY2(edge > 40 && edge < 215, qPrintable(QString::number(edge)));
            QCOMPARE(px(img, 160, 90).a, 255);
        }
        int worst = 0;
        int bad = differing(blurred[0], blurred[1], 10, &worst);
        QVERIFY2(bad < 300, qPrintable(QStringLiteral("blur: %1 pixels differ, worst %2").arg(bad).arg(worst)));
        // Erode / Dilate: -6 px eats into the box, +6 px grows it.
        p.setBase(nodekeys::MatteBlur, 0.0);
        p.setBase(nodekeys::MatteErode, -6.0);
        for (const QImage& img : nodeImages(compTimeline(c), k)) {
            QVERIFY2(px(img, 124, 90).a < 40, describe(px(img, 124, 90)));
            QVERIFY2(px(img, 130, 90).a > 215, describe(px(img, 130, 90)));
        }
        p.setBase(nodekeys::MatteErode, 6.0);
        for (const QImage& img : nodeImages(compTimeline(c), k)) {
            QVERIFY2(px(img, 116, 90).a > 215, describe(px(img, 116, 90)));
            QVERIFY2(px(img, 110, 90).a < 40, describe(px(img, 110, 90)));
        }
        // Pre-Matte Blur / Erode work before the matte filters and agree on both renderers.
        p.setBase(nodekeys::MatteErode, 0.0);
        p.setBase(nodekeys::PreMatteBlur, 6.0);
        p.setBase(nodekeys::PreMatteErode, 3.0);
        const QVector<QImage> pre = nodeImages(compTimeline(c), k);
        bad = differing(pre[0], pre[1], 10, &worst);
        QVERIFY2(bad < 300, qPrintable(QStringLiteral("pre: %1 pixels differ, worst %2").arg(bad).arg(worst)));
        QVERIFY(px(pre[0], 118, 90).a > 20);
        // View Mode Matte: the matte as grey, opaque.
        p.setBase(nodekeys::PreMatteBlur, 0.0);
        p.setBase(nodekeys::PreMatteErode, 0.0);
        p.setStatic(nodekeys::ViewMode, viewmodes::Matte);
        for (const QImage& img : nodeImages(compTimeline(c), k)) {
            QVERIFY2(near(px(img, 160, 90), 255, 255, 255, 255, 1), describe(px(img, 160, 90)));
            QVERIFY2(near(px(img, 40, 40), 0, 0, 0, 255, 1), describe(px(img, 40, 40)));
        }
    }

    void deltaKeyerMattesAndCleanPlate()
    {
        quint64 k = 0, screen = 0;
        Composition c = keySetup(nodetypes::DeltaKeyer, &k, &screen);
        // A garbage matte over the middle of the box takes it out, a solid matte over the screen
        // keeps that part.
        const quint64 garbage = colorSource(c, Qt::white, true, 140, 70, 180, 110);
        const quint64 solid = colorSource(c, Qt::white, true, 20, 20, 60, 60);
        co::connect(c, garbage, 0, k, nodeports::GarbageMatte);
        co::connect(c, solid, 0, k, nodeports::SolidMatte);
        for (const QImage& img : nodeImages(compTimeline(c), k)) {
            QVERIFY2(px(img, 160, 90).a < 3, describe(px(img, 160, 90)));
            QVERIFY2(px(img, 128, 90).a > 250, describe(px(img, 128, 90)));
            QVERIFY2(px(img, 40, 40).a > 250, describe(px(img, 40, 40)));
            QVERIFY2(px(img, 100, 140).a < 3, describe(px(img, 100, 140)));
        }
        // Inverting the garbage matte keeps only what it covers.
        c.node(k)->params.setStatic(nodekeys::GarbageInvert, true);
        for (const QImage& img : nodeImages(compTimeline(c), k)) {
            QVERIFY2(px(img, 160, 90).a > 250, describe(px(img, 160, 90)));
            QVERIFY2(px(img, 128, 90).a < 3, describe(px(img, 128, 90)));
        }
    }

    void deltaKeyerCleanPlate()
    {
        // A darker patch of the screen (a shadow) keys out only with a clean plate that has it.
        Composition c = co::defaultComposition();
        const quint64 green = colorSource(c, QColor(0, 200, 0), false);
        const quint64 shade = colorSource(c, QColor(0, 100, 0), true, 20, 20, 100, 100);
        const quint64 plateShade = colorSource(c, QColor(0, 100, 0), true, 20, 20, 100, 100);
        const quint64 plateGreen = colorSource(c, QColor(0, 200, 0), false);
        const quint64 shot = co::mergeOnto(c, shade, green);
        const quint64 plate = co::mergeOnto(c, plateShade, plateGreen);
        const quint64 k = co::addNode(c, nodetypes::DeltaKeyer, {});
        co::connect(c, shot, 0, k, nodeports::Input);
        co::connect(c, k, 0, c.mediaOutId, nodeports::Input);
        for (const QImage& img : nodeImages(compTimeline(c), k)) {
            QVERIFY2(px(img, 60, 60).a > 60, describe(px(img, 60, 60))); // the shadow survives the flat key
            QVERIFY2(px(img, 200, 140).a < 3, describe(px(img, 200, 140)));
        }
        co::connect(c, plate, 0, k, nodeports::CleanPlate);
        for (const QImage& img : nodeImages(compTimeline(c), k))
            QVERIFY2(px(img, 60, 60).a < 3, describe(px(img, 60, 60)));
    }

    void deltaKeyerFringeAndReplace()
    {
        // A half-green pixel (the key leaves it half transparent): the replace modes and the
        // fringe colour act on it, GPU = CPU.
        quint64 k = 0;
        Composition c = co::defaultComposition();
        const quint64 src = colorSource(c, QColor(0, 100, 0), false);
        k = co::addNode(c, nodetypes::DeltaKeyer, {});
        co::connect(c, src, 0, k, nodeports::Input);
        co::connect(c, k, 0, c.mediaOutId, nodeports::Input);
        auto& p = c.node(k)->params;
        p.setBase(nodekeys::Despill, 0.0);
        const QVector<QImage> base = nodeImages(compTimeline(c), k);
        const int a = px(base[0], 50, 50).a;
        QVERIFY2(a > 100 && a < 160, qPrintable(QString::number(a))); // about half
        p.setStatic(nodekeys::ReplaceMode, replacemodes::Soft);
        p.setStatic(nodekeys::ReplaceColor, QColor(0, 0, 255));
        for (const QImage& img : nodeImages(compTimeline(c), k)) {
            const Px q = px(img, 50, 50);
            QVERIFY2(q.b > q.g + 20, describe(q)); // the blue replaces the green
            QVERIFY2(std::abs(q.a - a) <= 2, describe(q));
        }
        p.setStatic(nodekeys::ReplaceMode, replacemodes::None);
        p.setBase(nodekeys::FringeMagentaGreen, -1.0); // towards magenta: less green
        p.setBase(nodekeys::FringeCyanRed, 1.0);
        const QVector<QImage> fr = nodeImages(compTimeline(c), k);
        for (const QImage& img : fr)
            QVERIFY2(px(img, 50, 50).r > px(base[0], 50, 50).r + 8, describe(px(img, 50, 50)));
        QVERIFY(differing(fr[0], fr[1], 6) < 50);
        // The spill method None leaves the colour; Burnt pulls it down further than Rare.
        p.setBase(nodekeys::FringeMagentaGreen, 0.0);
        p.setBase(nodekeys::FringeCyanRed, 0.0);
        p.setBase(nodekeys::Despill, 1.0);
        p.setStatic(nodekeys::SpillMethod, spillmethods::None);
        const int greenNone = px(nodeImages(compTimeline(c), k)[0], 50, 50).g;
        p.setStatic(nodekeys::SpillMethod, spillmethods::Rare);
        const int greenRare = px(nodeImages(compTimeline(c), k)[0], 50, 50).g;
        QVERIFY2(greenNone > 20 && greenRare <= greenNone, qPrintable(QStringLiteral("%1 %2").arg(greenNone).arg(greenRare)));
    }

    void ultraKeyer()
    {
        quint64 k = 0;
        Composition c = keySetup(nodetypes::UltraKeyer, &k);
        const QVector<QImage> imgs = nodeImages(compTimeline(c), k);
        for (const QImage& img : imgs) {
            QVERIFY2(px(img, 40, 40).a < 3, describe(px(img, 40, 40)));
            QVERIFY2(near(px(img, 160, 90), 255, 0, 0, 255, 2), describe(px(img, 160, 90)));
        }
        QVERIFY(differing(imgs[0], imgs[1], 4) < 50);
        // Matte tab: blur softens, Contract/Expand moves the edge.
        auto& p = c.node(k)->params;
        p.setBase(nodekeys::MatteErode, -6.0);
        for (const QImage& img : nodeImages(compTimeline(c), k))
            QVERIFY2(px(img, 124, 90).a < 40, describe(px(img, 124, 90)));
        p.setBase(nodekeys::MatteErode, 0.0);
        p.setBase(nodekeys::MatteBlur, 10.0);
        const QVector<QImage> blurred = nodeImages(compTimeline(c), k);
        int worst = 0;
        const int bad = differing(blurred[0], blurred[1], 10, &worst);
        QVERIFY2(bad < 300, qPrintable(QStringLiteral("%1 pixels differ, worst %2").arg(bad).arg(worst)));
        // The levels: a very high Red Level makes the red box look like screen (it keys out).
        p.setBase(nodekeys::MatteBlur, 0.0);
        p.setBase(nodekeys::UkRedLevel, 2.0);
        // Garbage matte over the screen part keeps nothing there; a blue screen key works as well.
        p.setBase(nodekeys::UkRedLevel, 1.0);
        p.setStatic(nodekeys::Color, QColor(0, 0, 255));
        Composition blue = co::defaultComposition();
        const quint64 bs = colorSource(blue, QColor(0, 0, 255), false);
        const quint64 uk = co::addNode(blue, nodetypes::UltraKeyer, {});
        blue.node(uk)->params.setStatic(nodekeys::Color, QColor(0, 0, 255));
        co::connect(blue, bs, 0, uk, nodeports::Input);
        co::connect(blue, uk, 0, blue.mediaOutId, nodeports::Input);
        for (const QImage& img : nodeImages(compTimeline(blue), uk))
            QVERIFY2(px(img, 100, 100).a < 3, describe(px(img, 100, 100)));
        // Matte Separation steepens the transition of a half-keyed colour.
        Composition half = co::defaultComposition();
        const quint64 hs = colorSource(half, QColor(0, 150, 60), false);
        const quint64 hk = co::addNode(half, nodetypes::UltraKeyer, {});
        co::connect(half, hs, 0, hk, nodeports::Input);
        co::connect(half, hk, 0, half.mediaOutId, nodeports::Input);
        half.node(hk)->params.setStatic(nodekeys::SubtractBackground, false);
        const int soft = px(nodeImages(compTimeline(half), hk)[0], 50, 50).a;
        half.node(hk)->params.setBase(nodekeys::MatteSeparation, 1.0);
        const QVector<QImage> sep = nodeImages(compTimeline(half), hk);
        QVERIFY2(soft > 20 && soft < 235, qPrintable(QString::number(soft)));
        QVERIFY(std::abs(px(sep[0], 50, 50).a - px(sep[1], 50, 50).a) <= 3);
        QVERIFY(std::abs(px(sep[0], 50, 50).a - 128) > std::abs(soft - 128) - 2);
    }

    void matteControlAndRestoreFringe()
    {
        // A white box with a blurred edge made by one Matte Control, then thresholded by another.
        Composition c = co::defaultComposition();
        const quint64 box = colorSource(c, Qt::white, true, 120, 60, 200, 120);
        const quint64 soft = co::addNode(c, nodetypes::MatteControl, {});
        c.node(soft)->params.setBase(nodekeys::MatteBlur, 12.0);
        co::connect(c, box, 0, soft, nodeports::Input);
        const quint64 hard = co::addNode(c, nodetypes::MatteControl, {});
        c.node(hard)->params.setBase(nodekeys::MatteLow, 0.6);
        co::connect(c, soft, 0, hard, nodeports::Input);
        co::connect(c, hard, 0, c.mediaOutId, nodeports::Input);
        const QVector<QImage> softImgs = nodeImages(compTimeline(c), soft);
        for (const QImage& img : softImgs) {
            const int edge = px(img, 120, 90).a;
            QVERIFY2(edge > 60 && edge < 200, qPrintable(QString::number(edge)));
            QVERIFY2(near(px(img, 160, 90), 255, 255, 255, 255, 2), describe(px(img, 160, 90)));
        }
        QVERIFY(differing(softImgs[0], softImgs[1], 10) < 300);
        // Threshold Low 0.6 drops the faint outer part of the soft edge.
        const QVector<QImage> hardImgs = nodeImages(compTimeline(c), hard);
        for (const QImage& img : hardImgs)
            QVERIFY2(px(img, 112, 90).a < px(softImgs[0], 112, 90).a, describe(px(img, 112, 90)));
        // Restore Fringe brings part of it back (closer to the solid than the threshold left it).
        c.node(hard)->params.setBase(nodekeys::RestoreFringe, 1.0);
        const QVector<QImage> restored = nodeImages(compTimeline(c), hard);
        for (const QImage& img : restored)
            QVERIFY2(px(img, 119, 90).a > px(hardImgs[0], 119, 90).a + 8, describe(px(img, 119, 90)));
        int worst = 0;
        const int bad = differing(restored[0], restored[1], 12, &worst);
        QVERIFY2(bad < 400, qPrintable(QStringLiteral("%1 pixels differ, worst %2").arg(bad).arg(worst)));
        // Invert Matte flips the alpha; garbage / solid mattes apply.
        c.node(hard)->params.setBase(nodekeys::RestoreFringe, 0.0);
        c.node(hard)->params.setStatic(nodekeys::Invert, true);
        for (const QImage& img : nodeImages(compTimeline(c), hard))
            QVERIFY2(px(img, 160, 90).a < 3, describe(px(img, 160, 90)));
    }

    void matteMathOnItsOwn()
    {
        // Gamma, clean and threshold on single values; restore fringe needs a solid neighbour.
        compmath::MatteFilterParams m;
        m.low = 0.3f;
        m.high = 0.8f;
        QCOMPARE(compmath::matteShape(m, 0.2f), 0.0f);
        QVERIFY(std::abs(compmath::matteShape(m, 0.55f) - 0.5f) < 1e-4f);
        QCOMPARE(compmath::matteShape(m, 0.9f), 1.0f);
        m.gamma = 2.0f; // raises the middle
        QVERIFY(compmath::matteShape(compmath::MatteFilterParams(), 0.25f) < 0.26f);
        compmath::MatteFilterParams g;
        g.gamma = 2.0f;
        QVERIFY(compmath::matteShape(g, 0.25f) > 0.45f);
        m.restoreFringe = 1.0f;
        m.gamma = 1.0f;
        QVERIFY(compmath::matteRestore(m, 0.2f, 0.0f, 0.5f) > 0.19f); // next to solid: restored
        QCOMPARE(compmath::matteRestore(m, 0.2f, 0.0f, 0.0f), 0.0f); // far from it: not
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

    void bezierMaskGpuMatchesCpu()
    {
        // A Bezier mask node (spec 117): a curved shape with uneven tangents, a
        // rotation and a smaller size. Both renderers fill the same shape; the Soft Edge
        // section below feathers it.
        Composition c = co::defaultComposition();
        const quint64 bz = co::addNode(c, nodetypes::BezierMask, {});
        CompNode& n = *c.node(bz);
        n.path.setShape(0,
            {{-60, -40, 0, 0, 50, -30, true}, {70, -30, -30, -40, 0, 50, true},
                {40, 50, 40, 20, -50, 0, true}, {-50, 40, 30, 20, -10, -50, true}});
        n.params.setBase(nodekeys::Angle, 20.0);
        n.params.setBase(nodekeys::Size, 0.8);
        const QVector<QImage> imgs = nodeImages(compTimeline(c), bz);
        QVERIFY(!imgs[0].isNull() && !imgs[1].isNull());
        for (const QImage& img : imgs) {
            QVERIFY2(px(img, 160, 90).a > 240, describe(px(img, 160, 90)));
            QCOMPARE(px(img, 3, 3).a, 0);
            QCOMPARE(px(img, 316, 176).a, 0);
        }
        int worst = 0;
        const int bad = differing(imgs[0], imgs[1], 12, &worst);
        // The GPU rasterises the curve with a slightly smoother edge than the CPU's painter, so
        // edge pixels differ; the covered area and the shape agree.
        const double areaGpu = alphaSum(imgs[0]), areaCpu = alphaSum(imgs[1]);
        QVERIFY2(std::abs(areaGpu - areaCpu) < 0.02 * areaCpu,
            qPrintable(QStringLiteral("areas %1 %2").arg(areaGpu).arg(areaCpu)));
        QVERIFY2(bad < 700, qPrintable(QStringLiteral("%1 pixels differ, worst %2").arg(bad).arg(worst)));
        // Inverted: the picture flips on both.
        n.params.setStatic(nodekeys::Invert, true);
        const QVector<QImage> inv = nodeImages(compTimeline(c), bz);
        for (const QImage& img : inv) {
            QVERIFY(px(img, 160, 90).a < 15);
            QVERIFY(px(img, 3, 3).a > 240);
        }
        QVERIFY(differing(inv[0], inv[1], 12) < 700);
        // Soft Edge: the edge fades out over a band on both renderers and they agree.
        n.params.setStatic(nodekeys::Invert, false);
        const QVector<QImage> hard = nodeImages(compTimeline(c), bz);
        n.params.setBase(nodekeys::SoftEdge, 0.1);
        const QVector<QImage> soft = nodeImages(compTimeline(c), bz);
        for (int i = 0; i < 2; ++i) {
            int partial = 0, partialHard = 0;
            for (int y = 0; y < soft[i].height(); ++y)
                for (int x = 0; x < soft[i].width(); ++x) {
                    const int a = px(soft[i], x, y).a;
                    partial += a > 25 && a < 230;
                    const int ah = px(hard[i], x, y).a;
                    partialHard += ah > 25 && ah < 230;
                }
            QVERIFY2(partial > 4 * partialHard + 200, qPrintable(QString::number(partial)));
        }
        int softWorst = 0;
        const int softBad = differing(soft[0], soft[1], 16, &softWorst);
        QVERIFY2(softBad < 1500,
            qPrintable(QStringLiteral("%1 soft pixels differ, worst %2").arg(softBad).arg(softWorst)));
        const double softGpu = alphaSum(soft[0]), softCpu = alphaSum(soft[1]);
        QVERIFY2(std::abs(softGpu - softCpu) < 0.03 * softCpu,
            qPrintable(QStringLiteral("soft areas %1 %2").arg(softGpu).arg(softCpu)));
    }

    void multiPatternTrackerGpuMatchesCpu()
    {
        // A Tracker node with two patterns (spec 118): the second goes to a point twice as far
        // away and a quarter turn round by frame 10, so the picture is moved, scaled and turned.
        Composition c = co::defaultComposition();
        const quint64 box = colorSource(c, Qt::white, true, 130, 70, 190, 110);
        const quint64 tr = co::addNode(c, nodetypes::Tracker, {});
        co::connect(c, box, 0, tr, nodeports::Input);
        co::connect(c, tr, 0, c.mediaOutId, nodeports::Input);
        ParamSet& q = c.node(tr)->params;
        q.setBase(nodekeys::Patterns, 2);
        q.setBase(nodekeys::patternKey(2, false), 0.5 + 32.0 / kW);
        q.setBase(nodekeys::patternKey(2, true), 0.5);
        q.animated(nodekeys::patternKey(2, false), 0.6).setKey(0, 0.5 + 32.0 / kW);
        q.animated(nodekeys::patternKey(2, false), 0.6).setKey(10, 0.5 + 8.0 / kW);
        q.animated(nodekeys::patternKey(2, true), 0.5).setKey(0, 0.5);
        q.animated(nodekeys::patternKey(2, true), 0.5).setKey(10, 0.5 + 20.0 / kH);
        const QSizeF fs(kW, kH);
        const QTransform m = compmath::trackerMatrix(q, 10, fs);
        QVERIFY(!m.isIdentity());
        QVERIFY(std::hypot(m.m11(), m.m12()) < 0.9); // shrunk
        const TimelineData tl = compTimeline(c);
        const QVector<QImage> at0 = nodeImages(tl, tr, 0);
        const QVector<QImage> at10 = nodeImages(tl, tr, 10);
        // Frame 0 is the reference: the box is where it was, on both.
        for (const QImage& img : at0) {
            QVERIFY(px(img, 160, 90).a > 240);
            QVERIFY(px(img, 135, 90).a > 240);
        }
        // Frame 10: the box has moved with the group's middle.
        const QPointF dest = m.map(QPointF(160, 90));
        for (const QImage& img : at10) {
            QVERIFY2(px(img, int(dest.x()), int(dest.y())).a > 240, describe(px(img, int(dest.x()), int(dest.y()))));
            QVERIFY(px(img, 135, 90).a < 15 || std::abs(dest.x() - 160) > 1); // not left in place
        }
        QVERIFY(differing(at0[0], at0[1], 12) < 250);
        int worst = 0;
        const int bad = differing(at10[0], at10[1], 14, &worst);
        QVERIFY2(bad < 300, qPrintable(QStringLiteral("%1 pixels differ, worst %2").arg(bad).arg(worst)));
        // Stabilize inverts: the same on both renderers.
        q.statics[nodekeys::TrackMode] = trackmodes::Stabilize;
        const QVector<QImage> stab = nodeImages(compTimeline(c), tr, 10);
        QVERIFY(differing(stab[0], stab[1], 14) < 300);
        QVERIFY(differing(stab[0], at10[0], 14) > 100); // a different picture
    }

    void linkedAndModifiedParametersRenderTheSame()
    {
        // Spec 119.8: a Transform whose Center X is connected to another node's and whose Center Y
        // is an expression of the frame draws the same on the GPU and on the CPU.
        Composition c = co::defaultComposition();
        const quint64 box = colorSource(c, Qt::white, true, 130, 70, 190, 110);
        const quint64 other = co::addNode(c, nodetypes::Transform, {});
        c.node(other)->params.setBase(nodekeys::CenterX, 0.7);
        const quint64 tr = co::addNode(c, nodetypes::Transform, {});
        co::connect(c, box, 0, tr, nodeports::Input);
        co::connect(c, tr, 0, c.mediaOutId, nodeports::Input);
        QCOMPARE(complinks::connectParam(c, tr, nodekeys::CenterX, {other, nodekeys::CenterX}),
            complinks::LinkError::None);
        const QString id = modifiers::add(c.node(tr)->params, nodekeys::CenterY, ModifierKind::Expression);
        modifiers::find(c.node(tr)->params, nodekeys::CenterY, id)->text = QStringLiteral("0.5 + time / 100");
        const TimelineData tl = compTimeline(c);
        for (const int64_t frame : {int64_t(0), int64_t(20)}) {
            const QVector<QImage> imgs = nodeImages(tl, tr, frame);
            // The box (60 x 40 px, centre 160, 90) moved right by 0.2 * 320 = 64 px, and up by
            // frame / 100 * 180 px (the Transform's Y goes up).
            const int cx = 160 + 64, cy = 90 - int(std::lround(frame * 1.8));
            for (const QImage& img : imgs) {
                QVERIFY(!img.isNull());
                QVERIFY2(px(img, cx, cy).a > 240, qPrintable(QStringLiteral("frame %1: %2").arg(frame).arg(describe(px(img, cx, cy)))));
                QVERIFY(px(img, 160, 90).a < 15);
            }
            int worst = 0;
            const int bad = differing(imgs[0], imgs[1], 14, &worst);
            QVERIFY2(bad < 300, qPrintable(QStringLiteral("frame %1: %2 pixels differ, worst %3").arg(frame).arg(bad).arg(worst)));
        }
        // Without the link the box would stay at the middle: the link made the difference.
        c.node(tr)->params.values[nodekeys::CenterX].modifiers.clear();
        for (const QImage& img : nodeImages(compTimeline(c), tr, 0))
            QVERIFY(px(img, 160, 90).a > 240);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestCompFilters>(argc, argv);
}
#include "test_compfilters.moc"
