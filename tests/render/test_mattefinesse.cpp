// SPDX-License-Identifier: GPL-3.0-or-later
// Qualifier Matte Finesse and the RGB / Luma / 3D qualifier modes (spec section 108): the
// matte maths on small pictures (Clean Black / White, Shrink / Grow, Blur Radius, In / Out
// Ratio, Denoise, their order), the modes' keys, the pick maths of the new modes, the project
// JSON round trip and that a grade with finesse renders through the CPU reference.
#include "core/GradeMath.h"
#include "core/Serialization.h"
#include "render/GradeEval.h"
#include "ui/color/QualifierPick.h"

#include <QTest>

#include <cmath>

using namespace kk;
using namespace kk::grademath;
namespace go = kk::gradeops;

namespace {

constexpr int kW = 24;
constexpr int kH = 24;

std::vector<float> blank(float v = 0.0f)
{
    return std::vector<float>(size_t(kW) * kH, v);
}

float& at(std::vector<float>& m, int x, int y)
{
    return m[size_t(y) * kW + size_t(x)];
}

float at(const std::vector<float>& m, int x, int y)
{
    return m[size_t(y) * kW + size_t(x)];
}

float sum(const std::vector<float>& m)
{
    float s = 0;
    for (const float v : m)
        s += v;
    return s;
}

/// A white block x0..x1, y0..y1 (inclusive) on black.
std::vector<float> block(int x0, int y0, int x1, int y1)
{
    std::vector<float> m = blank();
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            at(m, x, y) = 1.0f;
    return m;
}

MatteFinesse withRadii(int cleanBlack, int cleanWhite, int shrinkGrow, int denoise, float sigma,
    float ratio)
{
    MatteFinesse f;
    f.cleanBlack = cleanBlack;
    f.cleanWhite = cleanWhite;
    f.shrinkGrow = shrinkGrow;
    f.denoise = denoise;
    f.blurSigma = sigma;
    f.ratio = ratio;
    return f;
}

} // namespace

class TestMatteFinesse : public QObject
{
    Q_OBJECT
private slots:
    void inactiveByDefault()
    {
        QVERIFY(!MatteFinesse::at(GradeQualifier {}, 1080).active());
        GradeQualifier q;
        q.denoise = 0.5;
        QVERIFY(MatteFinesse::at(q, 1080).active());
        QVERIFY(!GradeQualifier {}.hasFinesse());
        QVERIFY(q.hasFinesse());
        // The radii follow the picture height, and any set value moves at least one pixel.
        q = {};
        q.shrinkGrow = -1.0;
        QCOMPARE(MatteFinesse::at(q, 1000).shrinkGrow, -15);
        q.shrinkGrow = 0.01;
        QCOMPARE(MatteFinesse::at(q, 36).shrinkGrow, 1);
        // Nothing active changes nothing.
        std::vector<float> m = block(4, 4, 10, 10);
        const std::vector<float> before = m;
        applyMatteFinesse({}, m, kW, kH);
        QCOMPARE(m, before);
    }

    void cleanBlackRemovesWhiteSpecks()
    {
        std::vector<float> m = block(6, 6, 16, 16);
        at(m, 2, 2) = 1.0f;
        at(m, 20, 3) = 1.0f; // two single white pixels in the black
        applyMatteFinesse(withRadii(1, 0, 0, 0, 0, 0), m, kW, kH);
        QCOMPARE(at(m, 2, 2), 0.0f);
        QCOMPARE(at(m, 20, 3), 0.0f);
        QCOMPARE(at(m, 11, 11), 1.0f); // the block stays
        QCOMPARE(at(m, 6, 6), 1.0f); // with its corners
    }

    void cleanWhiteFillsBlackHoles()
    {
        std::vector<float> m = block(4, 4, 18, 18);
        at(m, 10, 10) = 0.0f;
        at(m, 12, 8) = 0.0f;
        applyMatteFinesse(withRadii(0, 1, 0, 0, 0, 0), m, kW, kH);
        QCOMPARE(at(m, 10, 10), 1.0f);
        QCOMPARE(at(m, 12, 8), 1.0f);
        QCOMPARE(at(m, 1, 1), 0.0f);
    }

    void shrinkAndGrowMoveTheEdge()
    {
        const std::vector<float> original = block(8, 8, 15, 15); // 8 x 8
        std::vector<float> grown = original, shrunk = original;
        applyMatteFinesse(withRadii(0, 0, 2, 0, 0, 0), grown, kW, kH);
        applyMatteFinesse(withRadii(0, 0, -2, 0, 0, 0), shrunk, kW, kH);
        QCOMPARE(sum(grown), 12.0f * 12.0f);
        QCOMPARE(sum(shrunk), 4.0f * 4.0f);
        QCOMPARE(at(grown, 6, 6), 1.0f);
        QCOMPARE(at(shrunk, 8, 8), 0.0f);
        QCOMPARE(at(shrunk, 10, 10), 1.0f);
    }

