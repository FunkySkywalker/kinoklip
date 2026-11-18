// SPDX-License-Identifier: GPL-3.0-or-later
// Planar Tracker Track Mask input, several patterns in one Tracker node (rotation and scale from
// two patterns), and Follow Track for size and angle (spec section 118).
#include "compositing/tracking/PlanarTracker.h"
#include "compositing/tracking/TrackFollow.h"
#include "compositing/tracking/TrackLink.h"
#include "compositing/tracking/TrackStore.h"
#include "core/CompOps.h"
#include "core/NodeRegistry.h"
#include "core/Serialization.h"
#include "render/CompMath.h"

#include <QTest>

#include <cmath>
#include <random>

using namespace kk;
using namespace kk::tracking;
namespace co = kk::compops;
namespace compmath = kk::render::compmath;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;
constexpr QSizeF kFrame(kW, kH);

double texture(double x, double y)
{
    static const std::vector<std::array<double, 4>> waves = [] {
        std::mt19937 rng(4242);
        std::uniform_real_distribution<double> wl(6.0, 28.0), dir(0.0, 2 * M_PI), ph(0.0, 2 * M_PI);
        std::vector<std::array<double, 4>> w;
        for (int i = 0; i < 24; ++i) {
            const double k = 2 * M_PI / wl(rng), a = dir(rng);
            w.push_back({k * std::cos(a), k * std::sin(a), ph(rng), 0.085});
        }
        return w;
    }();
    double v = 0.5;
    for (const auto& w : waves)
        v += w[3] * std::sin(w[0] * x + w[1] * y + w[2]);
    return v;
}

/// The textured plane shifted by (2 t, 0).
GrayImage frameAt(double t)
{
    GrayImage img;
    img.width = kW;
    img.height = kH;
    img.data.resize(size_t(kW) * kH);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            img.data[size_t(y) * kW + x] = float(texture(x - 2.0 * t, y));
    return img;
}

GrayImage maskLeftOf(int xEdge, bool all = false)
{
    GrayImage m;
    m.width = kW;
    m.height = kH;
    m.data.assign(size_t(kW) * kH, 0.0f);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < (all ? kW : xEdge); ++x)
            m.data[size_t(y) * kW + x] = 1.0f;
    return m;
}

Quad region()
{
    return {{{40, 30}, {280, 30}, {280, 150}, {40, 150}}};
}

} // namespace

class TrackerPatternsTest : public QObject
{
    Q_OBJECT
private slots:
    void trackMaskKeepsPatchesInsideTheMask()
    {
        const GrayImage first = frameAt(0);
        PlanarTracker plain;
        QVERIFY(plain.reset(first, region()));
        const PlanarResult free = plain.track(frameAt(2));
        QVERIFY(free.ok);
        double freeMax = 0;
        for (const Vec2& p : free.inlierFrom)
            freeMax = std::max(freeMax, p.x);
        QVERIFY(freeMax > 200); // without a mask patches sit all over the region

        const GrayImage mask = maskLeftOf(150);
        PlanarTracker masked;
        QVERIFY(masked.reset(first, region(), &mask));
        const PlanarResult r = masked.track(frameAt(2), &mask);
        QVERIFY(r.ok);
        QVERIFY(!r.inlierFrom.empty());
        QVERIFY(r.inliers < free.inliers);
        for (const Vec2& p : r.inlierFrom)
            QVERIFY2(p.x < 150, qPrintable(QString::number(p.x)));
        // Still a good fit of the plane's movement (4 px right after 2 frames).
        QVERIFY(std::abs(r.corners[0].x - (40 + 4)) < 0.5);
        // A mask that is black everywhere leaves no patch: tracking cannot start.
        const GrayImage none = maskLeftOf(0);
        PlanarTracker blank;
        QVERIFY(!blank.reset(first, region(), &none));
        // A white mask changes nothing.
        const GrayImage white = maskLeftOf(0, true);
        PlanarTracker all;
        QVERIFY(all.reset(first, region(), &white));
        QCOMPARE(all.track(frameAt(2), &white).inliers, free.inliers);
    }

