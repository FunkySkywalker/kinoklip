// SPDX-License-Identifier: GPL-3.0-or-later
// Color page core (docs/color-page.md, spec 44a): the grade maths against reference values
// (primaries, curves, HSL curves, qualifier, windows), the node graph operations, keyframe
// paths, edits that move keys, serialization (format 16), migration of format 15 files,
// undo through Project and the gallery.
#include "CoreTestUtils.h"
#include "core/EditOps.h"
#include "core/GradeGallery.h"
#include "core/GradeMath.h"
#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/Serialization.h"
#include "core/Timeline.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <cmath>

using namespace kk;
using namespace kk::test;
using namespace kk::grademath;
namespace go = kk::gradeops;
namespace gk = kk::gradekeys;
namespace wk = kk::gradewindowkeys;

namespace {

bool near(double a, double b, double eps = 1e-4)
{
    return std::abs(a - b) <= eps;
}

#define KK_COMPARE_RGB(c, r, g, b, eps)                                                            \
    QVERIFY2(near((c)[0], r, eps) && near((c)[1], g, eps) && near((c)[2], b, eps),                 \
        qPrintable(QStringLiteral("got %1 %2 %3").arg((c)[0]).arg((c)[1]).arg((c)[2])))

/// Primaries of a node with @p values set.
PrimaryValues primaries(std::initializer_list<std::pair<const QString, double>> values)
{
    GradeNode n;
    for (const auto& [k, v] : values)
        n.params.setBase(k, v);
    return PrimaryValues::at(n, 0);
}

/// Curve tables with one curve set to @p points.
CurveTables tablesWith(GradeCurve c, CurvePoints points)
{
    GradeCurves curves;
    curves[c] = std::move(points);
    return CurveTables::bake(curves);
}

/// A grade using every feature: serial, parallel and layer nodes, keyed primaries, curves,
/// qualifier, every window shape, a LUT path and a bypassed node.
ClipGrade richGrade()
{
    ClipGrade g;
    const quint64 a = go::initialize(g);
    GradeNode* n = g.node(a);
    n->params.values[gk::GainR].setKey(0, 1.0);
    n->params.values[gk::GainR].setKey(24, 1.5);
    n->params.setBase(gk::Saturation, 1.2);
    n->label = QStringLiteral("Balance");
    const quint64 b = go::addSerialNode(g, a);
    g.node(b)->curves[GradeCurve::Luma] = {{0.25, 0.2}, {0.75, 0.8}};
    g.node(b)->curves[GradeCurve::HueSat] = {{0.0, 0.5}, {0.33, 0.9}};
    g.node(b)->curves[GradeCurve::SatSat] = {{0.5, 0.4}};
    g.node(b)->lutPath = QStringLiteral("/luts/look.cube");
    g.node(b)->lutInterpolation = LutInterpolation::Trilinear;
    const quint64 c = go::addParallelNode(g, b);
    g.node(c)->qualifier.enabled = true;
    g.node(c)->qualifier.hueCenter = 0.05;
    g.node(c)->params.setBase(gk::Mix, 0.5);
    const quint64 w1 = go::addWindow(g, c, GradeWindowShape::Circle);
    g.window(w1)->params.values[wk::CenterX].setKey(0, 0.3);
    g.window(w1)->params.values[wk::CenterX].setKey(10, 0.7);
    go::addWindow(g, c, GradeWindowShape::Linear);
    const quint64 w3 = go::addWindow(g, c, GradeWindowShape::Polygon);
    g.window(w3)->invert = true;
    go::addWindow(g, c, GradeWindowShape::Gradient);
    const quint64 d = go::addLayerNode(g, c);
    g.node(d)->enabled = false;
    return g;
}

} // namespace

class TestGrade : public QObject
{
    Q_OBJECT
private slots:
    // ---- Primaries ----

    void liftGammaGainReference()
    {
        QCOMPARE(liftGammaGain(0.5f, 0, 0, 2), 1.0f);
        QVERIFY(near(liftGammaGain(0.0f, 0.2f, 0, 1), 0.2));
        QVERIFY(near(liftGammaGain(1.0f, 0.2f, 0, 1), 1.0)); // lift leaves white alone
        QVERIFY(near(liftGammaGain(0.25f, 0, 0.5f, 1), 0.5)); // exponent 2^-1
        QVERIFY(near(liftGammaGain(0.25f, 0, -0.5f, 1), 0.0625)); // exponent 2
        QCOMPARE(liftGammaGain(-0.5f, 0, 0.3f, 1), 0.0f); // negative input clamps before pow
    }

    void primariesReference()
    {
        float c[3] = {0.2f, 0.4f, 0.6f};
        const PrimaryValues neutral = primaries({});
        QVERIFY(neutral.isIdentity());
        applyPrimaries(neutral, c);
        KK_COMPARE_RGB(c, 0.2, 0.4, 0.6, 1e-6);

        float o[3] = {0.2f, 0.3f, 0.4f};
        applyPrimaries(primaries({{gk::OffsetY, 0.1}, {gk::OffsetR, 0.05}}), o);
        KK_COMPARE_RGB(o, 0.35, 0.4, 0.5, 1e-5);

        float k[3] = {0.75f, 0.5f, 0.25f};
        applyPrimaries(primaries({{gk::Contrast, 2.0}, {gk::Pivot, 0.5}}), k);
        KK_COMPARE_RGB(k, 1.0, 0.5, 0.0, 1e-5);

        float s[3] = {0.2f, 0.4f, 0.6f};
        const float y = 0.2126f * 0.2f + 0.7152f * 0.4f + 0.0722f * 0.6f;
        applyPrimaries(primaries({{gk::Saturation, 0.0}}), s);
        KK_COMPARE_RGB(s, y, y, y, 1e-5);

        // Hue 120 degrees turns red into green (rotation about the grey axis).
        float h[3] = {1.0f, 0.0f, 0.0f};
        applyPrimaries(primaries({{gk::Hue, 120.0}}), h);
        KK_COMPARE_RGB(h, 0.0, 1.0, 0.0, 1e-5);

        // Temperature +1 on grey: red x 1.25, blue x 0.75; tint +1: green x 0.75.
        float t[3] = {0.5f, 0.5f, 0.5f};
        applyPrimaries(primaries({{gk::Temperature, 1.0}, {gk::Tint, 1.0}}), t);
        KK_COMPARE_RGB(t, 0.625, 0.375, 0.375, 1e-5);

        // Master gain with lum mix 1 (default) adds the luma change to every channel (keeps the
        // colour differences); with lum mix 0 it scales each channel.
        float m1[3] = {0.2f, 0.4f, 0.6f};
        applyPrimaries(primaries({{gk::GainY, 2.0}}), m1);
        KK_COMPARE_RGB(m1, 0.2 + y, 0.4 + y, 0.6 + y, 1e-5);
        float m0[3] = {0.2f, 0.4f, 0.6f};
        applyPrimaries(primaries({{gk::GainY, 2.0}, {gk::LumMix, 0.0}}), m0);
        KK_COMPARE_RGB(m0, 0.4, 0.8, 1.2, 1e-5);

        // Per-channel gain only touches its channel.
        float g[3] = {0.5f, 0.5f, 0.5f};
        applyPrimaries(primaries({{gk::GainB, 0.5}}), g);
        KK_COMPARE_RGB(g, 0.5, 0.5, 0.25, 1e-5);
    }

    void keyframedPrimaries()
    {
        GradeNode n;
        n.params.values[gk::GainG].setKey(0, 1.0);
        n.params.values[gk::GainG].setKey(10, 2.0);
        QCOMPARE(PrimaryValues::at(n, 0).gain[1], 1.0f);
        QCOMPARE(PrimaryValues::at(n, 10).gain[1], 2.0f);
        QVERIFY(PrimaryValues::at(n, 5).gain[1] > 1.0f);
        QVERIFY(!go::isNodeIdentity(n, 5));
        QVERIFY(go::isNodeIdentity(n, 0));
    }

    // ---- Curves ----

    void customCurves()
    {
        QCOMPARE(evaluateCurve({}, GradeCurve::Luma, 0.3), 0.3);
        QCOMPARE(evaluateCurve({}, GradeCurve::HueSat, 0.3), 0.5);
        const CurvePoints s = {{0.5, 0.75}};
        QVERIFY(near(evaluateCurve(s, GradeCurve::Luma, 0.5), 0.75, 1e-9));
        QVERIFY(near(evaluateCurve(s, GradeCurve::Luma, 0.0), 0.0, 1e-9)); // neutral ends
        QVERIFY(near(evaluateCurve(s, GradeCurve::Luma, 1.0), 1.0, 1e-9));
        // Monotone: no overshoot between points.
        double prev = -1;
        for (int i = 0; i <= 100; ++i) {
            const double v = evaluateCurve(s, GradeCurve::Luma, i / 100.0);
            QVERIFY(v >= prev - 1e-12);
            QVERIFY(v >= 0.0 && v <= 1.0);
            prev = v;
        }
        // Hue curves are periodic.
        const CurvePoints hue = {{0.1, 0.8}, {0.6, 0.2}};
        QVERIFY(near(evaluateCurve(hue, GradeCurve::HueHue, 0.0),
            evaluateCurve(hue, GradeCurve::HueHue, 1.0), 1e-9));
        QVERIFY(near(evaluateCurve(hue, GradeCurve::HueHue, 1.1), 0.8, 1e-9));

        // The baked table interpolates the curve.
        const CurveTables t = tablesWith(GradeCurve::Red, s);
        QVERIFY(!t.allIdentity);
        QCOMPARE(t.mask(), 1 << int(GradeCurve::Red));
        QVERIFY(near(t.sample(GradeCurve::Red, 0.5f), 0.75, 1e-3));
        float c[3] = {0.5f, 0.5f, 0.5f};
        applyCurves(t, c);
        KK_COMPARE_RGB(c, 0.75, 0.5, 0.5, 1e-3);

        // The Y curve applies to every channel.
        float y[3] = {0.5f, 0.5f, 0.5f};
        applyCurves(tablesWith(GradeCurve::Luma, s), y);
        KK_COMPARE_RGB(y, 0.75, 0.75, 0.75, 1e-3);
    }

    void addCurvePointAnchorsNeighbours()
    {
        CurvePoints p;
        const int i = addCurvePoint(p, GradeCurve::HueSat, 0.5, 0.9);
        QCOMPARE(p.size(), 3);
        QCOMPARE(p[i].y, 0.9);
        QVERIFY(near(evaluateCurve(p, GradeCurve::HueSat, 0.1), 0.5, 1e-9)); // far away: neutral
        CurvePoints lum;
        addCurvePoint(lum, GradeCurve::Luma, 0.4, 0.6);
        QCOMPARE(lum.size(), 1); // the Y curve gets no extra points
        addCurvePoint(lum, GradeCurve::Luma, 0.4, 0.7); // same x replaces
        QCOMPARE(lum.size(), 1);
        QCOMPARE(lum[0].y, 0.7);
    }