    void denoiseIsAMedian()
    {
        std::vector<float> m = block(4, 4, 19, 19);
        at(m, 10, 10) = 0.0f; // grain inside
        at(m, 1, 1) = 1.0f; // grain outside
        applyMatteFinesse(withRadii(0, 0, 0, 1, 0, 0), m, kW, kH);
        QCOMPARE(at(m, 10, 10), 1.0f);
        QCOMPARE(at(m, 1, 1), 0.0f);
        QCOMPARE(at(m, 8, 8), 1.0f);
        // A straight edge survives a median (no blur, no shift).
        std::vector<float> edge = block(0, 0, 11, kH - 1);
        applyMatteFinesse(withRadii(0, 0, 0, 2, 0, 0), edge, kW, kH);
        QCOMPARE(at(edge, 11, 12), 1.0f);
        QCOMPARE(at(edge, 12, 12), 0.0f);
    }

    void blurSoftensTheEdgeAndKeepsTheMean()
    {
        std::vector<float> m = block(0, 0, 11, kH - 1);
        const float before = sum(m);
        applyMatteFinesse(withRadii(0, 0, 0, 0, 2.0f, 0), m, kW, kH);
        const float mid = at(m, 11, 12);
        QVERIFY(mid > 0.5f && mid < 1.0f);
        QVERIFY(at(m, 12, 12) > 0.0f && at(m, 12, 12) < 0.5f);
        QVERIFY(std::abs(sum(m) - before) < 0.05f * before);
        QVERIFY(at(m, 2, 12) > 0.99f);
    }

    void inOutRatioMovesTheSoftEdge()
    {
        const std::vector<float> original = block(0, 0, 11, kH - 1);
        auto run = [&](float ratio) {
            std::vector<float> m = original;
            applyMatteFinesse(withRadii(0, 0, 0, 0, 2.0f, ratio), m, kW, kH);
            return m;
        };
        const std::vector<float> even = run(0.0f), inside = run(1.0f), outside = run(-1.0f);
        // Inside: the soft edge sits within the original white; the outside stays black.
        QVERIFY(at(inside, 12, 12) < 0.001f);
        QVERIFY(at(inside, 10, 12) < at(even, 10, 12));
        QVERIFY(at(inside, 8, 12) > 0.0f);
        // Outside: the white stays full to the original edge and the ramp lies beyond it.
        QVERIFY(at(outside, 11, 12) > 0.99f);
        QVERIFY(at(outside, 13, 12) > at(even, 13, 12));
        QVERIFY(sum(outside) > sum(even) && sum(even) > sum(inside));
        // The remap itself: monotonic, 0 -> 0 and 1 -> 1.
        for (const float r : {-1.0f, -0.4f, 0.4f, 1.0f}) {
            QCOMPARE(matteRatio(0.0f, r), 0.0f);
            QCOMPARE(matteRatio(1.0f, r), 1.0f);
            QVERIFY(matteRatio(0.3f, r) <= matteRatio(0.6f, r));
        }
    }

    void orderIsCleanThenDenoiseThenShrinkThenBlur()
    {
        // A speck next to a block: Clean Black removes it before Grow could join it to the block.
        std::vector<float> m = block(10, 10, 16, 16);
        at(m, 6, 13) = 1.0f;
        std::vector<float> both = m;
        applyMatteFinesse(withRadii(1, 0, 3, 0, 0, 0), both, kW, kH);
        QCOMPARE(at(both, 3, 13), 0.0f); // the speck did not grow
        QCOMPARE(at(both, 8, 13), 1.0f); // the block did
    }

    void qualifierMatteUsesTheFinesse()
    {
        // A key picture: red block on grey; the qualifier selects red. One red speck outside.
        const int w = kW, h = kH;
        std::vector<float> rgb(size_t(w) * h * 3, 0.5f);
        auto setRed = [&](int x, int y) {
            float* p = &rgb[(size_t(y) * w + size_t(x)) * 3];
            p[0] = 0.9f, p[1] = 0.1f, p[2] = 0.1f;
        };
        for (int y = 6; y < 18; ++y)
            for (int x = 6; x < 18; ++x)
                setRed(x, y);
        setRed(2, 2);
        GradeQualifier q;
        q.enabled = true;
        q.hueCenter = 0.0;
        q.hueWidth = 0.1;
        q.satLow = 0.3;
        const std::vector<float> raw
            = render::qualifierMatte(q, MatteFinesse::at(q, h), rgb, w, h);
        QCOMPARE(raw[size_t(2) * w + 2], 1.0f);
        q.cleanBlack = 1.0; // 0.015 x 24 px rounds to the 1 px minimum
        const std::vector<float> clean
            = render::qualifierMatte(q, MatteFinesse::at(q, h), rgb, w, h);
        QCOMPARE(clean[size_t(2) * w + 2], 0.0f);
        QCOMPARE(clean[size_t(12) * w + 12], 1.0f);
    }