    void plannerTrackerHasTheMaskPort()
    {
        const NodeDescriptor* d = NodeRegistry::instance().find(nodetypes::PlanarTracker);
        QVERIFY(d);
        const NodePort* p = d->input(nodeports::TrackMask);
        QVERIFY(p);
        QCOMPARE(p->kind, PortKind::Mask);
        QVERIFY(p->analysisOnly);
        QVERIFY(!p->required);
        // A mask node can be plugged in and the port is never drawn through.
        Composition c = co::defaultComposition();
        const quint64 pt = co::addNode(c, nodetypes::PlanarTracker, {});
        const quint64 m = co::addNode(c, nodetypes::EllipseMask, {});
        QCOMPARE(co::connect(c, m, 0, pt, nodeports::TrackMask), co::ConnectError::None);
        QVERIFY(!co::evaluationOrder(c, pt).contains(m));
        // Saved and loaded with the link.
        const Composition back
            = serialization::compositionFromJson(serialization::compositionToJson(c));
        QVERIFY(back.node(pt)->inputs.value(nodeports::TrackMask).isConnected());
    }

    void severalPatternsTurnAndScaleTheOutput()
    {
        ParamSet p = NodeRegistry::instance().defaultParams(nodetypes::Tracker);
        QCOMPARE(int(std::lround(p.value(nodekeys::Patterns, 0, -1))), 1);
        // One pattern: translation only, as before.
        p.animated(nodekeys::CenterX, 0.5).setKey(0, 0.5);
        p.animated(nodekeys::CenterX, 0.5).setKey(10, 0.6);
        const QTransform one = compmath::trackerMatrix(p, 10, kFrame);
        QVERIFY(std::abs(one.dx() - 32) < 1e-6 && std::abs(one.m11() - 1) < 1e-9
            && std::abs(one.m12()) < 1e-9);

        // Two patterns: A stays at the middle, B at (0.6, 0.5) goes to a point twice as far and
        // 90 degrees round (counter-clockwise on the screen, so up).
        ParamSet q = NodeRegistry::instance().defaultParams(nodetypes::Tracker);
        q.setBase(nodekeys::Patterns, 2);
        q.setBase(nodekeys::patternKey(2, false), 0.5 + 32.0 / kW);
        q.setBase(nodekeys::patternKey(2, true), 0.5);
        q.animated(nodekeys::patternKey(2, false), 0.6).setKey(0, 0.5 + 32.0 / kW);
        q.animated(nodekeys::patternKey(2, false), 0.6).setKey(10, 0.5);
        q.animated(nodekeys::patternKey(2, true), 0.5).setKey(0, 0.5);
        q.animated(nodekeys::patternKey(2, true), 0.5).setKey(10, 0.5 + 64.0 / kH);
        // Pattern A (CenterX/Y) stays at 0.5, 0.5.
        QTransform t = compmath::trackerMatrix(q, 10, kFrame);
        // The group's middle: before ((160)+(192))/2 = 176, 90; after (160, 90)+(160, 26) / 2.
        const double angle = std::atan2(t.m12(), t.m11()) * 180 / M_PI;
        const double scale = std::hypot(t.m11(), t.m12());
        QVERIFY2(std::abs(scale - 2.0) < 1e-6, qPrintable(QString::number(scale)));
        QVERIFY2(std::abs(angle - -90.0) < 1e-6, qPrintable(QString::number(angle)));
        // The middle of the group before maps to the middle after.
        const QPointF mid = t.map(QPointF(176, 90));
        QVERIFY(std::abs(mid.x() - 160) < 1e-6 && std::abs(mid.y() - 58) < 1e-6);
        // Match Rotation / Scale / Position off: each part is dropped.
        q.statics[nodekeys::TrackRotation] = false;
        t = compmath::trackerMatrix(q, 10, kFrame);
        QVERIFY(std::abs(std::atan2(t.m12(), t.m11())) < 1e-9);
        q.statics[nodekeys::TrackScale] = false;
        q.statics[nodekeys::TrackPosition] = false;
        QVERIFY(compmath::trackerMatrix(q, 10, kFrame).isIdentity());
        q.statics[nodekeys::TrackPosition] = true;
        // Stabilize inverts everything.
        q.statics[nodekeys::TrackRotation] = true;
        q.statics[nodekeys::TrackScale] = true;
        const QTransform fwd = compmath::trackerMatrix(q, 10, kFrame);
        q.statics[nodekeys::TrackMode] = trackmodes::Stabilize;
        const QTransform inv = compmath::trackerMatrix(q, 10, kFrame);
        const QPointF back = inv.map(fwd.map(QPointF(17, 33)));
        QVERIFY(std::abs(back.x() - 17) < 1e-6 && std::abs(back.y() - 33) < 1e-6);
        // At the reference frame nothing moves.
        QVERIFY(compmath::trackerMatrix(q, 0, kFrame).isIdentity());
    }