    void hslCurves()
    {
        // Hue vs Sat flat at 0: everything grey (lightness kept).
        float red[3] = {1.0f, 0.0f, 0.0f};
        applyCurves(tablesWith(GradeCurve::HueSat, {{0.5, 0.0}}), red);
        KK_COMPARE_RGB(red, 0.5, 0.5, 0.5, 1e-4);
        // Sat vs Sat flat at 0 likewise.
        float blue[3] = {0.0f, 0.0f, 1.0f};
        applyCurves(tablesWith(GradeCurve::SatSat, {{0.0, 0.0}, {1.0, 0.0}}), blue);
        KK_COMPARE_RGB(blue, 0.5, 0.5, 0.5, 1e-4);
        // Hue vs Hue: +1/3 turn (y = 0.5 + 1/3) turns red into green.
        float r2[3] = {1.0f, 0.0f, 0.0f};
        applyCurves(tablesWith(GradeCurve::HueHue, {{0.5, 0.5 + 1.0 / 3.0}}), r2);
        KK_COMPARE_RGB(r2, 0.0, 1.0, 0.0, 1e-3);
        // Hue vs Lum flat at 1 (factor 2): the luma doubles, colour differences stay.
        float g[3] = {0.3f, 0.2f, 0.1f};
        const float y = luma(g);
        applyCurves(tablesWith(GradeCurve::HueLum, {{0.5, 1.0}}), g);
        KK_COMPARE_RGB(g, 0.3 + y, 0.2 + y, 0.1 + y, 1e-3);
        // Lum vs Sat at 0 for dark colours only: a dark colour greys, a bright one keeps.
        const CurvePoints darkGrey = {{0.0, 0.0}, {0.3, 0.0}, {0.6, 0.5}};
        float dark[3] = {0.2f, 0.0f, 0.0f};
        applyCurves(tablesWith(GradeCurve::LumSat, darkGrey), dark);
        QVERIFY(near(dark[0], dark[1], 1e-4));
        float bright[3] = {1.0f, 1.0f, 0.6f};
        applyCurves(tablesWith(GradeCurve::LumSat, darkGrey), bright);
        KK_COMPARE_RGB(bright, 1.0, 1.0, 0.6, 1e-3);
    }

    void hslRoundTrip()
    {
        for (const auto& c : {std::array {0.9f, 0.1f, 0.3f}, std::array {0.2f, 0.8f, 0.5f},
                 std::array {0.4f, 0.4f, 0.4f}, std::array {0.0f, 0.3f, 1.0f}}) {
            float hsl[3], back[3];
            rgbToHsl(c.data(), hsl);
            hslToRgb(hsl, back);
            KK_COMPARE_RGB(back, c[0], c[1], c[2], 1e-5);
        }
        float hsl[3];
        const float green[3] = {0, 1, 0};
        rgbToHsl(green, hsl);
        QVERIFY(near(hsl[0], 1.0 / 3.0, 1e-6));
        QVERIFY(near(hsl[1], 1.0) && near(hsl[2], 0.5));
    }

    // ---- Qualifier and windows ----

    void qualifierKeys()
    {
        GradeQualifier q;
        const float red[3] = {1, 0, 0}, green[3] = {0, 1, 0}, grey[3] = {0.5f, 0.5f, 0.5f};
        QCOMPARE(qualifierKey(q, green), 1.0f); // disabled = everything
        q.enabled = true;
        q.hueCenter = 0.0;
        QCOMPARE(qualifierKey(q, red), 1.0f);
        QCOMPARE(qualifierKey(q, green), 0.0f);
        QCOMPARE(qualifierKey(q, grey), 0.0f); // saturation 0 < satLow
        q.invert = true;
        QCOMPARE(qualifierKey(q, red), 0.0f);
        QCOMPARE(qualifierKey(q, green), 1.0f);
        q.invert = false;
        // Soft edge: half way through the softness ramp the key is 0.5.
        q.hueWidth = 0.1;
        q.hueSoft = 0.1;
        float hsl[3] = {0.1f, 1.0f, 0.5f}; // 0.05 (half width) + 0.05 (half the softness)
        float rgb[3];
        hslToRgb(hsl, rgb);
        QVERIFY(near(qualifierKey(q, rgb), 0.5, 1e-3));
        // Only lightness.
        q.useHue = q.useSat = false;
        q.lumLow = 0.6;
        q.lumSoft = 0.0;
        QCOMPARE(qualifierKey(q, grey), 0.0f);
        const float light[3] = {0.8f, 0.8f, 0.8f};
        QCOMPARE(qualifierKey(q, light), 1.0f);
    }

    void windowShapes()
    {
        PictureGeometry g;
        g.width = 200;
        g.height = 100;
        GradeNode n;
        ClipGrade grade;
        grade.nodes.append(n);
        grade.nodes[0].id = 1;
        grade.output = 1;
        const quint64 id = go::addWindow(grade, 1, GradeWindowShape::Circle);
        GradeWindow* w = grade.window(id);
        w->params.setBase(wk::SoftIn, 0.0);
        w->params.setBase(wk::SoftOut, 0.0);
        auto matte = [&](double x, double y) {
            return windowsMatte(evaluateWindows(*grade.node(1), 0, g), float(x), float(y));
        };
        // Circle: 100 x 50 px ellipse around the centre (100, 50).
        QCOMPARE(matte(100, 50), 1.0f);
        QCOMPARE(matte(5, 5), 0.0f);
        QVERIFY(near(matte(150, 50), 0.5, 1e-3)); // on the edge (half-pixel antialiasing)
        QCOMPARE(matte(140, 50), 1.0f);
        QCOMPARE(matte(160, 50), 0.0f);
        // Soft edges: inside and outside softness of 10 px (0.1 picture heights).
        w->params.setBase(wk::SoftIn, 0.1);
        w->params.setBase(wk::SoftOut, 0.1);
        QVERIFY(near(matte(150, 50), 0.5, 1e-3));
        const float inside = matte(145, 50), outside = matte(155, 50);
        QVERIFY(inside > 0.5f && inside < 1.0f);
        QVERIFY(outside > 0.0f && outside < 0.5f);
        QVERIFY(near(inside + outside, 1.0, 1e-3)); // symmetric ramp
        // Invert and opacity.
        w->invert = true;
        QCOMPARE(matte(100, 50), 0.0f);
        QCOMPARE(matte(5, 5), 1.0f);
        w->invert = false;
        w->params.setBase(wk::Opacity, 0.5);
        QCOMPARE(matte(100, 50), 0.5f);
        w->params.setBase(wk::Opacity, 1.0);
        w->params.setBase(wk::SoftIn, 0.0);
        w->params.setBase(wk::SoftOut, 0.0);

        // Linear (rectangle) rotated by 90 degrees: 100 x 50 becomes 50 wide, 100 tall... in
        // window units (fractions of the picture): width 0.5 = 100 px, height 0.5 = 50 px.
        w->shape = GradeWindowShape::Linear;
        QCOMPARE(matte(145, 70), 1.0f);
        QCOMPARE(matte(100, 80), 0.0f);
        w->params.setBase(wk::Rotation, 90.0);
        QCOMPARE(matte(100, 70), 1.0f); // now 50 px wide and 100 px tall
        QCOMPARE(matte(145, 50), 0.0f);
        w->params.setBase(wk::Rotation, 0.0);

        // Polygon: the square of the window box equals the rectangle.
        w->shape = GradeWindowShape::Polygon;
        w->points = {{QPointF(-0.5, -0.5)}, {QPointF(0.5, -0.5)}, {QPointF(0.5, 0.5)},
            {QPointF(-0.5, 0.5)}};
        QCOMPARE(matte(145, 70), 1.0f);
        QCOMPARE(matte(100, 80), 0.0f);
        QVERIFY(near(matte(150, 50), 0.5, 1e-3));
        // Triangle: the point under the tip is inside, the corners of the box are not.
        w->points = {{QPointF(0, -0.5)}, {QPointF(0.5, 0.5)}, {QPointF(-0.5, 0.5)}};
        QCOMPARE(matte(100, 60), 1.0f);
        QCOMPARE(matte(55, 30), 0.0f);

        // Gradient: the upper half (before rotation) selected.
        w->shape = GradeWindowShape::Gradient;
        QCOMPARE(matte(10, 10), 1.0f);
        QCOMPARE(matte(10, 90), 0.0f);
        QVERIFY(near(matte(10, 50), 0.5, 1e-3));
        w->params.setBase(wk::Rotation, 180.0);
        QCOMPARE(matte(10, 10), 0.0f);

        // Two windows combine as a union.
        w->shape = GradeWindowShape::Circle;
        w->params.setBase(wk::Rotation, 0.0);
        w->params.setBase(wk::Width, 0.1);
        w->params.setBase(wk::Height, 0.1);
        w->params.setBase(wk::CenterX, 0.1);
        const quint64 second = go::addWindow(grade, 1, GradeWindowShape::Circle);
        grade.window(second)->params.setBase(wk::Width, 0.1);
        grade.window(second)->params.setBase(wk::Height, 0.1);
        grade.window(second)->params.setBase(wk::CenterX, 0.9);
        QCOMPARE(matte(20, 50), 1.0f);
        QCOMPARE(matte(180, 50), 1.0f);
        QCOMPARE(matte(100, 50), 0.0f);
        // A disabled window does not count; a polygon needs three points.
        grade.window(second)->enabled = false;
        QCOMPARE(matte(180, 50), 0.0f);
        GradeWindow line;
        line.shape = GradeWindowShape::Polygon;
        line.points = {{QPointF(0, 0)}, {QPointF(1, 1)}};
        QVERIFY(!line.isRenderable());
    }

    // ---- Curve windows (spec 111) ----

    void curveWindowRoundTripAndOldFiles()
    {
        ClipGrade g;
        GradeNode n;
        n.id = 1;
        g.nodes.append(n);
        g.output = 1;
        const quint64 id = go::addWindow(g, 1, GradeWindowShape::Curve);
        QCOMPARE(g.window(id)->points.size(), 4);
        QVERIFY(g.window(id)->isRenderable());
        GradeWindowShapePoints moved = g.window(id)->points;
        moved[1].outHandle = QPointF(0.05, 0.3);
        moved[1].smooth = false;
        g.window(id)->shapeKeys.insert(0, g.window(id)->points);
        g.window(id)->shapeKeys.insert(12, moved);
        QVERIFY(g.isAnimated());
        const ClipGrade back = serialization::gradeFromJson(serialization::gradeToJson(g));
        QCOMPARE(back, g);
        QCOMPARE(back.window(id)->shape, GradeWindowShape::Curve);
        QCOMPARE(back.window(id)->shapeKeys.size(), 2);

        // A file of format 16 - 20 stores polygon points as [x, y]: they load as corners.
        QJsonObject window {{QLatin1String("id"), 5}, {QLatin1String("shape"), QLatin1String("polygon")},
            {QLatin1String("points"),
                QJsonArray {QJsonArray {0.0, -0.5}, QJsonArray {0.5, 0.5}, QJsonArray {-0.5, 0.5}}}};
        QJsonObject node {{QLatin1String("id"), 1}, {QLatin1String("windows"), QJsonArray {window}}};
        QJsonObject root {{QLatin1String("nodes"), QJsonArray {node}}, {QLatin1String("output"), 1}};
        const ClipGrade old = serialization::gradeFromJson(root);
        const GradeWindow* w = old.window(5);
        QVERIFY(w);
        QCOMPARE(w->points.size(), 3);
        QCOMPARE(w->points[1].pos, QPointF(0.5, 0.5));
        QVERIFY(!w->points[1].hasHandles() && !w->points[1].smooth);

        // Counts are clamped; a key with another point count is dropped.
        QJsonArray many;
        for (int i = 0; i < kMaxWindowPoints + 10; ++i)
            many.append(QJsonArray {0.1 * i, 0.0});
        window[QLatin1String("points")] = many;
        window[QLatin1String("shapeKeys")] = QJsonArray {QJsonObject {{QLatin1String("frame"), 3},
            {QLatin1String("points"), QJsonArray {QJsonArray {0.0, 0.0}}}}};
        node[QLatin1String("windows")] = QJsonArray {window};
        root[QLatin1String("nodes")] = QJsonArray {node};
        const GradeWindow* clamped = serialization::gradeFromJson(root).window(5);
        QCOMPARE(clamped->points.size(), kMaxWindowPoints);
        QVERIFY(clamped->shapeKeys.isEmpty());
    }