    void rgbMode()
    {
        GradeQualifier q;
        q.enabled = true;
        q.mode = QualifierMode::Rgb;
        q.redLow = 0.6;
        q.greenHigh = 0.4;
        q.blueHigh = 0.4;
        q.redSoft = q.greenSoft = q.blueSoft = 0.01;
        const float red[3] = {0.9f, 0.2f, 0.2f}, green[3] = {0.2f, 0.9f, 0.2f};
        QVERIFY(qualifierKey(q, red) > 0.99f);
        QVERIFY(qualifierKey(q, green) < 0.01f);
        q.useRed = false; // the red range no longer limits
        const float dark[3] = {0.1f, 0.2f, 0.2f};
        QVERIFY(qualifierKey(q, dark) > 0.99f);
        q.invert = true;
        QVERIFY(qualifierKey(q, dark) < 0.01f);
    }

    void lumaModeIgnoresColour()
    {
        GradeQualifier q;
        q.enabled = true;
        q.mode = QualifierMode::Luma;
        q.hueCenter = 0.5; // would exclude red in HSL mode
        q.hueWidth = 0.05;
        q.lumLow = 0.3;
        q.lumHigh = 1.0;
        q.lumSoft = 0.01;
        const float red[3] = {0.95f, 0.2f, 0.2f}, dim[3] = {0.1f, 0.1f, 0.1f};
        QVERIFY(qualifierKey(q, red) > 0.99f); // luma 0.36
        QVERIFY(qualifierKey(q, dim) < 0.01f);
        q.mode = QualifierMode::Hsl;
        QVERIFY(qualifierKey(q, red) < 0.01f);
    }

    void threeDMode()
    {
        GradeQualifier q;
        q.enabled = true;
        q.mode = QualifierMode::ThreeD;
        const float skin[3] = {0.8f, 0.6f, 0.5f}, near[3] = {0.82f, 0.58f, 0.52f},
                    sky[3] = {0.3f, 0.5f, 0.9f};
        QCOMPARE(qualifierKey(q, skin), 0.0f); // nothing picked: nothing selected
        q.colors.append({0.8f, 0.6f, 0.5f, false});
        QVERIFY(qualifierKey(q, skin) > 0.99f);
        QVERIFY(qualifierKey(q, near) > 0.99f);
        QVERIFY(qualifierKey(q, sky) < 0.01f);
        q.colors.append({0.3f, 0.5f, 0.9f, false});
        QVERIFY(qualifierKey(q, sky) > 0.99f);
        q.colors.append({0.82f, 0.58f, 0.52f, true}); // take the nearby colour out again
        QVERIFY(qualifierKey(q, near) < 0.01f);
        QVERIFY(qualifierKey(q, sky) > 0.99f);
    }

    void picksInTheNewModes()
    {
        using namespace qualifierpick;
        const QVector<Rgb> red {{0.9f, 0.1f, 0.1f}}, blue {{0.1f, 0.1f, 0.9f}};
        GradeQualifier q;
        q.mode = QualifierMode::Rgb;
        q = applyForMode(q, red, Mode::Pick);
        QVERIFY(q.enabled);
        const float r[3] = {0.9f, 0.1f, 0.1f}, b[3] = {0.1f, 0.1f, 0.9f};
        QVERIFY(qualifierKey(q, r) > 0.99f);
        QVERIFY(qualifierKey(q, b) < 0.01f);
        q = applyForMode(q, blue, Mode::Add);
        QVERIFY(qualifierKey(q, b) > 0.99f);

        GradeQualifier luma;
        luma.mode = QualifierMode::Luma;
        luma = applyForMode(luma, red, Mode::Pick);
        const float lumaOfRed = grademath::luma(r);
        QVERIFY(luma.lumLow <= lumaOfRed && luma.lumHigh >= lumaOfRed);
        QCOMPARE(luma.hueWidth, GradeQualifier {}.hueWidth); // only the luma range moved

        GradeQualifier d;
        d.mode = QualifierMode::ThreeD;
        d = applyForMode(d, {{0.9f, 0.1f, 0.1f}, {0.9f, 0.1f, 0.1f}, {0.9f, 0.11f, 0.1f}}, Mode::Pick);
        QCOMPARE(d.colors.size(), 1); // near-equal samples count once
        d = applyForMode(d, blue, Mode::Add);
        QCOMPARE(d.colors.size(), 2);
        d = applyForMode(d, blue, Mode::Remove);
        QCOMPARE(d.colors.size(), 3);
        QVERIFY(d.colors.last().subtract);
        QVERIFY(qualifierKey(d, r) > 0.99f);
        QVERIFY(qualifierKey(d, b) < 0.01f);
        d = applyForMode(d, {{0.5f, 0.5f, 0.5f}}, Mode::Pick);
        QCOMPARE(d.colors.size(), 1); // Pick replaces
    }

