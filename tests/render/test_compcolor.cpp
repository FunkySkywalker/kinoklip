// SPDX-License-Identifier: GPL-3.0-or-later
// Pixel tests of the colour nodes of spec 124: Brightness Contrast, Alpha Multiply / Divide,
// Channel Mapper, Color Curves, Hue Curves, the Custom Tool and Vector Motion Blur. Every case
// renders on the GPU and on the CPU and checks both against the expectation and against each
// other.
#include "RenderTestUtil.h"
#include "core/CompLinks.h"
#include "core/CompOps.h"
#include "core/CurveParam.h"
#include "core/Serialization.h"
#include "core/NodeRegistry.h"
#include "render/CompColorMath.h"
#include "render/CompMath.h"
#include "render/CompWarpMath.h"
#include "render/Compositor.h"

#include <QJsonDocument>
#include <QTest>

#include <cmath>
#include <limits>
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

class TestCompColor : public QObject
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

    /// GPU and CPU picture of @p id: both there and no more than @p maxBad pixels off by > @p tol.
    void agree(const TimelineData& tl, quint64 id, int tol = 3, int maxBad = 40)
    {
        const QVector<QImage> imgs = nodeImages(tl, id);
        QVERIFY(!imgs[0].isNull() && !imgs[1].isNull());
        int worst = 0;
        const int bad = differing(imgs[0], imgs[1], tol, &worst);
        QVERIFY2(bad <= maxBad,
            qPrintable(QStringLiteral("%1 pixels differ, worst %2").arg(bad).arg(worst)));
    }

    /// A picture of one node @p type on a coloured cropped box (the colour @p box at 100..220 x
    /// 40..140 over a transparent frame): the node's id in @p node.
    Composition boxSetup(const QString& type, QColor box, quint64* node, QColor back = QColor())
    {
        Composition c = co::defaultComposition();
        const quint64 src = colorSource(c, box, true, 100, 40, 220, 140);
        *node = co::addNode(c, type, {});
        if (back.isValid()) {
            const quint64 bg = colorSource(c, back, false);
            const quint64 m = co::addNode(c, nodetypes::Merge, {});
            co::connect(c, bg, 0, m, nodeports::Background);
            co::connect(c, src, 0, m, nodeports::Foreground);
            co::connect(c, m, 0, *node, nodeports::Input);
        } else {
            co::connect(c, src, 0, *node, nodeports::Input);
        }
        co::connect(c, *node, 0, c.mediaOutId, nodeports::Input);
        return c;
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

    void brightnessContrastMatchesItsFormula()
    {
        quint64 n = 0;
        Composition c = boxSetup(nodetypes::BrightnessContrast, QColor(100, 80, 60), &n);
        ParamSet& p = c.node(n)->params;
        // Defaults change nothing.
        for (const QImage& img : nodeImages(compTimeline(c), n))
            QVERIFY2(near(px(img, 160, 90), 100, 80, 60, 255, 1), describe(px(img, 160, 90)));
        // Gain 1.5.
        p.setBase(nodekeys::BcGain, 1.5);
        for (const QImage& img : nodeImages(compTimeline(c), n))
            QVERIFY2(near(px(img, 160, 90), 150, 120, 90, 255, 2), describe(px(img, 160, 90)));
        // Every control at once, against the reference maths.
        p.setBase(nodekeys::BcGain, 1.2);
        p.setBase(nodekeys::BcLift, 0.1);
        p.setBase(nodekeys::BcGamma, 1.4);
        p.setBase(nodekeys::BcContrast, 0.5);
        p.setBase(nodekeys::BcBrightness, -0.05);
        p.setBase(nodekeys::Saturation, 0.6);
        p.setBase(nodekeys::BcLow, 0.05);
        p.setBase(nodekeys::BcHigh, 0.9);
        const compmath::BrightnessContrastParams b = compmath::brightnessContrastParams(p, 0);
        float want[4] = {100 / 255.0f, 80 / 255.0f, 60 / 255.0f, 1};
        compmath::applyBrightnessContrast(b, want);
        const QVector<QImage> imgs = nodeImages(compTimeline(c), n);
        for (const QImage& img : imgs) {
            const Px got = px(img, 160, 90);
            QVERIFY2(near(got, int(std::lround(std::clamp(want[0], 0.0f, 1.0f) * 255)),
                         int(std::lround(std::clamp(want[1], 0.0f, 1.0f) * 255)),
                         int(std::lround(std::clamp(want[2], 0.0f, 1.0f) * 255)), 255, 2),
                describe(got));
        }
        agree(compTimeline(c), n, 2, 5);
        // A channel switched off keeps its value: Red off leaves red at 100.
        p.setStatic(nodekeys::BcRed, false);
        p.setBase(nodekeys::Saturation, 1.0);
        for (const QImage& img : nodeImages(compTimeline(c), n))
            QVERIFY2(std::abs(px(img, 160, 90).r - 100) <= 1, describe(px(img, 160, 90)));
        // Saturation 0: grey.
        p.setStatic(nodekeys::BcRed, true);
        p.setBase(nodekeys::BcGain, 1.0);
        p.setBase(nodekeys::BcLift, 0.0);
        p.setBase(nodekeys::BcGamma, 1.0);
        p.setBase(nodekeys::BcContrast, 0.0);
        p.setBase(nodekeys::BcBrightness, 0.0);
        p.setBase(nodekeys::BcLow, 0.0);
        p.setBase(nodekeys::BcHigh, 1.0);
        p.setBase(nodekeys::Saturation, 0.0);
        for (const QImage& img : nodeImages(compTimeline(c), n)) {
            const Px g = px(img, 160, 90);
            QVERIFY2(std::abs(g.r - g.g) <= 1 && std::abs(g.g - g.b) <= 1, describe(g));
            QVERIFY2(std::abs(g.g - 82) <= 2, describe(g)); // Rec. 709 luma of (100, 80, 60)
        }
        // Alpha on: the alpha channel goes through the same chain (Gain 0.5 halves it).
        p.setBase(nodekeys::Saturation, 1.0);
        p.setBase(nodekeys::BcGain, 0.5);
        p.setStatic(nodekeys::BcAlpha, true);
        for (const QImage& img : nodeImages(compTimeline(c), n)) {
            QVERIFY2(std::abs(px(img, 160, 90).a - 128) <= 2, describe(px(img, 160, 90)));
            QVERIFY2(px(img, 20, 20).a < 3, describe(px(img, 20, 20)));
        }
    }

    void alphaMultiplyAndDivide()
    {
        // A box of straight colour (200, 100, 50) at alpha 128.
        quint64 mul = 0;
        Composition c = boxSetup(nodetypes::AlphaMultiply, QColor(200, 100, 50, 128), &mul);
        for (const QImage& img : nodeImages(compTimeline(c), mul)) {
            // Straight colour times 0.5 = (100, 50, 25), stored premultiplied by 0.5.
            const Px p = px(img, 160, 90);
            QVERIFY2(near(p, 50, 25, 13, 128, 2), describe(p));
        }
        agree(compTimeline(c), mul, 2, 5);
        // Divide then multiply restores the colour of the source.
        quint64 div = 0;
        Composition d = boxSetup(nodetypes::AlphaDivide, QColor(200, 100, 50, 128), &div);
        for (const QImage& img : nodeImages(compTimeline(d), div)) {
            // Straight (200, 100, 50) / 0.5 stays at most 1: (1, 0.78, 0.39), stored x 0.5.
            const Px p = px(img, 160, 90);
            QVERIFY2(near(p, 128, 100, 50, 128, 2), describe(p));
            QVERIFY2(px(img, 20, 20).a < 3, describe(px(img, 20, 20)));
        }
        agree(compTimeline(d), div, 2, 5);
        // The pair through two nodes is the source again.
        const quint64 mul2 = co::addNode(d, nodetypes::AlphaMultiply, {});
        co::connect(d, div, 0, mul2, nodeports::Input);
        co::connect(d, mul2, 0, d.mediaOutId, nodeports::Input);
        for (const QImage& img : nodeImages(compTimeline(d), mul2)) {
            const Px p = px(img, 160, 90);
            QVERIFY2(near(p, 64, 50, 25, 128, 3), describe(p)); // (1, 0.78, 0.39) * 0.5 * 0.5 ...
        }
    }

    void channelMapperMixesChannels()
    {
        quint64 n = 0;
        Composition c = boxSetup(nodetypes::ChannelMapper, QColor(200, 100, 50), &n);
        ParamSet& p = c.node(n)->params;
        for (const QImage& img : nodeImages(compTimeline(c), n))
            QVERIFY2(near(px(img, 160, 90), 200, 100, 50, 255, 1), describe(px(img, 160, 90)));
        // Swap red and blue.
        p.setBase(nodekeys::mapperKey(0, 0), 0.0);
        p.setBase(nodekeys::mapperKey(0, 2), 1.0);
        p.setBase(nodekeys::mapperKey(2, 2), 0.0);
        p.setBase(nodekeys::mapperKey(2, 0), 1.0);
        for (const QImage& img : nodeImages(compTimeline(c), n))
            QVERIFY2(near(px(img, 160, 90), 50, 100, 200, 255, 1), describe(px(img, 160, 90)));
        // Green is half of itself plus a quarter of red plus an offset of 0.1.
        p.setBase(nodekeys::mapperKey(1, 1), 0.5);
        p.setBase(nodekeys::mapperKey(1, 0), 0.25);
        p.setBase(nodekeys::mapperOffsetKey(1), 0.1);
        const int green = int(std::lround((0.5 * 100 / 255.0 + 0.25 * 200 / 255.0 + 0.1) * 255));
        for (const QImage& img : nodeImages(compTimeline(c), n))
            QVERIFY2(near(px(img, 160, 90), 50, green, 200, 255, 2), describe(px(img, 160, 90)));
        agree(compTimeline(c), n, 2, 5);
        // Alpha from the red channel: the box turns half transparent, the outside stays clear.
        p.setBase(nodekeys::mapperKey(3, 3), 0.0);
        p.setBase(nodekeys::mapperKey(3, 2), 1.0); // blue after the swap source is red 200 -> input blue = 50
        for (const QImage& img : nodeImages(compTimeline(c), n)) {
            const int a = px(img, 160, 90).a;
            QVERIFY2(std::abs(a - 50) <= 2, qPrintable(QString::number(a))); // input blue 50
            QVERIFY2(px(img, 20, 20).a < 3, describe(px(img, 20, 20)));
        }
        agree(compTimeline(c), n, 2, 5);
    }

    void colorCurvesBendChannels()
    {
        quint64 n = 0;
        Composition c = boxSetup(nodetypes::ColorCurves, QColor(128, 64, 200), &n);
        ParamSet& p = c.node(n)->params;
        for (const QImage& img : nodeImages(compTimeline(c), n))
            QVERIFY2(near(px(img, 160, 90), 128, 64, 200, 255, 1), describe(px(img, 160, 90)));
        // A master curve lifting the middle: (0.5 -> 0.8).
        p.setStatic(nodekeys::CurveLuma, QStringLiteral("0,0 0.5,0.8 1,1"));
        const compmath::CurvesNodeParams cp = compmath::colorCurvesParams(p);
        float want[4] = {128 / 255.0f, 64 / 255.0f, 200 / 255.0f, 1};
        compmath::applyCurvesNode(cp, want);
        QVERIFY(want[0] > 0.75f && want[0] < 0.82f);
        for (const QImage& img : nodeImages(compTimeline(c), n)) {
            const Px g = px(img, 160, 90);
            QVERIFY2(near(g, int(std::lround(want[0] * 255)), int(std::lround(want[1] * 255)),
                         int(std::lround(want[2] * 255)), 255, 2),
                describe(g));
        }
        agree(compTimeline(c), n, 2, 5);
        // The red curve inverted: red becomes 255 - 128.
        p.setStatic(nodekeys::CurveLuma, QStringLiteral("0,0 1,1"));
        p.setStatic(nodekeys::CurveRed, QStringLiteral("0,1 1,0"));
        for (const QImage& img : nodeImages(compTimeline(c), n))
            QVERIFY2(near(px(img, 160, 90), 127, 64, 200, 255, 3), describe(px(img, 160, 90)));
        // The alpha curve: alpha 255 maps to 0.5 -> half transparent, outside stays clear.
        p.setStatic(nodekeys::CurveRed, QStringLiteral("0,0 1,1"));
        p.setStatic(nodekeys::CurveAlpha, QStringLiteral("0,0 1,0.5"));
        for (const QImage& img : nodeImages(compTimeline(c), n)) {
            QVERIFY2(std::abs(px(img, 160, 90).a - 128) <= 2, describe(px(img, 160, 90)));
            QVERIFY2(px(img, 20, 20).a < 3, describe(px(img, 20, 20)));
        }
        agree(compTimeline(c), n, 2, 5);
    }

    void hueCurvesChangeColours()
    {
        quint64 n = 0;
        Composition c = boxSetup(nodetypes::HueCurves, QColor(200, 60, 60), &n);
        ParamSet& p = c.node(n)->params;
        for (const QImage& img : nodeImages(compTimeline(c), n))
            QVERIFY2(near(px(img, 160, 90), 200, 60, 60, 255, 1), describe(px(img, 160, 90)));
        // Hue vs Sat at 0 everywhere: grey.
        p.setStatic(nodekeys::CurveHueSat, QStringLiteral("0,0 1,0"));
        for (const QImage& img : nodeImages(compTimeline(c), n)) {
            const Px g = px(img, 160, 90);
            QVERIFY2(std::abs(g.r - g.g) <= 2 && std::abs(g.g - g.b) <= 2, describe(g));
        }
        agree(compTimeline(c), n, 2, 5);
        // Hue vs Hue at 0.75 everywhere: a quarter turn, red goes towards yellow-green.
        p.setStatic(nodekeys::CurveHueSat, QStringLiteral("0,0.5 1,0.5"));
        p.setStatic(nodekeys::CurveHueHue, QStringLiteral("0,0.75 1,0.75"));
        for (const QImage& img : nodeImages(compTimeline(c), n)) {
            const Px g = px(img, 160, 90);
            QVERIFY2(g.g > g.r && g.g > g.b + 30, describe(g));
        }
        agree(compTimeline(c), n, 2, 5);
        // Lum vs Sat and Hue vs Lum are in step on both renderers too.
        p.setStatic(nodekeys::CurveHueHue, QStringLiteral("0,0.5 1,0.5"));
        p.setStatic(nodekeys::CurveHueLum, QStringLiteral("0,0.8 1,0.8"));
        p.setStatic(nodekeys::CurveLumSat, QStringLiteral("0,0.5 0.5,0.2 1,0.5"));
        agree(compTimeline(c), n, 3, 5);
    }

    void customToolComputesPixels()
    {
        quint64 n = 0;
        Composition c = boxSetup(nodetypes::CustomTool, QColor(100, 50, 200), &n);
        ParamSet& p = c.node(n)->params;
        // Defaults pass the input through.
        for (const QImage& img : nodeImages(compTimeline(c), n))
            QVERIFY2(near(px(img, 160, 90), 100, 50, 200, 255, 1), describe(px(img, 160, 90)));
        // Red inverted, green times Number 1, blue from the position, alpha as it is.
        p.setStatic(nodekeys::CustomRed, QStringLiteral("1 - r1"));
        p.setStatic(nodekeys::CustomGreen, QStringLiteral("g1 * n1"));
        p.setStatic(nodekeys::CustomBlue, QStringLiteral("x"));
        p.setBase(nodekeys::customNumberKey(0), 0.5);
        const QVector<QImage> imgs = nodeImages(compTimeline(c), n);
        for (const QImage& img : imgs) {
            const int blue = int(std::lround((160 + 0.5) / 320.0 * 255));
            QVERIFY2(near(px(img, 160, 90), 155, 25, blue, 255, 2), describe(px(img, 160, 90)));
            const int blue2 = int(std::lround((200 + 0.5) / 320.0 * 255));
            QVERIFY2(near(px(img, 200, 90), 155, 25, blue2, 255, 2), describe(px(img, 200, 90)));
            QVERIFY2(px(img, 20, 20).a < 3, describe(px(img, 20, 20)));
        }
        agree(compTimeline(c), n, 2, 5);
        // Intermediates: i1 = 2 * x, i2 = i1 - 1 (x above the middle), Red = clamp(i2, 0, 1) and
        // Blue picks y (up): 90 / 180 = the middle row.
        p.setStatic(nodekeys::customInterKey(0), QStringLiteral("2 * x"));
        p.setStatic(nodekeys::customInterKey(1), QStringLiteral("i1 - 1"));
        p.setStatic(nodekeys::CustomRed, QStringLiteral("clamp(i2, 0, 1)"));
        p.setStatic(nodekeys::CustomBlue, QStringLiteral("y"));
        for (const QImage& img : nodeImages(compTimeline(c), n)) {
            QVERIFY2(px(img, 130, 90).r < 6, describe(px(img, 130, 90)));
            const int r = int(std::lround((2 * (200 + 0.5) / 320.0 - 1) * 255));
            QVERIFY2(std::abs(px(img, 200, 90).r - r) <= 2, describe(px(img, 200, 90)));
            QVERIFY2(std::abs(px(img, 200, 90).b - 128) <= 2, describe(px(img, 200, 90)));
        }
        agree(compTimeline(c), n, 2, 5);
        // A formula that does not compile leaves its channel as it is and reports why.
        p.setStatic(nodekeys::CustomGreen, QStringLiteral("g1 +"));
        p.setStatic(nodekeys::customInterKey(0), QStringLiteral("foo(1)"));
        const compmath::CustomToolParams bad = compmath::customToolParams(p, 0);
        QVERIFY(!bad.error[4 + 1].isEmpty()); // Green
        QVERIFY(!bad.error[0].isEmpty());     // Intermediate 1
        for (const QImage& img : nodeImages(compTimeline(c), n))
            QVERIFY2(std::abs(px(img, 160, 90).g - 50) <= 2, describe(px(img, 160, 90)));
        // Time: Alpha = time / 10 at frame 5.
        p.setStatic(nodekeys::CustomGreen, QStringLiteral("g1"));
        p.setStatic(nodekeys::customInterKey(0), QString());
        p.setStatic(nodekeys::CustomAlpha, QStringLiteral("time / 10"));
        for (const QImage& img : nodeImages(compTimeline(c), n, 5))
            QVERIFY2(std::abs(px(img, 160, 90).a - 128) <= 2, describe(px(img, 160, 90)));
    }

    void customToolFormulasAgreeOnBothRenderers()
    {
        // A zoo of the functions and operators, GPU against CPU (the shader is generated from
        // the same tree).
        quint64 n = 0;
        Composition c = boxSetup(nodetypes::CustomTool, QColor(180, 90, 30), &n);
        ParamSet& p = c.node(n)->params;
        p.setBase(nodekeys::customNumberKey(0), 1.7);
        p.setBase(nodekeys::customNumberKey(1), -0.4);
        p.setStatic(nodekeys::CustomRed,
            QStringLiteral("abs(sin(x * 6.28 + n1)) * 0.5 + smoothstep(0.2, 0.8, y) * 0.5"));
        p.setStatic(nodekeys::CustomGreen,
            QStringLiteral("(n2 ^ 2) + (n2 ^ 3) * 0.5 + mix(g1, b1, 0.5) * (r1 > 0.5 ? 1 : 0.4)"));
        p.setStatic(nodekeys::CustomBlue,
            QStringLiteral("pow(b1, 1.5) + fract(x * 4) * 0.3 - floor(y * 3) * 0.1 + sqrt(max(n2, 0)) + log(r1 + 1) * 0.2"));
        p.setStatic(nodekeys::CustomAlpha, QStringLiteral("a1 * (1 - step(0.9, x)) / (n1 - 1.7 + 1)"));
        agree(compTimeline(c), n, 3, 30);
        const QVector<QImage> imgs = nodeImages(compTimeline(c), n);
        // Division by zero is 0, not an error: Alpha = a1 / 0 = 0.
        p.setStatic(nodekeys::CustomAlpha, QStringLiteral("a1 / (n1 - 1.7)"));
        for (const QImage& img : nodeImages(compTimeline(c), n))
            QVERIFY2(px(img, 160, 90).a < 3, describe(px(img, 160, 90)));
        // Two pictures: Red from Image 2.
        const quint64 second = colorSource(c, QColor(10, 220, 30), false);
        co::connect(c, second, 0, n, nodeports::Image2);
        p.setStatic(nodekeys::CustomAlpha, QStringLiteral("a1"));
        p.setStatic(nodekeys::CustomRed, QStringLiteral("g2"));
        p.setStatic(nodekeys::CustomGreen, QStringLiteral("r2 + b2"));
        p.setStatic(nodekeys::CustomBlue, QStringLiteral("a2 * 0.5"));
        int which = 0;
        for (const QImage& img : nodeImages(compTimeline(c), n))
            QVERIFY2(near(px(img, 160, 90), 220, 40, 128, 255, 2),
                qPrintable(QStringLiteral("%1: %2 (image null: %3)")
                               .arg(which++ == 0 ? "GPU" : "CPU", describe(px(img, 160, 90)))
                               .arg(img.isNull())));
        agree(compTimeline(c), n, 2, 5);
    }

    void expressionPixelMaths()
    {
        QString why;
        const QStringList vars {QStringLiteral("x"), QStringLiteral("y")};
        const auto eval = [&](const QString& text, double x, double y) {
            const auto prog = expr::Program::compilePixel(text, vars, &why);
            if (!prog)
                return std::numeric_limits<double>::quiet_NaN();
            const double v[2] = {x, y};
            return prog->evaluatePixel(0.0, v);
        };
        QCOMPARE(eval(QStringLiteral("x + y * 2"), 1, 3), 7.0);
        QCOMPARE(eval(QStringLiteral("x / y"), 1, 0), 0.0);       // no division error
        QCOMPARE(eval(QStringLiteral("(-8) ^ 3"), 0, 0), -512.0); // odd power of a negative base
        QCOMPARE(eval(QStringLiteral("(-2) ^ 2"), 0, 0), 4.0);
        QCOMPARE(eval(QStringLiteral("(-2) ^ 0.5"), 0, 0), 0.0);
        QCOMPARE(eval(QStringLiteral("sqrt(0 - 4)"), 0, 0), 0.0);
        QCOMPARE(eval(QStringLiteral("round(2.5) + round(-2.5)"), 0, 0), 0.0);
        QCOMPARE(eval(QStringLiteral("X + Y"), 1, 2), 3.0); // names in any case
        QVERIFY(std::isnan(eval(QStringLiteral("z + 1"), 0, 0)));   // an unknown name
        QVERIFY(!why.isEmpty());
        QVERIFY(std::isnan(eval(QStringLiteral("noise(x)"), 0, 0))); // not in a pixel formula
        QVERIFY(std::isnan(eval(QStringLiteral("Blur1.size"), 0, 0)));
        // The GLSL has the same shape: every variable, the helpers, no references.
        const auto prog = expr::Program::compilePixel(QStringLiteral("x * 2 + sin(y)"), vars);
        const QString glsl = prog->toGlsl(vars);
        QVERIFY(glsl.contains(QLatin1String("v_x")) && glsl.contains(QLatin1String("sin(")));
        QVERIFY(expr::Program::pixelGlslHelpers().contains(QLatin1String("kk_div")));
    }

    void vectorMotionBlurAlongVectors()
    {
        Composition c = co::defaultComposition();
        const quint64 box = colorSource(c, Qt::white, true, 130, 60, 190, 120);
        // Vectors: red 1.0 = +32 px to the right at Vector Range 32 (green 0.5 = no vertical).
        const quint64 map = colorSource(c, QColor(255, 128, 0), false);
        const quint64 vb = co::addNode(c, nodetypes::VectorMotionBlur, {});
        co::connect(c, box, 0, vb, nodeports::Input);
        co::connect(c, map, 0, vb, nodeports::Map);
        co::connect(c, vb, 0, c.mediaOutId, nodeports::Input);
        const QVector<QImage> imgs = nodeImages(compTimeline(c), vb);
        for (const QImage& img : imgs) {
            // The box is smeared along x by about 32 px, centred: soft edges left and right,
            // nothing changed in y, far outside still empty.
            const int left = px(img, 122, 90).a, right = px(img, 197, 90).a;
            QVERIFY2(left > 20 && left < 235, qPrintable(QString::number(left)));
            QVERIFY2(right > 20 && right < 235, qPrintable(QString::number(right)));
            QVERIFY2(px(img, 160, 90).a > 250, describe(px(img, 160, 90)));
            QVERIFY2(px(img, 160, 50).a < 3, describe(px(img, 160, 50))); // above the box: no y blur
            QVERIFY2(px(img, 60, 90).a < 3, describe(px(img, 60, 90)));
        }
        int worst = 0;
        const int bad = differing(imgs[0], imgs[1], 8, &worst);
        QVERIFY2(bad < 150, qPrintable(QStringLiteral("%1 pixels differ, worst %2").arg(bad).arg(worst)));
        // Center Bias 1: the blur trails only behind the motion (to the left of a box moving right).
        c.node(vb)->params.setBase(nodekeys::VmbBias, 1.0);
        for (const QImage& img : nodeImages(compTimeline(c), vb)) {
            QVERIFY2(px(img, 122, 90).a > 20, describe(px(img, 122, 90)));
            QVERIFY2(px(img, 197, 90).a < 10, describe(px(img, 197, 90)));
        }
        // Scale 0: no blur.
        c.node(vb)->params.setBase(nodekeys::VmbScale, 0.0);
        for (const QImage& img : nodeImages(compTimeline(c), vb)) {
            QVERIFY2(px(img, 122, 90).a < 3, describe(px(img, 122, 90)));
            QVERIFY2(px(img, 160, 90).a > 250, describe(px(img, 160, 90)));
        }
    }

    void newNodesAreKeyframeableAndSerialise()
    {
        // Numbers are keyframeable, the curve and formula strings are saved, an older file
        // without any of the keys loads with the defaults.
        Composition c = co::defaultComposition();
        const quint64 bc = co::addNode(c, nodetypes::BrightnessContrast, {});
        const quint64 cc = co::addNode(c, nodetypes::ColorCurves, {});
        const quint64 ct = co::addNode(c, nodetypes::CustomTool, {});
        c.node(bc)->params.animated(nodekeys::BcGain, 1.0).setKey(0, 1.0);
        c.node(bc)->params.animated(nodekeys::BcGain, 1.0).setKey(10, 2.0);
        c.node(cc)->params.setStatic(nodekeys::CurveLuma, QStringLiteral("0,0 0.5,0.8 1,1"));
        c.node(ct)->params.setStatic(nodekeys::CustomRed, QStringLiteral("1 - r1"));
        c.node(ct)->params.animated(nodekeys::customNumberKey(2), 0.0).setKey(0, 0.0);
        QCOMPARE(compmath::brightnessContrastParams(c.node(bc)->params, 5).gain, 1.5f);
        const Composition back
            = serialization::compositionFromJson(serialization::compositionToJson(c));
        QCOMPARE(back, c);
        QCOMPARE(back.node(cc)->params.staticValue(nodekeys::CurveLuma).toString(),
            QStringLiteral("0,0 0.5,0.8 1,1"));
        QCOMPARE(back.node(ct)->params.staticValue(nodekeys::CustomRed).toString(),
            QStringLiteral("1 - r1"));
        // A node written before these keys existed: the defaults apply.
        const QByteArray text = R"({"nodes": [{"id": 1, "type": "media_in"},
            {"id": 2, "type": "custom_tool", "inputs": {"input": [1, 0]}},
            {"id": 3, "type": "hue_curves", "inputs": {"input": [2, 0]}},
            {"id": 4, "type": "media_out", "inputs": {"input": [3, 0]}}], "mediaOut": 4})";
        const Composition old
            = serialization::compositionFromJson(QJsonDocument::fromJson(text).object());
        QVERIFY(compmath::customToolParams(old.node(2)->params, 0).isIdentity());
        QVERIFY(compmath::hueCurvesParams(old.node(3)->params).isIdentity());
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestCompColor>(argc, argv);
}
#include "test_compcolor.moc"