    void shapeKeysInterpolate()
    {
        GradeWindow w;
        w.shape = GradeWindowShape::Polygon;
        w.points = {{QPointF(0, 0)}, {QPointF(1, 0)}, {QPointF(0, 1)}};
        QCOMPARE(w.shapeAt(5), w.points); // no keys: the static shape
        GradeWindowShapePoints a = w.points, b = w.points;
        for (GradeWindowPoint& p : b)
            p.pos += QPointF(1, 2);
        b[0].outHandle = QPointF(0.4, 0.0);
        w.shapeKeys.insert(10, a);
        w.shapeKeys.insert(20, b);
        QCOMPARE(w.shapeAt(0), a); // held before the first key
        QCOMPARE(w.shapeAt(10), a); // exact on keys
        QCOMPARE(w.shapeAt(20), b);
        QCOMPARE(w.shapeAt(99), b); // held after the last
        const GradeWindowShapePoints mid = w.shapeAt(15);
        QCOMPARE(mid[1].pos, QPointF(1.5, 1.0));
        QCOMPARE(mid[0].outHandle, QPointF(0.2, 0.0));
        // Keys move with the clip (trim / split).
        GradeNode n;
        n.windows.append(w);
        n.shiftKeys(-4);
        QVERIFY(n.windows[0].shapeKeys.contains(6) && n.windows[0].shapeKeys.contains(16));
    }

    void insertPointKeepsTheCurve()
    {
        GradeWindowShapePoints pts = {{QPointF(0, 0), QPointF(), QPointF(0.3, 0.3), true},
            {QPointF(1, 0), QPointF(0.0, -0.4), QPointF(), true}, {QPointF(0.5, 1)}};
        const QPointF before = windowSpanPoint(pts, 0, 0.25);
        QVERIFY(insertWindowPoint(pts, 0, 0.5));
        QCOMPARE(pts.size(), 4);
        // The first half of the new two spans traces the old span's first half.
        const QPointF after = windowSpanPoint(pts, 0, 0.5);
        QVERIFY(near((before - after).x(), 0.0, 1e-9) && near((before - after).y(), 0.0, 1e-9));
        // A polygon edge gets a point on the edge.
        GradeWindowShapePoints poly = {{QPointF(0, 0)}, {QPointF(2, 0)}, {QPointF(0, 2)}};
        QVERIFY(insertWindowPoint(poly, 0, 0.25));
        QCOMPARE(poly[1].pos, QPointF(0.5, 0));
        // Smoothing: automatic handles along the neighbours.
        const GradeWindowPoint s = smoothedPoint(poly, 1);
        QVERIFY(s.smooth);
        QCOMPARE(s.outHandle, -s.inHandle);
    }

    void curveWindowFlattening()
    {
        PictureGeometry g;
        g.width = 200;
        g.height = 200;
        ClipGrade grade;
        GradeNode n;
        n.id = 1;
        grade.nodes.append(n);
        grade.output = 1;
        const quint64 id = go::addWindow(grade, 1, GradeWindowShape::Curve);
        GradeWindow* w = grade.window(id);
        w->params.setBase(wk::SoftIn, 0.0);
        w->params.setBase(wk::SoftOut, 0.0);
        // The four-point rounded default (width 0.5 of 200 px) stays within half a pixel of
        // the true circle of radius 50 px.
        const auto values = evaluateWindows(*grade.node(1), 0, g);
        QCOMPARE(values.size(), size_t(1));
        const std::vector<float>& flat = values[0].points;
        QVERIFY(flat.size() / 2 > 8);
        QVERIFY(flat.size() / 2 <= size_t(kMaxWindowOutlinePoints));
        for (size_t i = 0; i < flat.size(); i += 2) {
            const double r = std::hypot(double(flat[i]), double(flat[i + 1]));
            QVERIFY2(std::abs(r - 50.0) < 0.5, qPrintable(QString::number(r)));
        }
        // Zero-length handles give exactly the polygon of the same points.
        GradeWindowShapePoints corners = w->points;
        for (GradeWindowPoint& p : corners)
            p.inHandle = p.outHandle = QPointF();
        w->points = corners;
        auto curveMatte = [&](double x, double y) {
            return windowsMatte(evaluateWindows(*grade.node(1), 0, g), float(x), float(y));
        };
        std::vector<float> curveSamples;
        for (int y = 5; y < 200; y += 17)
            for (int x = 3; x < 200; x += 13)
                curveSamples.push_back(curveMatte(x, y));
        w->shape = GradeWindowShape::Polygon;
        size_t k = 0;
        for (int y = 5; y < 200; y += 17)
            for (int x = 3; x < 200; x += 13)
                QCOMPARE(curveMatte(x, y), curveSamples[k++]);
        // The soft edge of a curve equals the polygon's ramp, and a tracker moves it.
        w->shape = GradeWindowShape::Curve;
        w->params.setBase(wk::SoftOut, 0.1);
        const float edge = curveMatte(150, 100); // on the outline: half
        QVERIFY(edge > 0.0f && edge < 1.0f);
        grade.node(1)->track.samples.append({0.1, 0.0, 1.0, 1.0, 0.0});
        grade.node(1)->track.pan = true;
        grade.node(1)->track.tilt = true;
        QVERIFY(curveMatte(150, 100) != edge); // the track moved the whole curve
    }

    void layerFormula()
    {
        // layerOver() equals mix(result, graded, alpha) for o = mix(in, graded, alpha).
        const float in[3] = {0.2f, 0.4f, 0.6f};
        const float graded[3] = {0.8f, 0.1f, 0.3f};
        const float alpha = 0.3f;
        float o[3];
        for (int i = 0; i < 3; ++i)
            o[i] = in[i] + (graded[i] - in[i]) * alpha;
        float r[3] = {0.5f, 0.5f, 0.5f};
        layerOver(r, in, o, alpha);
        KK_COMPARE_RGB(
            r, 0.5 + (0.8 - 0.5) * 0.3, 0.5 + (0.1 - 0.5) * 0.3, 0.5 + (0.3 - 0.5) * 0.3, 1e-6);
    }

    // ---- Graph operations ----

    void serialChain()
    {
        ClipGrade g;
        QVERIFY(go::isIdentity(g, 0));
        QCOMPARE(go::defaultSelection(g), quint64(0));
        const quint64 a = go::addSerialNode(g, 0); // an empty grade gets its first node
        QCOMPARE(g.nodes.size(), 1);
        QCOMPARE(g.output, a);
        QVERIFY(go::isIdentity(g, 0));
        const quint64 b = go::addSerialNode(g, a);
        const quint64 c = go::addSerialNode(g, b);
        QCOMPARE(g.output, c);
        QCOMPARE(g.node(c)->inputs, QVector<quint64> {b});
        QCOMPARE(g.node(b)->inputs, QVector<quint64> {a});
        // Inserting after a keeps the order: a -> new -> b -> c.
        const quint64 n = go::addSerialNode(g, a);
        QCOMPARE(g.node(b)->inputs, QVector<quint64> {n});
        QCOMPARE(g.node(n)->inputs, QVector<quint64> {a});
        const std::vector<int> order = go::evaluationOrder(g);
        QCOMPARE(order.size(), size_t(4));
        QCOMPARE(g.nodes[order[0]].id, a);
        QCOMPARE(g.nodes[order[1]].id, n);
        QCOMPARE(g.nodes[order[3]].id, c);
        // Before b: a -> n -> new -> b.
        const quint64 before = go::addSerialNodeBefore(g, b);
        QCOMPARE(g.node(b)->inputs, QVector<quint64> {before});
        QCOMPARE(g.node(before)->inputs, QVector<quint64> {n});
        // Remove keeps the chain connected.
        QVERIFY(go::removeNode(g, before));
        QCOMPARE(g.node(b)->inputs, QVector<quint64> {n});
        QVERIFY(go::removeNode(g, c));
        QCOMPARE(g.output, b);
        QVERIFY(!go::removeNode(g, 999));
        // Bypass and reset.
        g.node(b)->params.setBase(gk::GainR, 2.0);
        QVERIFY(!go::isIdentity(g, 0));
        QVERIFY(go::setNodeEnabled(g, b, false));
        QVERIFY(!go::setNodeEnabled(g, b, false));
        QVERIFY(go::isIdentity(g, 0));
        QVERIFY(go::setNodeEnabled(g, b, true));
        g.node(b)->params.setBase(gk::Mix, 0.0);
        QVERIFY(go::isIdentity(g, 0));
        QVERIFY(go::resetNode(g, b));
        QVERIFY(g.node(b)->params == ParamSet {});
        QVERIFY(g.node(b)->enabled);
    }

    void parallelAndLayerMixers()
    {
        ClipGrade g;
        const quint64 a = go::initialize(g);
        const quint64 p = go::addParallelNode(g, a);
        QCOMPARE(g.nodes.size(), 3);
        const GradeNode* mixer = g.node(g.output);
        QVERIFY(mixer && mixer->type == GradeNodeType::Parallel);
        QCOMPARE(mixer->inputs, (QVector<quint64> {a, p}));
        QCOMPARE(g.node(p)->inputs, QVector<quint64> {0}); // fed from a's input
        // A second parallel node joins the same mixer.
        const quint64 p2 = go::addParallelNode(g, a);
        QCOMPARE(g.node(g.output)->inputs.size(), 3);
        QCOMPARE(g.node(g.output)->inputs.last(), p2);
        QVERIFY(go::isIdentity(g, 0)); // mixers of neutral nodes: still identity
        g.node(p)->params.setBase(gk::Saturation, 0.5);
        QVERIFY(!go::isIdentity(g, 0));
        g.node(p)->params.setBase(gk::Saturation, 1.0);
        // Removing the extra inputs dissolves the mixer.
        QVERIFY(go::removeNode(g, p2));
        QVERIFY(go::removeNode(g, p));
        QCOMPARE(g.nodes.size(), 1);
        QCOMPARE(g.output, a);
        // Layer mixer: the new node is the top (last) input.
        const quint64 l = go::addLayerNode(g, a);
        QCOMPARE(g.node(g.output)->type, GradeNodeType::Layer);
        QCOMPARE(g.node(g.output)->inputs, (QVector<quint64> {a, l}));
        // Connect refuses cycles and unknown targets; disconnect keeps one mixer input.
        QVERIFY(!go::connect(g, g.output, a, 0));
        QVERIFY(!go::connect(g, a, 999, 0));
        QVERIFY(go::disconnect(g, g.output, 1));
        QVERIFY(!go::disconnect(g, g.output, 0));
        QVERIFY(go::connect(g, l, g.output, 1));
        QVERIFY(go::reaches(g, g.output, l));
        // A disconnected output passes the picture through.
        QVERIFY(go::disconnect(g, 0, 0));
        QVERIFY(go::evaluationOrder(g).empty());
        QVERIFY(go::isIdentity(g, 0));
    }

    void windowsShareTheIdSpace()
    {
        ClipGrade g;
        const quint64 a = go::initialize(g);
        const quint64 w = go::addWindow(g, a, GradeWindowShape::Circle);
        QVERIFY(w > a);
        const quint64 b = go::addSerialNode(g, a);
        QVERIFY(b > w);
        const GradeNode* owner = nullptr;
        QVERIFY(g.window(w, &owner));
        QCOMPARE(owner->id, a);
        QVERIFY(g.node(a)->hasKey());
        for (int i = 1; i < kMaxGradeWindows; ++i)
            QVERIFY(go::addWindow(g, a, GradeWindowShape::Linear));
        QCOMPARE(go::addWindow(g, a, GradeWindowShape::Linear), quint64(0)); // full
        QVERIFY(go::removeWindow(g, w));
        QVERIFY(!go::removeWindow(g, w));
        const quint64 mixer = g.node(g.output)->id;
        Q_UNUSED(mixer);
        QCOMPARE(go::addWindow(g, 999, GradeWindowShape::Circle), quint64(0));
    }