    void patternsAreStoredAndCleared()
    {
        Composition c = co::defaultComposition();
        const quint64 t = co::addNode(c, nodetypes::Tracker, {});
        CompNode& n = *c.node(t);
        n.params.setBase(nodekeys::Patterns, 3);
        std::vector<TrackSample> a, b;
        for (int f = 0; f <= 5; ++f) {
            a.push_back({f, 0.3 + 0.01 * f, 0.4, 1.0});
            b.push_back({f, 0.7, 0.6 - 0.01 * f, 1.0});
        }
        QVERIFY(storeTrack(n, a, 0, 1));
        QVERIFY(storeTrack(n, b, 0, 2));
        QVERIFY(std::abs(n.params.value(nodekeys::CenterX, 5, 0) - 0.35) < 1e-9);
        QVERIFY(std::abs(n.params.value(nodekeys::patternKey(2, true), 5, 0) - 0.55) < 1e-9);
        QVERIFY(!n.params.values.value(nodekeys::patternKey(3, false)).isAnimated());
        QCOMPARE(trackFrames(n).size(), size_t(6));
        // The centre of a track with several patterns is where their middle is.
        const QPointF mid = trackCentre(n, 5);
        QVERIFY(std::abs(mid.x() - (0.35 + 0.7 + 0.5) / 3) < 1e-9);
        // Clearing one pattern leaves the other.
        clearTrack(n, 5, 2);
        QVERIFY(!n.params.values.value(nodekeys::patternKey(2, false)).isAnimated());
        QVERIFY(n.params.values.value(nodekeys::CenterX).isAnimated());
        clearTrack(n, 5, 0);
        QVERIFY(!n.params.values.value(nodekeys::CenterX).isAnimated());
        // Round trip.
        const Composition back
            = serialization::compositionFromJson(serialization::compositionToJson(c));
        QCOMPARE(int(std::lround(back.node(t)->params.value(nodekeys::Patterns, 0, 0))), 3);
        QCOMPARE(back.node(t)->params.staticValue(nodekeys::TrackRotation, false).toBool(), true);
    }

    void followTrackTakesSizeAndAngleFromTwoPatterns()
    {
        Composition c = co::defaultComposition();
        const quint64 t = co::addNode(c, nodetypes::Tracker, {});
        const quint64 xf = co::addNode(c, nodetypes::Transform, {});
        const quint64 merge = co::addNode(c, nodetypes::Merge, {});
        CompNode& tr = *c.node(t);
        tr.params.setBase(nodekeys::Patterns, 2);
        // Pattern 1 stays, pattern 2 moves from 60 px right of it to 120 px right and below:
        // the pair doubles in length (angle changes by atan2).
        std::vector<TrackSample> a, b;
        for (int f = 0; f <= 10; ++f) {
            a.push_back({f, 0.5, 0.5, 1.0});
            const double k = f / 10.0;
            b.push_back({f, 0.5 + 60.0 / kW, 0.5, 1.0});
            Q_UNUSED(k);
        }
        // Make pattern 2 go to (+85, +85) px (down-right): length 120.2, 45 degrees clockwise.
        for (int f = 0; f <= 10; ++f) {
            const double k = f / 10.0;
            b[size_t(f)].u = 0.5 + (60 + (85 - 60) * k) / kW;
            b[size_t(f)].v = 0.5 - (85 * k) / kH;
        }
        QVERIFY(storeTrack(tr, a, 0, 1));
        QVERIFY(storeTrack(tr, b, 0, 2));
        FollowOptions o;
        o.size = o.angle = true;
        for (const quint64 id : {xf, merge}) {
            CompNode& n = *c.node(id);
            QVERIFY(canFollowSize(n) && canFollowAngle(n));
            n.params.setBase(nodekeys::Angle, 5.0);
            QVERIFY(followTrack(n, tr, 0, kFrame, o));
            const double want = std::hypot(85.0, 85.0) / 60.0;
            QVERIFY(std::abs(n.params.value(nodekeys::Size, 10, 0) - want) < 1e-3);
            QVERIFY(std::abs(n.params.value(nodekeys::Angle, 10, 0) - (5.0 - 45.0)) < 1e-3);
            QVERIFY(std::abs(n.params.value(nodekeys::Size, 0, 0) - 1.0) < 1e-6);
        }
        // With a single pattern a Tracker has no turn or scale.
        tr.params.setBase(nodekeys::Patterns, 1);
        CompNode& other = *c.node(xf);
        QVERIFY(followTrack(other, tr, 0, kFrame, o));
        QVERIFY(std::abs(other.params.value(nodekeys::Size, 10, 0) - 1.0) < 1e-9);
    }
};

QTEST_MAIN(TrackerPatternsTest)
#include "test_trackerpatterns.moc"