    void serializationAndReset()
    {
        ClipGrade g;
        const quint64 n = go::initialize(g);
        GradeQualifier& q = g.node(n)->qualifier;
        q.enabled = true;
        q.mode = QualifierMode::ThreeD;
        q.colors = {{0.1f, 0.5f, 0.25f, false}, {0.9f, 0.5f, 0.25f, true}};
        q.colorRange = 0.2;
        q.colorSoft = 0.3;
        q.cleanBlack = 0.1;
        q.cleanWhite = 0.2;
        q.shrinkGrow = -0.3;
        q.blurRadius = 0.4;
        q.inOutRatio = 0.5;
        q.denoise = 0.6;
        q.useRed = false;
        q.redLow = 0.2;
        q.blueSoft = 0.3;
        QCOMPARE(serialization::gradeFromJson(serialization::gradeToJson(g)), g);
        q.mode = QualifierMode::Luma;
        QCOMPARE(serialization::gradeFromJson(serialization::gradeToJson(g)), g);
        // An older project's qualifier has none of the new keys: HSL mode, no finesse.
        GradeQualifier old;
        old.enabled = true;
        old.hueCenter = 0.3;
        ClipGrade h;
        go::initialize(h);
        h.nodes[0].qualifier = old;
        const QJsonObject json = serialization::gradeToJson(h);
        const QJsonObject qj = json.value(QLatin1String("nodes")).toArray()[0].toObject()
                                   .value(QLatin1String("qualifier")).toObject();
        QVERIFY(!qj.contains(QLatin1String("mode")));
        QVERIFY(!qj.contains(QLatin1String("cleanBlack")));
        const ClipGrade back = serialization::gradeFromJson(json);
        QCOMPARE(back.nodes[0].qualifier.mode, QualifierMode::Hsl);
        QVERIFY(!back.nodes[0].qualifier.hasFinesse());
        QVERIFY(back.nodes[0].qualifier.colors.isEmpty());
        // Reset node clears the lot.
        QVERIFY(go::resetNode(g, n));
        QCOMPARE(g.node(n)->qualifier, GradeQualifier {});
        QVERIFY(serialization::kFormatVersion >= 20);
    }

    void cpuGradeCleansTheKey()
    {
        // A red and a grey half; one red speck in the grey half. The qualifier selects red and
        // darkens it; with Clean Black the speck is left alone, without it the speck darkens.
        const int w = 40, h = 40;
        QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
        img.fill(qRgb(128, 128, 128));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w / 2; ++x)
                img.setPixel(x, y, qRgb(230, 30, 30));
        img.setPixel(30, 20, qRgb(230, 30, 30));
        ClipGrade g;
        const quint64 n = go::initialize(g);
        g.node(n)->params.setBase(gradekeys::GainR, 0.3);
        g.node(n)->params.setBase(gradekeys::GainG, 0.3);
        g.node(n)->params.setBase(gradekeys::GainB, 0.3);
        GradeQualifier& q = g.node(n)->qualifier;
        q.enabled = true;
        q.hueWidth = 0.1;
        q.satLow = 0.3;
        QImage plain = img;
        render::applyGrade(g, 0, plain);
        QVERIFY(qRed(plain.pixel(30, 20)) < 150); // the speck darkened
        QVERIFY(qRed(plain.pixel(5, 20)) < 150);
        q.cleanBlack = 1.0; // 0.015 x 40 px = 0.6 px: one pixel
        QImage cleaned = img;
        render::applyGrade(g, 0, cleaned);
        QCOMPARE(qRed(cleaned.pixel(30, 20)), qRed(img.pixel(30, 20))); // untouched
        QVERIFY(qRed(cleaned.pixel(5, 20)) < 150); // the red half still darkens
        QVERIFY(qRed(cleaned.pixel(35, 5)) == 128); // grey stays
    }
};

QTEST_GUILESS_MAIN(TestMatteFinesse)
#include "test_mattefinesse.moc"