    // ---- Keyframes and edits ----

    void keyframePaths()
    {
        ClipData clip = mediaClip(0, 100);
        clip.grade = richGrade();
        const quint64 first = clip.grade.nodes[0].id;
        const AnimatedValue* v
            = keyframeops::findValue(clip, keyframeops::gradePath(first, gk::GainR));
        QVERIFY(v && v->keys.size() == 2);
        quint64 window = 0;
        for (const GradeNode& n : clip.grade.nodes) {
            if (!n.windows.isEmpty())
                window = n.windows[0].id;
        }
        QVERIFY(keyframeops::findValue(clip, keyframeops::gradeWindowPath(window, wk::CenterX)));
        QVERIFY(!keyframeops::findValue(clip, keyframeops::gradePath(999, gk::GainR)));
        // The keyframe editors list the grade groups.
        const QVector<keyframeops::AnimatedParam> params = keyframeops::animatedParams(clip);
        int grade = 0;
        for (const auto& p : params) {
            if (p.path.startsWith(QLatin1String("grade")))
                ++grade;
        }
        QCOMPARE(grade, 2);
        QVERIFY(keyframeops::allKeyFrames(clip).contains(24));
        QVERIFY(keyframeops::allKeyFrames(clip).contains(10));
    }

    void splitShiftsGradeKeys()
    {
        TimelineData d = timeline();
        ClipData clip = mediaClip(10, 100);
        clip.grade = richGrade();
        const quint64 id = editops::overwriteClip(d, kV1, clip);
        QVERIFY(id);
        QVERIFY(!editops::splitAt(d, 30, editops::unlockedTracks()).isEmpty());
        const TrackData& t = *d.track(kV1);
        QCOMPARE(t.clips.size(), size_t(2));
        const quint64 first = t.clips[1].grade.nodes[0].id;
        const AnimatedValue& v = t.clips[1].grade.node(first)->params.values[gk::GainR];
        QCOMPARE(v.keys.first().frame, int64_t(-20));
        QCOMPARE(
            t.clips[0].grade.node(first)->params.values[gk::GainR].keys.first().frame, int64_t(0));
        // Both halves keep the same grade, so they can be joined again.
        QVERIFY(editops::canJoin(d, t.clips[0].id, t.clips[1].id));
        TimelineData other = d;
        other.track(kV1)->clips[1].grade.nodes[0].params.setBase(gk::Hue, 10.0);
        QVERIFY(!editops::canJoin(other, t.clips[0].id, t.clips[1].id));
    }

    // ---- Blur palette ----

    void blurValuesMath()
    {
        GradeNode n;
        // Defaults: nothing to do.
        QVERIFY(!BlurValues::at(n, 0, 1080).active());
        QVERIFY(go::isNodeIdentity(n, 0));
        // Radius above 0.5 blurs, sigma grows with the radius and the picture height.
        n.params.setBase(gk::BlurRadiusR, 0.75);
        n.params.setBase(gk::BlurRadiusG, 0.75);
        n.params.setBase(gk::BlurRadiusB, 0.75);
        BlurValues v = BlurValues::at(n, 0, 1080);
        QVERIFY(v.active() && v.stages[0].active() && !v.stages[1].active());
        QVERIFY(!go::isNodeIdentity(n, 0));
        QCOMPARE(v.stages[0].sigmaH[0], 0.5f * kBlurMaxSigma * 1080.0f);
        QCOMPARE(v.stages[0].mix[0], 1.0f);
        QVERIFY(v.stages[0].uniformSigma());
        QCOMPARE(BlurValues::at(n, 0, 2160).stages[0].sigmaH[0], 2.0f * v.stages[0].sigmaH[0]);
        // Radius below 0.5 sharpens in Blur mode (an unsharp mask).
        n.params.setBase(gk::BlurRadiusR, 0.0);
        v = BlurValues::at(n, 0, 1080);
        QCOMPARE(v.stages[0].mix[0], 0.0f);
        QVERIFY(v.stages[0].sharp[0] > 0.0f);
        QVERIFY(!v.stages[0].uniformSigma());
        // H/V ratio 0 blurs only horizontally; 1 only vertically.
        n.params.setBase(gk::BlurRadiusR, 1.0);
        n.params.setBase(gk::BlurRatio, 0.0);
        v = BlurValues::at(n, 0, 1080);
        QVERIFY(v.stages[0].sigmaH[1] > 0.0f);
        QCOMPARE(v.stages[0].sigmaV[1], 0.0f);
        n.params.setBase(gk::BlurRatio, 1.0);
        v = BlurValues::at(n, 0, 1080);
        QCOMPARE(v.stages[0].sigmaH[1], 0.0f);
        QVERIFY(v.stages[0].sigmaV[1] > 0.0f);
        // Mist needs its radius; Sharpen sharpens above 0.5.
        GradeNode m;
        m.params.setBase(gk::MistMix, 1.0);
        QVERIFY(!BlurValues::at(m, 0, 1080).active());
        m.params.setBase(gk::MistRadius, 0.5);
        QVERIFY(BlurValues::at(m, 0, 1080).stages[2].active());
        m.params.setBase(gk::MistRadius, 0.0);
        m.params.setBase(gk::SharpRadiusG, 1.0);
        const BlurStage s = BlurValues::at(m, 0, 1080).stages[1];
        QVERIFY(s.sharp[1] > 0.0f && s.sharp[0] == 0.0f);
        // Keyframed radius.
        GradeNode k;
        k.params.values[gk::BlurRadiusR].setKey(0, 0.5);
        k.params.values[gk::BlurRadiusR].setKey(10, 1.0);
        QVERIFY(!BlurValues::at(k, 0, 1080).active());
        QVERIFY(BlurValues::at(k, 10, 1080).active());
    }

    void blurKernelAndCoring()
    {
        QCOMPARE(blurTapRadius(0.0f), 0);
        QCOMPARE(blurTapRadius(2.0f), 6);
        QCOMPARE(blurTapRadius(1e6f), kMaxBlurTaps);
        const std::vector<float> w = blurKernel(2.0f);
        QCOMPARE(int(w.size()), 13);
        float sum = 0.0f;
        for (const float x : w)
            sum += x;
        QVERIFY(std::abs(sum - 1.0f) < 1e-5f);
        QVERIFY(w[6] > w[5] && w[5] > w[4]); // bell shaped
        QCOMPARE(w[5], w[7]);
        QCOMPARE(coringFactor(0.01f, 0.0f), 1.0f);
        QCOMPARE(coringFactor(0.0f, 0.5f), 0.0f);
        QCOMPARE(coringFactor(0.5f, 0.5f), 1.0f);
        // Blur and unsharp combination.
        QCOMPARE(blurCombine(0.8f, 0.4f, 1.0f, 0.0f, 0.0f), 0.4f);
        QVERIFY(std::abs(blurCombine(0.8f, 0.4f, 0.0f, 1.0f, 0.0f) - 1.2f) < 1e-6f);
        QCOMPARE(blurCombine(0.8f, 0.4f, 0.0f, 0.0f, 0.0f), 0.8f);
    }

    void blurSerializationAndReset()
    {
        ClipGrade g;
        const quint64 a = go::initialize(g);
        g.node(a)->params.setBase(gk::BlurRadiusG, 0.8);
        g.node(a)->params.setBase(gk::SharpCoring, 0.3);
        g.node(a)->params.setBase(gk::MistRadius, 0.4);
        g.node(a)->params.values[gk::BlurScaling].setKey(0, 0.2);
        g.node(a)->params.values[gk::BlurScaling].setKey(5, 0.9);
        QCOMPARE(serialization::gradeFromJson(serialization::gradeToJson(g)), g);
        // A project saved before the palette has no blur keys and still reads as no blur.
        ClipGrade old;
        go::initialize(old);
        const ClipGrade back = serialization::gradeFromJson(serialization::gradeToJson(old));
        QVERIFY(!BlurValues::at(back.nodes[0], 0, 1080).active());
        QVERIFY(go::isNodeIdentity(back.nodes[0], 0));
        // Reset node clears the blur.
        QVERIFY(go::resetNode(g, a));
        QVERIFY(!BlurValues::at(*g.node(a), 0, 1080).active());
        // Every blur key is listed for keyframing and has a label and range.
        for (const QString& key : gk::blurKeys()) {
            QVERIFY(gk::allKeys().contains(key));
            QVERIFY(gk::label(key) != key);
            QCOMPARE(gk::range(key), (std::pair<double, double> {0.0, 1.0}));
        }
    }

    // ---- RGB Mixer palette ----

    void rgbMixerMath()
    {
        GradeNode n;
        MixerValues v = MixerValues::at(n, 0);
        QVERIFY(v.isIdentity() && !v.mono && !v.preserveLum);
        QVERIFY(go::isNodeIdentity(n, 0));
        float c[3] = {0.2f, 0.5f, 0.9f};
        applyMixer(v, c);
        QCOMPARE(c[0], 0.2f);
        QCOMPARE(c[2], 0.9f);
        // Swapping the red and blue rows swaps the channels.
        n.params.setBase(gk::MixerRR, 0.0);
        n.params.setBase(gk::MixerRB, 1.0);
        n.params.setBase(gk::MixerBB, 0.0);
        n.params.setBase(gk::MixerBR, 1.0);
        v = MixerValues::at(n, 0);
        QVERIFY(!v.isIdentity());
        QVERIFY(!go::isNodeIdentity(n, 0));
        applyMixer(v, c);
        QCOMPARE(c[0], 0.9f);
        QCOMPARE(c[1], 0.5f);
        QCOMPARE(c[2], 0.2f);
        // Negative weights are allowed and not clamped in matrix mode.
        GradeNode neg;
        neg.params.setBase(gk::MixerRG, -1.0);
        float d[3] = {0.5f, 0.5f, 0.5f};
        applyMixer(MixerValues::at(neg, 0), d);
        QCOMPARE(d[0], 0.0f);
        neg.params.setBase(gk::MixerRG, -2.0);
        applyMixer(MixerValues::at(neg, 0), d);
        QVERIFY(d[0] < 0.0f);
    }

    void rgbMixerMonochromeAndPreserve()
    {
        // Monochrome with the default weights is Rec. 709 luma on the primaries (0.3 / 0.59 /
        // 0.11 are the Rec. 601-style defaults, so compare with its own weights).
        GradeNode n;
        n.params.setBase(gk::MixerMono, 1.0);
        const MixerValues v = MixerValues::at(n, 0);
        QVERIFY(v.mono && !v.isIdentity());
        float r[3] = {1, 0, 0}, g[3] = {0, 1, 0}, b[3] = {0, 0, 1};
        applyMixer(v, r);
        applyMixer(v, g);
        applyMixer(v, b);
        QVERIFY(std::abs(r[0] - 0.3f) < 1e-6f && r[1] == r[0] && r[2] == r[0]);
        QVERIFY(std::abs(g[0] - 0.59f) < 1e-6f);
        QVERIFY(std::abs(b[2] - 0.11f) < 1e-6f);
        // Rec. 709 weights typed in give Rec. 709 luma.
        n.params.setBase(gk::MixerMonoR, double(kLumaR));
        n.params.setBase(gk::MixerMonoG, double(kLumaG));
        n.params.setBase(gk::MixerMonoB, double(kLumaB));
        float c[3] = {0.3f, 0.6f, 0.2f};
        const float y = luma(c);
        applyMixer(MixerValues::at(n, 0), c);
        QVERIFY(std::abs(c[0] - y) < 1e-6f);
        // A negative result floors at 0 (darktable process_gray).
        n.params.setBase(gk::MixerMonoR, -1.0);
        n.params.setBase(gk::MixerMonoG, 0.0);
        n.params.setBase(gk::MixerMonoB, 0.0);
        float e[3] = {0.5f, 0.5f, 0.5f};
        applyMixer(MixerValues::at(n, 0), e);
        QCOMPARE(e[1], 0.0f);
        // Preserve Luminance in Monochrome: weights act as shares (divided by their sum).
        n.params.setBase(gk::MixerPreserve, 1.0);
        n.params.setBase(gk::MixerMonoR, 0.6);
        n.params.setBase(gk::MixerMonoG, 0.6);
        n.params.setBase(gk::MixerMonoB, 0.0);
        float w[3] = {1.0f, 1.0f, 1.0f};
        applyMixer(MixerValues::at(n, 0), w);
        QVERIFY(std::abs(w[0] - 1.0f) < 1e-6f);
        // Preserve Luminance in matrix mode keeps luma709 for any matrix.
        GradeNode m;
        const double vals[9] = {0.9, -0.4, 1.2, 0.3, 1.6, -0.2, -0.5, 0.8, 0.7};
        const QString* keys[9] = {&gk::MixerRR, &gk::MixerRG, &gk::MixerRB, &gk::MixerGR,
            &gk::MixerGG, &gk::MixerGB, &gk::MixerBR, &gk::MixerBG, &gk::MixerBB};
        for (int i = 0; i < 9; ++i)
            m.params.setBase(*keys[i], vals[i]);
        m.params.setBase(gk::MixerPreserve, 1.0);
        const MixerValues mv = MixerValues::at(m, 0);
        quint32 seed = 12345;
        auto rnd = [&seed] {
            seed = seed * 1664525u + 1013904223u;
            return float(seed >> 8) / float(1 << 24);
        };
        int checked = 0;
        for (int i = 0; i < 100; ++i) {
            float in[3] = {rnd(), rnd(), rnd()};
            float out[3] = {in[0], in[1], in[2]};
            applyMixer(mv, out);
            // Only where the unscaled mix has positive luma the rule applies.
            float raw[3];
            for (int r = 0; r < 3; ++r)
                raw[r] = mv.m[r * 3] * in[0] + mv.m[r * 3 + 1] * in[1] + mv.m[r * 3 + 2] * in[2];
            if (luma(raw) <= kMixerEpsilon)
                continue;
            ++checked;
            QVERIFY(std::abs(luma(out) - luma(in)) < 1e-4f);
        }
        QVERIFY(checked > 30);
    }

    void rgbMixerKeyframesSerializationAndReset()
    {
        GradeNode k;
        k.params.values[gk::MixerRG].setKey(0, 0.0);
        k.params.values[gk::MixerRG].setKey(10, 1.0);
        QVERIFY(go::isNodeIdentity(k, 0));
        QVERIFY(!go::isNodeIdentity(k, 10));
        QVERIFY(std::abs(MixerValues::at(k, 5).m[1] - 0.5f) < 1e-5f);
        ClipGrade g;
        const quint64 a = go::initialize(g);
        g.node(a)->params.setBase(gk::MixerBR, 0.6);
        g.node(a)->params.setBase(gk::MixerMono, 1.0);
        g.node(a)->params.setBase(gk::MixerPreserve, 1.0);
        g.node(a)->params.values[gk::MixerMonoG].setKey(0, 0.2);
        g.node(a)->params.values[gk::MixerMonoG].setKey(5, 0.9);
        const ClipGrade back = serialization::gradeFromJson(serialization::gradeToJson(g));
        QCOMPARE(back, g);
        QVERIFY(MixerValues::at(back.nodes[0], 0).mono);
        QVERIFY(MixerValues::at(back.nodes[0], 0).preserveLum);
        // A project saved before the palette reads as identity.
        ClipGrade old;
        go::initialize(old);
        QVERIFY(MixerValues::at(old.nodes[0], 0).isIdentity());
        QVERIFY(go::resetNode(g, a));
        QVERIFY(MixerValues::at(*g.node(a), 0).isIdentity());
        for (const QString& key : gk::rgbMixerKeys()) {
            QVERIFY(gk::allKeys().contains(key));
            QVERIFY(gk::label(key) != key);
        }
        QCOMPARE(gk::range(gk::MixerRG), (std::pair<double, double> {-2.0, 2.0}));
    }

    // ---- AgX ----

    void agxKnownValues()
    {
        // Reference numbers from an independent Python port of the minimal AgX implementation
        // (Linear in, sRGB out). Mid grey 0.18 lands on 0.4967, as in Blender's AgX.
        struct Case
        {
            AgxLook look;
            AgxOutput out;
            float in[3];
            float want[3];
        };
        const Case cases[] = {
            {AgxLook::None, AgxOutput::Srgb, {0, 0, 0}, {0, 0, 0}},
            {AgxLook::None, AgxOutput::Srgb, {0.18f, 0.18f, 0.18f}, {0.49668f, 0.49674f, 0.49675f}},
            {AgxLook::None, AgxOutput::Srgb, {1, 1, 1}, {0.78675f, 0.78688f, 0.78689f}},
            {AgxLook::None, AgxOutput::Srgb, {16, 16, 16}, {0.99761f, 0.99782f, 0.99783f}},
            {AgxLook::None, AgxOutput::Srgb, {1, 0, 0}, {0.86098f, 0.23016f, 0.23032f}},
            {AgxLook::None, AgxOutput::Srgb, {0, 1, 0}, {0.31138f, 0.83109f, 0.31146f}},
            {AgxLook::None, AgxOutput::Srgb, {0, 0, 1}, {0.31278f, 0.31274f, 0.83064f}},
            {AgxLook::None, AgxOutput::Srgb, {0.5f, 0.2f, 0.1f}, {0.69378f, 0.52145f, 0.41084f}},
            {AgxLook::Punchy, AgxOutput::Srgb, {0.18f, 0.18f, 0.18f}, {0.38881f, 0.38884f, 0.38884f}},
            {AgxLook::Punchy, AgxOutput::Srgb, {0.5f, 0.2f, 0.1f}, {0.67498f, 0.40441f, 0.24327f}},
            {AgxLook::Golden, AgxOutput::Srgb, {0.18f, 0.18f, 0.18f}, {0.58404f, 0.53794f, 0.34093f}},
            {AgxLook::Golden, AgxOutput::Srgb, {0.5f, 0.2f, 0.1f}, {0.73161f, 0.56106f, 0.30952f}},
            {AgxLook::Greyscale, AgxOutput::Srgb, {0.5f, 0.2f, 0.1f}, {0.54282f, 0.54294f, 0.54294f}},
            {AgxLook::None, AgxOutput::Rec709, {0.18f, 0.18f, 0.18f}, {0.5265f, 0.52657f, 0.52657f}},
            {AgxLook::None, AgxOutput::Rec709, {1, 0, 0}, {0.87178f, 0.26013f, 0.2603f}},
        };
        for (const Case& c : cases) {
            AgxValues v;
            v.on = true;
            v.input = AgxInput::Linear;
            v.look = c.look;
            v.output = c.out;
            float rgb[3] = {c.in[0], c.in[1], c.in[2]};
            applyAgx(v, rgb);
            for (int i = 0; i < 3; ++i)
                QVERIFY2(std::abs(rgb[i] - c.want[i]) < 2e-3f,
                    qPrintable(QStringLiteral("look %1 out %2 in %3 channel %4: %5 vs %6")
                            .arg(int(c.look)).arg(int(c.out)).arg(c.in[0]).arg(i)
                            .arg(rgb[i]).arg(c.want[i])));
        }
        // Exposure: one stop up on 0.09 equals 0.18.
        AgxValues up;
        up.on = true;
        up.input = AgxInput::Linear;
        up.output = AgxOutput::Srgb;
        up.exposure = 1.0f;
        float half[3] = {0.09f, 0.09f, 0.09f};
        applyAgx(up, half);
        QVERIFY(std::abs(half[1] - 0.49674f) < 2e-3f);
    }

    void agxInputTransfers()
    {
        // The public reference points: 18 % grey encodes to these code values.
        QVERIFY(std::abs(agxLinearize(AgxInput::SLog3, 420.0f / 1023.0f) - 0.18f) < 1e-3f);
        QVERIFY(std::abs(agxLinearize(AgxInput::LogC3, 0.391007f) - 0.18f) < 1e-3f);
        QVERIFY(std::abs(agxLinearize(AgxInput::VLog, 0.423311f) - 0.18f) < 1e-3f);
        QVERIFY(std::abs(agxLinearize(AgxInput::AppleLog, 0.488300f) - 0.18f) < 2e-3f);
        QVERIFY(std::abs(agxLinearize(AgxInput::Rec709, 0.5f) - std::pow(0.5f, 2.4f)) < 1e-6f);
        QVERIFY(std::abs(agxLinearize(AgxInput::Srgb, 0.5f) - 0.21404f) < 1e-4f);
        QCOMPARE(agxLinearize(AgxInput::Linear, 0.3f), 0.3f);
        // Every log is continuous at its toe and rises with the code value.
        for (const AgxInput in : {AgxInput::AppleLog, AgxInput::SLog3, AgxInput::VLog, AgxInput::LogC3}) {
            float last = agxLinearize(in, 0.0f);
            for (int i = 1; i <= 100; ++i) {
                const float now = agxLinearize(in, float(i) / 100.0f);
                QVERIFY(now >= last);
                last = now;
            }
        }
    }

    void agxNodeParametersAndSerialization()
    {
        GradeNode n;
        QVERIFY(go::isNodeIdentity(n, 0));
        n.params.setBase(gk::AgxOn, 1.0);
        QVERIFY(!go::isNodeIdentity(n, 0));
        n.params.setBase(gk::AgxLook, 2.0);
        n.params.setBase(gk::AgxInput, 3.0);
        n.params.setBase(gk::AgxExposure, -1.5);
        n.params.setBase(gk::AgxOutput, 1.0);
        const AgxValues v = AgxValues::at(n, 0);
        QVERIFY(v.on);
        QCOMPARE(int(v.look), int(AgxLook::Golden));
        QCOMPARE(int(v.input), int(AgxInput::AppleLog));
        QCOMPARE(int(v.output), int(AgxOutput::Srgb));
        QCOMPARE(v.exposure, -1.5f);
        n.params.setBase(gk::AgxLook, 99.0); // out of range values are clamped
        QCOMPARE(int(AgxValues::at(n, 0).look), int(AgxLook::Greyscale));
        ClipGrade g;
        const quint64 id = go::initialize(g);
        g.node(id)->params = n.params;
        QCOMPARE(serialization::gradeFromJson(serialization::gradeToJson(g)), g);
        for (const QString& key : gk::agxKeys()) {
            QVERIFY(gk::allKeys().contains(key));
            QVERIFY(gk::label(key) != key);
        }
        QVERIFY(go::resetNode(g, id));
        QVERIFY(!AgxValues::at(*g.node(id), 0).on);
        // The tone mapping grade: empty while off.
        QVERIFY(go::toneMapGrade(ToneMap {}).isEmpty());
        ToneMap on;
        on.agx = true;
        on.look = AgxLook::Punchy;
        const ClipGrade t = go::toneMapGrade(on);
        QVERIFY(!go::isIdentity(t, 0));
        QCOMPARE(int(AgxValues::at(t.nodes[0], 0).look), int(AgxLook::Punchy));
    }

    void toneMapProjectRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Project p;
        QCOMPARE(p.settings().toneMap, ToneMap {}); // off by default
        ProjectSettings s = p.settings();
        s.toneMap.agx = true;
        s.toneMap.look = AgxLook::Golden;
        p.setSettings(s);
        QCOMPARE(p.settings().toneMap, s.toneMap);
        QVERIFY(p.timeline()->data().toneMap.agx); // reaches the timelines
        p.undo();
        QVERIFY(!p.settings().toneMap.agx);
        QVERIFY(!p.timeline()->data().toneMap.agx);
        p.redo();
        const QString path = dir.filePath(QStringLiteral("agx.kkproj"));
        QVERIFY(p.save(path));
        Project back;
        QString error;
        QVERIFY2(back.load(path, &error), qPrintable(error));
        QCOMPARE(back.settings().toneMap, s.toneMap);
        QCOMPARE(back.timeline()->data().toneMap, s.toneMap);
        // A new timeline follows the project.
        const quint64 uid = back.createTimeline(QStringLiteral("Second"), QString(), 1, 1);
        QVERIFY(back.timelineData(uid)->toneMap.agx);
        // A file saved while the mapping is off has no key and loads as off; so does a file
        // from before format 22.
        Project plain;
        const QString plainPath = dir.filePath(QStringLiteral("plain.kkproj"));
        QVERIFY(plain.save(plainPath));
        QFile file(plainPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QVERIFY(!root.value(QLatin1String("project")).toObject().contains(QLatin1String("toneMap")));
        root[QLatin1String("formatVersion")] = 21;
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(root).toJson());
        file.close();
        Project old;
        QVERIFY2(old.load(plainPath, &error), qPrintable(error));
        QCOMPARE(old.settings().toneMap, ToneMap {});
    }

    // ---- Serialization ----

    // ---- Primaries extras, Log palette, Soft Clip, Sat vs Lum, Auto Color, curve picks ----

    void extraKeysDefaultsAndRanges()
    {
        for (const QString& key : {gk::ColorBoost, gk::Shadows, gk::Highlights, gk::MidtoneDetail,
                 gk::LogLowRange, gk::LogHighRange, gk::SoftClipLow, gk::SoftClipHigh}) {
            QVERIFY(gk::allKeys().contains(key));
            QVERIFY2(gk::label(key) != key, qPrintable(key));
        }
        QCOMPARE(gk::defaultValue(gk::ColorBoost), 0.0);
        QCOMPARE(gk::defaultValue(gk::LogLowRange), 0.33);
        QCOMPARE(gk::defaultValue(gk::LogHighRange), 0.55);
        QCOMPARE(gk::defaultValue(gk::SoftClipHigh), 1.0);
        QCOMPARE(gk::range(gk::Shadows), (std::pair<double, double> {-100.0, 100.0}));
        QCOMPARE(gk::range(gk::LogLowRange), (std::pair<double, double> {0.0, 1.0}));
        QCOMPARE(gk::range(gk::LogShadowR), (std::pair<double, double> {-1.0, 1.0}));
        // The four Log wheels have their own keys; every wheel key is a known key.
        QCOMPARE(gk::wheelKey(gk::Wheel::LogShadow, 0), gk::LogShadowR);
        QCOMPARE(gk::wheelKey(gk::Wheel::LogOffset, 3), gk::LogOffsetY);
        for (int w = 0; w < gk::kWheelCount; ++w) {
            for (int ch = 0; ch < 4; ++ch)
                QVERIFY(gk::allKeys().contains(gk::wheelKey(gk::Wheel(w), ch)));
        }
        // A node with only a Log range moved is still identity (the ranges matter only with a wheel).
        GradeNode n;
        n.params.setBase(gk::LogLowRange, 0.2);
        QVERIFY(go::isNodeIdentity(n, 0));
        n.params.setBase(gk::LogMidG, 0.1);
        QVERIFY(!go::isNodeIdentity(n, 0));
        GradeNode shad;
        shad.params.setBase(gk::Shadows, 10.0);
        QVERIFY(!go::isNodeIdentity(shad, 0));
        GradeNode soft;
        soft.params.setBase(gk::SoftClipHigh, 0.9);
        QVERIFY(!go::isNodeIdentity(soft, 0));
        GradeNode md;
        md.params.setBase(gk::MidtoneDetail, -20.0);
        QVERIFY(!go::isNodeIdentity(md, 0));
    }

    void colorBoostProtectsStrongColours()
    {
        QVERIFY(primaries({}).isIdentity());
        const PrimaryValues boost = primaries({{gk::ColorBoost, 100.0}});
        QVERIFY(!boost.isIdentity());
        // Greys and white do not change.
        float grey[3] = {0.4f, 0.4f, 0.4f};
        applyPrimaries(boost, grey);
        KK_COMPARE_RGB(grey, 0.4, 0.4, 0.4, 1e-5);
        // A dull colour gains saturation more than a strong one.
        auto chromaOf = [](const float c[3]) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); };
        float dull[3] = {0.5f, 0.45f, 0.4f};
        float strong[3] = {0.9f, 0.2f, 0.1f};
        const float dull0 = chromaOf(dull), strong0 = chromaOf(strong);
        applyPrimaries(boost, dull);
        applyPrimaries(boost, strong);
        QVERIFY(chromaOf(dull) / dull0 > chromaOf(strong) / strong0 + 0.3f);
        QVERIFY(chromaOf(strong) / strong0 < 1.15f);
        // Negative boost lowers saturation, dull colours first.
        float dull2[3] = {0.5f, 0.45f, 0.4f};
        applyPrimaries(primaries({{gk::ColorBoost, -100.0}}), dull2);
        QVERIFY(chromaOf(dull2) < dull0);
        // Luma is kept.
        float c[3] = {0.5f, 0.45f, 0.4f};
        const float y = luma(c);
        applyPrimaries(boost, c);
        QVERIFY(near(luma(c), y, 1e-5));
    }

    void shadowsAndHighlights()
    {
        const PrimaryValues lift = primaries({{gk::Shadows, 100.0}});
        float dark[3] = {0.1f, 0.1f, 0.1f};
        float bright[3] = {0.9f, 0.9f, 0.9f};
        applyPrimaries(lift, dark);
        applyPrimaries(lift, bright);
        QVERIFY(dark[0] > 0.1f + 0.2f); // shadows lifted
        QVERIFY(near(bright[0], 0.9, 1e-5)); // highlights untouched
        // A negative Shad crushes the darks towards black without going below it.
        float crushed[3] = {0.1f, 0.1f, 0.1f};
        applyPrimaries(primaries({{gk::Shadows, -100.0}}), crushed);
        QVERIFY(crushed[0] < 0.1f && crushed[0] >= 0.0f);
        // Hi recovers (negative) or raises the brights and leaves the darks.
        float hi[3] = {0.9f, 0.9f, 0.9f}, lo[3] = {0.1f, 0.1f, 0.1f};
        applyPrimaries(primaries({{gk::Highlights, -100.0}}), hi);
        applyPrimaries(primaries({{gk::Highlights, -100.0}}), lo);
        QVERIFY(hi[0] < 0.9f - 0.2f);
        QVERIFY(near(lo[0], 0.1, 1e-5));
        float raised[3] = {0.8f, 0.8f, 0.8f};
        applyPrimaries(primaries({{gk::Highlights, 100.0}}), raised);
        QVERIFY(raised[0] > 0.8f && raised[0] <= 1.0f + 1e-6f);
    }

    void logZonesAndWheels()
    {
        // The zone weights always add up to one (midtones take what the ramps leave).
        for (float y = 0.0f; y <= 1.0f; y += 0.05f) {
            float w[3];
            logZoneWeights(y, 0.33f, 0.55f, w);
            QVERIFY(near(w[0] + w[1] + w[2], 1.0, 1e-5));
            QVERIFY(w[0] >= 0 && w[1] >= 0 && w[2] >= 0);
        }
        float w0[3], w1[3];
        logZoneWeights(0.0f, 0.33f, 0.55f, w0);
        logZoneWeights(1.0f, 0.33f, 0.55f, w1);
        QVERIFY(w0[0] > 0.99f && w1[2] > 0.99f);
        // The Shadow wheel moves darks, hardly the brights; the Highlight wheel the opposite.
        const PrimaryValues shadow = primaries({{gk::LogShadowR, 0.2}});
        float dark[3] = {0.05f, 0.05f, 0.05f}, bright[3] = {0.95f, 0.95f, 0.95f};
        applyPrimaries(shadow, dark);
        applyPrimaries(shadow, bright);
        QVERIFY(dark[0] > 0.05f + 0.15f);
        QVERIFY(near(bright[0], 0.95, 1e-4));
        const PrimaryValues high = primaries({{gk::LogHighB, 0.1}, {gk::LogHighY, 0.05}});
        float d2[3] = {0.05f, 0.05f, 0.05f}, b2[3] = {0.95f, 0.95f, 0.95f};
        applyPrimaries(high, d2);
        applyPrimaries(high, b2);
        QVERIFY(near(d2[0], 0.05, 1e-3));
        QVERIFY(near(b2[2], 0.95 + 0.15, 1e-3) && near(b2[0], 0.95 + 0.05, 1e-3));
        // Moving the range boundary moves the zone: a mid grey is shadow-ish with a high Low Range.
        const PrimaryValues lowRange = primaries({{gk::LogShadowG, 0.2}, {gk::LogLowRange, 0.8}});
        float mid[3] = {0.5f, 0.5f, 0.5f}, mid2[3] = {0.5f, 0.5f, 0.5f};
        applyPrimaries(shadow, mid);
        applyPrimaries(primaries({{gk::LogShadowR, 0.2}}), mid2);
        float mid3[3] = {0.5f, 0.5f, 0.5f};
        applyPrimaries(lowRange, mid3);
        QVERIFY(mid3[1] - 0.5f > mid[0] - 0.5f);
        // The Log Offset wheel adds to everything.
        float o[3] = {0.3f, 0.3f, 0.3f};
        applyPrimaries(primaries({{gk::LogOffsetR, 0.1}, {gk::LogOffsetY, 0.02}}), o);
        KK_COMPARE_RGB(o, 0.42, 0.32, 0.32, 1e-5);
    }

    void midtoneDetailStage()
    {
        GradeNode n;
        QVERIFY(!BlurValues::at(n, 0, 1080).stages[3].active());
        n.params.setBase(gk::MidtoneDetail, 50.0);
        BlurValues v = BlurValues::at(n, 0, 1080);
        QVERIFY(v.active() && v.stages[3].active() && v.stages[3].midWeight);
        QVERIFY(!v.stages[0].active() && !v.stages[1].active() && !v.stages[2].active());
        QCOMPARE(v.stages[3].sharp[0], 0.5f * kMidDetailGain);
        QCOMPARE(v.stages[3].mix[0], 0.0f);
        QCOMPARE(v.stages[3].sigmaH[0], kMidDetailSigma * 1080.0f);
        // Negative: softens (blends towards the blur), no sharpening.
        n.params.setBase(gk::MidtoneDetail, -50.0);
        v = BlurValues::at(n, 0, 1080);
        QCOMPARE(v.stages[3].sharp[0], 0.0f);
        QCOMPARE(v.stages[3].mix[0], 0.5f);
        // The midtone weight is 1 at mid grey and 0 at the ends; blurCombine scales by it.
        QCOMPARE(midtoneWeight(0.5f), 1.0f);
        QCOMPARE(midtoneWeight(0.0f), 0.0f);
        QCOMPARE(midtoneWeight(1.0f), 0.0f);
        QCOMPARE(blurCombine(0.6f, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f), 0.7f);
        QCOMPARE(blurCombine(0.6f, 0.5f, 0.0f, 1.0f, 0.0f, 0.5f), 0.65f);
    }

    void softClipMaths()
    {
        QVERIFY(SoftClipValues {}.isIdentity());
        SoftClipValues hard;
        hard.low = 0.2f;
        hard.high = 0.8f;
        QVERIFY(!hard.isIdentity());
        QCOMPARE(softClip(hard, 0.1f), 0.2f);
        QCOMPARE(softClip(hard, 0.5f), 0.5f);
        QCOMPARE(softClip(hard, 0.95f), 0.8f);
        // A soft knee is continuous, monotonic, never passes the limit and starts below it.
        SoftClipValues soft = hard;
        soft.lowSoft = 0.4f;
        soft.highSoft = 0.4f;
        float last = -1.0f;
        for (float x = -0.2f; x <= 1.2f; x += 0.01f) {
            const float y = softClip(soft, x);
            QVERIFY(y >= last - 1e-6f);
            QVERIFY(y >= soft.low - 1e-6f && y <= soft.high + 1e-6f);
            if (last >= 0.0f)
                QVERIFY(std::abs(y - last) < 0.0125f); // no jump
            last = y;
        }
        QVERIFY(softClip(soft, 0.7f) < 0.7f); // the knee begins before the limit
        QCOMPARE(softClip(soft, 0.5f), 0.5f);
        QVERIFY(near(softClip(soft, 1.2f), 0.8, 1e-6));
        // From a node, keyframeable params, applied to the three channels.
        GradeNode n;
        n.params.setBase(gk::SoftClipHigh, 0.5);
        n.params.values[gk::SoftClipLow].setKey(0, 0.0);
        n.params.values[gk::SoftClipLow].setKey(10, 0.4);
        QCOMPARE(SoftClipValues::at(n, 5).low, 0.2f);
        float c[3] = {0.1f, 0.3f, 0.9f};
        applySoftClip(SoftClipValues::at(n, 10), c);
        KK_COMPARE_RGB(c, 0.4, 0.4, 0.5, 1e-6);
        // High below Low is repaired (high follows low).
        GradeNode bad;
        bad.params.setBase(gk::SoftClipLow, 0.7);
        bad.params.setBase(gk::SoftClipHigh, 0.3);
        QVERIFY(SoftClipValues::at(bad, 0).high >= SoftClipValues::at(bad, 0).low);
    }

    void satVsLumCurve()
    {
        // Sat vs Lum at y = 1 for saturated colours only: a saturated colour gets brighter, a
        // grey does not change.
        const CurvePoints boostSaturated = {{0.0, 0.5}, {0.4, 0.5}, {1.0, 1.0}};
        float vivid[3] = {1.0f, 0.1f, 0.1f};
        const float y0 = luma(vivid);
        applyCurves(tablesWith(GradeCurve::SatLum, boostSaturated), vivid);
        QVERIFY(luma(vivid) > y0 + 0.05f);
        float grey[3] = {0.5f, 0.5f, 0.5f};
        applyCurves(tablesWith(GradeCurve::SatLum, boostSaturated), grey);
        KK_COMPARE_RGB(grey, 0.5, 0.5, 0.5, 1e-5);
        // It is a real curve: neutral when empty, saved and restored, listed.
        QVERIFY(GradeCurves {}.isIdentity(GradeCurve::SatLum));
        QCOMPARE(kGradeCurveCount, 10);
        QVERIFY(!isHueCurve(GradeCurve::SatLum) && !isChannelCurve(GradeCurve::SatLum));
        ClipGrade g;
        const quint64 a = go::initialize(g);
        g.node(a)->curves[GradeCurve::SatLum] = boostSaturated;
        QVERIFY(!go::isNodeIdentity(*g.node(a), 0));
        const ClipGrade back = serialization::gradeFromJson(serialization::gradeToJson(g));
        QCOMPARE(back, g);
        QCOMPARE(back.nodes[0].curves[GradeCurve::SatLum], boostSaturated);
        // Hue Lum and Sat Lum together multiply their luma factors.
        float both[3] = {1.0f, 0.1f, 0.1f};
        GradeCurves curves;
        curves[GradeCurve::SatLum] = {{1.0, 0.75}};
        curves[GradeCurve::HueLum] = {{0.0, 0.75}};
        applyCurves(CurveTables::bake(curves), both);
        const float yBoth = luma(both);
        const float y1[3] = {1.0f, 0.1f, 0.1f};
        QVERIFY(near(yBoth, luma(y1) * 1.5 * 1.5, 0.02)); // 1.5 x 1.5 on the luma
    }

    void curvePickPoints()
    {
        const float orange[3] = {0.9f, 0.5f, 0.1f};
        float hsl[3];
        rgbToHsl(orange, hsl);
        // Hue curves: three neutral points, the hue and one either side.
        CurvePoints hue;
        addCurvePickPoints(hue, GradeCurve::HueSat, orange);
        QCOMPARE(hue.size(), 3);
        bool middle = false;
        for (const CurvePoint& p : hue) {
            QCOMPARE(p.y, 0.5);
            middle = middle || near(p.x, hsl[0], 1e-6);
        }
        QVERIFY(middle);
        // Dragging the middle point changes only its area.
        // Picking the same colour again does not add duplicates.
        addCurvePickPoints(hue, GradeCurve::HueSat, orange);
        QCOMPARE(hue.size(), 3);
        // A hue near red wraps the neighbour round to the other end.
        const float red[3] = {1.0f, 0.02f, 0.0f};
        CurvePoints wrap;
        addCurvePickPoints(wrap, GradeCurve::HueHue, red);
        QCOMPARE(wrap.size(), 3);
        for (const CurvePoint& p : wrap)
            QVERIFY(p.x >= 0.0 && p.x <= 1.0);
        QVERIFY(wrap.front().x < 0.01 && wrap.back().x > 0.9);
        // Custom: one point on the diagonal at the luma (Y) or the channel (R, G, B).
        CurvePoints y;
        addCurvePickPoints(y, GradeCurve::Luma, orange);
        QCOMPARE(y.size(), 1);
        const float lum = luma(orange);
        QVERIFY(near(y[0].x, lum, 1e-6) && near(y[0].y, lum, 1e-6));
        CurvePoints g;
        addCurvePickPoints(g, GradeCurve::Green, orange);
        QVERIFY(near(g[0].x, 0.5, 1e-6) && near(g[0].y, 0.5, 1e-6));
        // Lum vs Sat / Sat vs Sat / Sat vs Lum: at the luma or saturation, neutral 0.5.
        CurvePoints ls, ss;
        addCurvePickPoints(ls, GradeCurve::LumSat, orange);
        addCurvePickPoints(ss, GradeCurve::SatLum, orange);
        QCOMPARE(ls.size(), 3);
        QCOMPARE(ss.size(), 3);
        QVERIFY(near(ls[1].x, lum, 1e-6) && near(ss[1].x, hsl[1], 1e-6));
        // A point added to a bent custom curve sits on the curve, so a pick changes nothing.
        CurvePoints bent = {{0.25, 0.1}, {0.75, 0.9}};
        addCurvePickPoints(bent, GradeCurve::Luma, orange);
        QCOMPARE(bent.size(), 3);
        for (const CurvePoint& p : bent) {
            if (near(p.x, lum, 1e-6))
                QVERIFY(near(p.y, evaluateCurve({{0.25, 0.1}, {0.75, 0.9}}, GradeCurve::Luma, lum), 1e-6));
        }
    }

    void autoColorBalancesACast()
    {
        // A picture with a blue cast, black at 0.1 / 0.05 / 0.2 and white at 0.8 / 0.7 / 0.95.
        std::vector<float> img;
        const int n = 2000;
        for (int i = 0; i < n; ++i) {
            const float t = float(i) / float(n - 1);
            img.push_back(0.1f + 0.7f * t);
            img.push_back(0.05f + 0.65f * t);
            img.push_back(0.2f + 0.75f * t);
        }
        const AutoColorResult r = autoColor(img.data(), size_t(n));
        QVERIFY(r.valid);
        // Black and white points land on 0 and 1.
        for (int ch = 0; ch < 3; ++ch) {
            const float lo = 0.1f - 0.05f * float(ch == 1) + 0.1f * float(ch == 2);
            const float hi = ch == 0 ? 0.8f : ch == 1 ? 0.7f : 0.95f;
            QVERIFY2(std::abs(liftGammaGain(lo + 0.002f * (hi - lo), r.lift[ch], 0.0f, r.gain[ch])) < 0.03f, qPrintable(QString::number(ch)));
            QVERIFY2(std::abs(liftGammaGain(hi - 0.002f * (hi - lo), r.lift[ch], 0.0f, r.gain[ch]) - 1.0f) < 0.03f, qPrintable(QString::number(ch)));
        }
        // The midtones are brought together: the three channels' middles agree afterwards.
        float mid[3];
        for (int ch = 0; ch < 3; ++ch) {
            const float lo = 0.1f - 0.05f * float(ch == 1) + 0.1f * float(ch == 2);
            const float hi = ch == 0 ? 0.8f : ch == 1 ? 0.7f : 0.95f;
            mid[ch] = liftGammaGain(lo + 0.5f * (hi - lo), r.lift[ch], r.gamma[ch], r.gain[ch]);
        }
        QVERIFY(std::abs(mid[0] - mid[1]) < 0.03f && std::abs(mid[1] - mid[2]) < 0.03f);
        // A neutral picture needs (almost) nothing; a flat or tiny one is refused.
        std::vector<float> ramp;
        for (int i = 0; i < n; ++i) {
            const float t = float(i) / float(n - 1);
            ramp.insert(ramp.end(), {t, t, t});
        }
        const AutoColorResult neutral = autoColor(ramp.data(), size_t(n));
        QVERIFY(neutral.valid);
        for (int ch = 0; ch < 3; ++ch) {
            QVERIFY(std::abs(neutral.lift[ch]) < 0.02f);
            QVERIFY(std::abs(neutral.gain[ch] - 1.0f) < 0.03f);
            QVERIFY(std::abs(neutral.gamma[ch]) < 0.03f);
        }
        const std::vector<float> flat(300, 0.4f);
        QVERIFY(!autoColor(flat.data(), 100).valid);
        QVERIFY(!autoColor(img.data(), 4).valid);
    }

    void extrasSerializeAndKeyframe()
    {
        ClipGrade g;
        const quint64 a = go::initialize(g);
        g.node(a)->params.setBase(gk::ColorBoost, 35.0);
        g.node(a)->params.setBase(gk::LogShadowB, 0.12);
        g.node(a)->params.setBase(gk::LogLowRange, 0.4);
        g.node(a)->params.setBase(gk::SoftClipHighSoft, 0.3);
        g.node(a)->params.values[gk::Highlights].setKey(0, 0.0);
        g.node(a)->params.values[gk::Highlights].setKey(10, -50.0);
        g.node(a)->params.values[gk::MidtoneDetail].setKey(0, 0.0);
        g.node(a)->params.values[gk::MidtoneDetail].setKey(10, 80.0);
        const ClipGrade back = serialization::gradeFromJson(serialization::gradeToJson(g));
        QCOMPARE(back, g);
        QCOMPARE(PrimaryValues::at(back.nodes[0], 5).highlights, -0.25f);
        QCOMPARE(PrimaryValues::at(back.nodes[0], 0).colorBoost, 0.35f);
        QCOMPARE(PrimaryValues::at(back.nodes[0], 0).logShadow[2], 0.12f);
        QCOMPARE(PrimaryValues::at(back.nodes[0], 0).logLowRange, 0.4f);
        QCOMPARE(BlurValues::at(back.nodes[0], 10, 1080).stages[3].sharp[0], 0.8f * kMidDetailGain);
        // Reset puts them all back.
        QVERIFY(go::resetNode(g, a));
        QVERIFY(go::isNodeIdentity(*g.node(a), 0));
        QVERIFY(go::isNodeIdentity(*g.node(a), 10));
        // An old file without them reads as neutral.
        ClipGrade old;
        go::initialize(old);
        QVERIFY(PrimaryValues::at(old.nodes[0], 0).isIdentity());
    }

    void serializationRoundTrip()
    {
        const ClipGrade g = richGrade();
        const QJsonObject json = serialization::gradeToJson(g);
        QCOMPARE(serialization::gradeFromJson(json), g);
        // Through text as well.
        const QJsonObject text = QJsonDocument::fromJson(QJsonDocument(json).toJson()).object();
        QCOMPARE(serialization::gradeFromJson(text), g);
        // A clip and a timeline carry it; an ungraded clip writes no "grade".
        ClipData clip = mediaClip(0, 10);
        QVERIFY(!serialization::clipToJson(clip).contains(QLatin1String("grade")));
        clip.grade = g;
        QCOMPARE(serialization::clipFromJson(serialization::clipToJson(clip)).grade, g);
        TimelineData tl = timeline();
        QVERIFY(!serialization::timelineToJson(tl).contains(QLatin1String("grade")));
        tl.grade = g;
        TimelineData back;
        QString error;
        QVERIFY2(serialization::timelineFromJson(serialization::timelineToJson(tl), back, &error),
            qPrintable(error));
        QCOMPARE(back.grade, g);
    }

    void damagedJsonIsRepaired()
    {
        const QByteArray text = R"({"output": 7, "nodes": [
            {"id": 1, "type": "corrector", "inputs": [0], "pos": [0, 0],
             "windows": [{"id": 1, "shape": "linear"}, {"id": 0, "shape": "bogus"}]},
            {"id": 1, "type": "corrector", "inputs": [0]},
            {"id": 0, "type": "layer", "inputs": [1]},
            {"id": 3, "type": "corrector", "inputs": [1, 2, 99]},
            {"id": 4, "type": "parallel", "inputs": [3, 42]}]})";
        const ClipGrade g = serialization::gradeFromJson(QJsonDocument::fromJson(text).object());
        QCOMPARE(g.nodes.size(), 3); // duplicate and zero ids dropped
        QCOMPARE(g.output, quint64(0)); // unknown output
        QCOMPARE(g.node(3)->inputs, QVector<quint64> {1}); // a corrector keeps one input
        QCOMPARE(g.node(4)->inputs, (QVector<quint64> {3, 0})); // unknown ids read the source
        // Window ids are made unique within the grade.
        const QVector<GradeWindow>& w = g.node(1)->windows;
        QCOMPARE(w.size(), 2);
        QVERIFY(w[0].id != 1 && w[0].id != 0 && w[1].id != 0 && w[0].id != w[1].id);
        QCOMPARE(w[1].shape, GradeWindowShape::Circle); // unknown shape
        QCOMPARE(g.allocateId(), std::max(w[0].id, w[1].id) + 1);
    }

    void formatVersionAndMigration()
    {
        QVERIFY(serialization::kFormatVersion >= 16); // 17: starting timecodes (bughunt3)
        QTemporaryDir dir;
        // A format 15 file (a composition, no grade, no gallery) loads unchanged.
        const QByteArray v15 = R"({"formatVersion": 15, "project": {"name": "old"},
            "mediaPool": {"bins": [], "items": []},
            "timelines": [{"uid": 1, "name": "T", "fps": {"num": 25, "den": 1}, "width": 1920,
              "height": 1080, "nextId": 10, "videoTracks": [{"id": 1, "clips": [
                {"id": 2, "kind": "solid", "start": 0, "duration": 100,
                 "composition": {"nodes": [], "mediaOut": 0}}]}],
              "audioTracks": []}]})";
        const QString oldPath = dir.filePath(QStringLiteral("v15.kkproj"));
        QFile o(oldPath);
        QVERIFY(o.open(QIODevice::WriteOnly));
        o.write(v15);
        o.close();
        Project old;
        QString error;
        QVERIFY2(old.load(oldPath, &error), qPrintable(error));
        const TimelineData& d = old.timeline()->data();
        QCOMPARE(d.videoTracks[0].clips.size(), size_t(1));
        QVERIFY(d.videoTracks[0].clips[0].grade.isEmpty());
        QVERIFY(d.videoTracks[0].clips[0].composition);
        QVERIFY(d.grade.isEmpty());
        QVERIFY(old.gallery()->stills().isEmpty());
        QVERIFY(!old.isDirty());
        // Saved again: format 16, still no grade or gallery keys (an ungraded project's file
        // only differs by its version number).
        const QString again = dir.filePath(QStringLiteral("again.kkproj"));
        QVERIFY2(old.save(again, &error), qPrintable(error));
        QFile f(again);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray saved = f.readAll();
        const QJsonObject root = QJsonDocument::fromJson(saved).object();
        QCOMPARE(root.value(QLatin1String("formatVersion")).toInt(), serialization::kFormatVersion);
        QVERIFY(!root.contains(QLatin1String("gallery")));
        QVERIFY(!saved.contains("\"grade\""));
    }

    void projectFileRoundTrip()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("graded.kkproj"));
        Project p;
        const quint64 uid = p.activeTimelineUid();
        p.beginEdit(QStringLiteral("Add"));
        const quint64 id
            = editops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 50));
        p.endEdit();
        QVERIFY(p.setClipGrade(uid, id, richGrade(), QStringLiteral("Grade")));
        ClipGrade tl;
        go::initialize(tl);
        tl.nodes[0].params.setBase(gk::Saturation, 0.5);
        QVERIFY(p.setTimelineGrade(uid, tl, QStringLiteral("Timeline Grade")));
        QImage still(64, 36, QImage::Format_RGB32);
        still.fill(QColor(200, 100, 50));
        p.gallery()->addStill(still, richGrade());
        QString error;
        QVERIFY2(p.save(path, &error), qPrintable(error));
        Project again;
        QVERIFY2(again.load(path, &error), qPrintable(error));
        QCOMPARE(again.clipGrade(again.activeTimelineUid(), id), richGrade());
        QCOMPARE(again.timelineGrade(again.activeTimelineUid()), tl);
        QCOMPARE(again.gallery()->stills().size(), 1);
        const GalleryStill& s = again.gallery()->stills()[0];
        QCOMPARE(s.grade, richGrade());
        QCOMPARE(s.image.size(), QSize(64, 36));
        QVERIFY(std::abs(QColor(s.image.pixel(10, 10)).red() - 200) < 8); // JPEG
        QCOMPARE(s.name, QStringLiteral("1.1"));
    }

    // ---- Undo ----

    void projectUndo()
    {
        Project p;
        const quint64 uid = p.activeTimelineUid();
        p.beginEdit(QStringLiteral("Add"));
        const quint64 a
            = editops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 50));
        const quint64 b
            = editops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(50, 50));
        p.endEdit();
        const int steps = p.undoStack()->count();
        QVERIFY(p.clipGrade(uid, a).isEmpty());

        ClipGrade g;
        const quint64 node = go::initialize(g);
        QVERIFY(p.setClipGrade(uid, a, g, QStringLiteral("Add Serial Node")));
        QCOMPARE(p.undoStack()->count(), steps + 1);
        QCOMPARE(p.undoStack()->undoText(), QStringLiteral("Add Serial Node"));
        // The same value again makes no step.
        QVERIFY(p.setClipGrade(uid, a, g, QStringLiteral("Nothing")));
        QCOMPARE(p.undoStack()->count(), steps + 1);
        // A wheel drag: many calls inside one open edit are one step.
        p.beginEdit(QStringLiteral("Gain"));
        for (const double v : {1.1, 1.2, 1.3}) {
            g.node(node)->params.setBase(gk::GainY, v);
            QVERIFY(p.setClipGrade(uid, a, g, QStringLiteral("Gain")));
        }
        p.endEdit();
        QCOMPARE(p.undoStack()->count(), steps + 2);
        QCOMPARE(p.clipGrade(uid, a).node(node)->value(gk::GainY, 0), 1.3);
        // Copy the grade to both clips in one step.
        QCOMPARE(p.setClipGrades(uid, {a, b, 999}, g, QStringLiteral("Apply Grade")), 1);
        QCOMPARE(p.undoStack()->count(), steps + 3);
        QCOMPARE(p.clipGrade(uid, b), g);
        // The timeline grade.
        QVERIFY(p.setTimelineGrade(uid, g, QStringLiteral("Timeline Grade")));
        QCOMPARE(p.undoStack()->count(), steps + 4);
        p.undo();
        QVERIFY(p.timelineGrade(uid).isEmpty());
        p.undo();
        QVERIFY(p.clipGrade(uid, b).isEmpty());
        p.undo();
        QCOMPARE(p.clipGrade(uid, a).node(node)->value(gk::GainY, 0), 1.0);
        p.undo();
        QVERIFY(p.clipGrade(uid, a).isEmpty());
        p.redo();
        QCOMPARE(p.clipGrade(uid, a), [] {
            ClipGrade x;
            go::initialize(x);
            return x;
        }());
        // Locked tracks and unknown clips / timelines are refused.
        p.beginEdit(QStringLiteral("Lock"));
        p.timeline()->mutableData().track(kV1)->locked = true;
        p.endEdit();
        QVERIFY(!p.setClipGrade(uid, a, ClipGrade {}, QStringLiteral("x")));
        QVERIFY(!p.setClipGrade(uid, 999, g, QStringLiteral("x")));
        QVERIFY(!p.setTimelineGrade(9999, g, QStringLiteral("x")));
    }

    void inactiveTimelineUndo()
    {
        Project p;
        const quint64 first = p.activeTimelineUid();
        p.beginEdit(QStringLiteral("Add"));
        const quint64 id
            = editops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 50));
        p.endEdit();
        const quint64 second = p.createTimeline(QStringLiteral("Other"));
        QVERIFY(p.setActiveTimeline(second));
        ClipGrade g;
        go::initialize(g);
        QVERIFY(p.setClipGrade(first, id, g, QStringLiteral("Grade")));
        QVERIFY(p.setTimelineGrade(first, g, QStringLiteral("Timeline Grade")));
        QCOMPARE(p.clipGrade(first, id), g);
        QCOMPARE(p.activeTimelineUid(), second);
        p.undo();
        p.undo();
        QVERIFY(p.clipGrade(first, id).isEmpty());
        QVERIFY(p.timelineGrade(first).isEmpty());
    }

    // ---- Gallery ----

    void gallery()
    {
        Project p;
        QVERIFY(!p.isDirty());
        GradeGallery* gallery = p.gallery();
        QSignalSpy changed(gallery, &GradeGallery::changed);
        QImage big(1280, 720, QImage::Format_RGB32);
        big.fill(Qt::red);
        const quint64 a = gallery->addStill(big, richGrade());
        const quint64 b = gallery->addStill(big, ClipGrade {});
        QCOMPARE(changed.count(), 2);
        QVERIFY(p.isDirty()); // not undoable, but the project has to be saved
        QCOMPARE(gallery->still(a)->image.width(), kGalleryImageWidth);
        QCOMPARE(gallery->still(b)->name, QStringLiteral("1.2"));
        QVERIFY(gallery->renameStill(b, QStringLiteral("Night")));
        QCOMPARE(gallery->still(b)->name, QStringLiteral("Night"));
        GradeGallery copy;
        copy.fromJson(gallery->toJson());
        QCOMPARE(copy.stills().size(), 2);
        QCOMPARE(copy.still(a)->grade, richGrade());
        QVERIFY(gallery->removeStill(a));
        QVERIFY(!gallery->removeStill(a));
        p.newProject();
        QVERIFY(gallery->stills().isEmpty());
        QVERIFY(!p.isDirty());
    }
};

QTEST_GUILESS_MAIN(TestGrade)
#include "test_grade.moc"
