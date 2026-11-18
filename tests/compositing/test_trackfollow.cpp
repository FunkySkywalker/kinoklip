// SPDX-License-Identifier: GPL-3.0-or-later
// The Mask Tracker and the Point Follower (spec section 117): a node follows a track with its
// position, size and angle or with the points of its shape, on the Compositing page and (a Match
// Move effect's track) on the Edit page; Roto Assist snaps a point to the nearest edge.
#include "../render/RenderTestUtil.h"
#include "compositing/RotoAssist.h"
#include "compositing/tracking/TrackFollow.h"
#include "compositing/tracking/TrackStore.h"
#include "core/CompOps.h"
#include "core/EffectTrack.h"
#include "core/MatchMove.h"
#include "core/NodeRegistry.h"
#include "core/VideoEffectKeys.h"
#include "render/CompMath.h"

#include <QImage>
#include <QPainter>
#include <QTest>

#include <cmath>

using namespace kk;
using namespace kk::test;
using namespace kk::tracking;
namespace co = kk::compops;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;
constexpr QSizeF kFrame(kW, kH);

/// A plane track: a square (centre 160, 90, side 80) turned by @p degrees clockwise on screen,
/// scaled by @p scale about its centre and moved by @p move px.
TrackSample squareAt(int64_t frame, double degrees, double scale, QPointF move)
{
    TrackSample s;
    s.frame = frame;
    s.planar = true;
    const QPointF c(160, 90);
    const double half = 40 * scale;
    const QPointF corner[4] = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
    QTransform t;
    t.rotate(degrees);
    for (int i = 0; i < 4; ++i) {
        const QPointF p = c + move + t.map(corner[i]);
        s.corners[size_t(2 * i)] = p.x() / kW;
        s.corners[size_t(2 * i + 1)] = 1.0 - p.y() / kH;
    }
    s.u = (c.x() + move.x()) / kW;
    s.v = 1.0 - (c.y() + move.y()) / kH;
    return s;
}

} // namespace

class TrackFollowTest : public QObject
{
    Q_OBJECT
private slots:
    void aMaskFollowsPositionSizeAndAngle()
    {
        Composition c = co::defaultComposition();
        const quint64 pt = co::addNode(c, nodetypes::PlanarTracker, {});
        const quint64 poly = co::addNode(c, nodetypes::PolygonMask, {});
        const quint64 rect = co::addNode(c, nodetypes::RectangleMask, {});
        QVERIFY(storeTrack(*c.node(pt),
            {squareAt(0, 0, 1, {0, 0}), squareAt(5, 15, 1.25, {20, -10}),
                squareAt(10, 30, 1.5, {40, -20})},
            0));
        // The polygon sits 50 px right of the plane's centre at frame 0.
        c.node(poly)->params.setBase(nodekeys::CenterX, (160 + 50) / double(kW));
        c.node(poly)->params.setBase(nodekeys::CenterY, 0.5);
        c.node(poly)->params.setBase(nodekeys::Size, 2.0);
        c.node(poly)->params.setBase(nodekeys::Angle, 10.0);
        FollowOptions o;
        o.size = o.angle = true;
        QVERIFY(followTrack(*c.node(poly), *c.node(pt), 0, kFrame, o));
        const ParamSet& p = c.node(poly)->params;
        QVERIFY(p.values.value(nodekeys::CenterX).isAnimated());
        QCOMPARE(p.values.value(nodekeys::Size).keys.size(), 3);
        // Frame 10: scale 1.5, a 30 degree clockwise turn: the polygon's size grows by 1.5, its
        // angle (counter-clockwise) drops by 30; its centre orbits the plane's centre.
        QVERIFY(std::abs(p.value(nodekeys::Size, 10, 0) - 3.0) < 1e-4);
        QVERIFY(std::abs(p.value(nodekeys::Angle, 10, 0) - (10.0 - 30.0)) < 1e-3);
        QVERIFY(std::abs(p.value(nodekeys::Size, 0, 0) - 2.0) < 1e-6);
        QVERIFY(std::abs(p.value(nodekeys::Angle, 0, 0) - 10.0) < 1e-6);
        QTransform t;
        t.translate(160 + 40, 90 - 20);
        t.rotate(30);
        t.scale(1.5, 1.5);
        t.translate(-160, -90);
        const QPointF want = t.map(QPointF(210, 90));
        QVERIFY(std::abs(p.value(nodekeys::CenterX, 10, 0) * kW - want.x()) < 0.01);
        QVERIFY(std::abs((1.0 - p.value(nodekeys::CenterY, 10, 0)) * kH - want.y()) < 0.01);
        // At the offset frame the node does not move.
        QVERIFY(std::abs(p.value(nodekeys::CenterX, 0, 0) * kW - 210) < 1e-6);

        // A rectangle mask: Size means Width and Height.
        c.node(rect)->params.setBase(nodekeys::Width, 0.2);
        c.node(rect)->params.setBase(nodekeys::Height, 0.1);
        QVERIFY(canFollowSize(*c.node(rect)) && canFollowAngle(*c.node(rect)));
        FollowOptions sizeOnly;
        sizeOnly.position = false;
        sizeOnly.size = true;
        QVERIFY(followTrack(*c.node(rect), *c.node(pt), 0, kFrame, sizeOnly));
        const ParamSet& r = c.node(rect)->params;
        QVERIFY(std::abs(r.value(nodekeys::Width, 10, 0) - 0.3) < 1e-4);
        QVERIFY(std::abs(r.value(nodekeys::Height, 10, 0) - 0.15) < 1e-4);
        QVERIFY(!r.values.value(nodekeys::CenterX).isAnimated()); // position not followed
        // Locked nodes and nodes without track are left alone.
        c.node(rect)->locked = true;
        QVERIFY(!followTrack(*c.node(rect), *c.node(pt), 0, kFrame, o));
        c.node(rect)->locked = false;
        const quint64 empty = co::addNode(c, nodetypes::PlanarTracker, {});
        QVERIFY(!followTrack(*c.node(rect), *c.node(empty), 0, kFrame, o));
    }

    void aPointTrackOnlyMoves()
    {
        Composition c = co::defaultComposition();
        const quint64 t = co::addNode(c, nodetypes::Tracker, {});
        const quint64 poly = co::addNode(c, nodetypes::PolygonMask, {});
        std::vector<TrackSample> samples;
        for (int f = 0; f <= 10; ++f)
            samples.push_back({f, 0.4 + 0.02 * f, 0.6 - 0.01 * f, 1.0});
        QVERIFY(storeTrack(*c.node(t), samples, 0));
        FollowOptions o;
        o.size = o.angle = true; // a single point has no turn or scale: they stay
        QVERIFY(followTrack(*c.node(poly), *c.node(t), 4, kFrame, o));
        const ParamSet& p = c.node(poly)->params;
        QVERIFY(std::abs(p.value(nodekeys::CenterX, 9, 0) - (0.5 + 0.02 * 5)) < 1e-9);
        QVERIFY(std::abs(p.value(nodekeys::Size, 9, 0) - 1.0) < 1e-9);
        QVERIFY(std::abs(p.value(nodekeys::Angle, 9, 0)) < 1e-9);
    }

    void thePointsOfAShapeFollow()
    {
        Composition c = co::defaultComposition();
        const quint64 pt = co::addNode(c, nodetypes::PlanarTracker, {});
        const quint64 bz = co::addNode(c, nodetypes::BezierMask, {});
        QVERIFY(storeTrack(*c.node(pt),
            {squareAt(0, 0, 1, {0, 0}), squareAt(10, 90, 1, {30, 10})}, 0));
        QVERIFY(hasPathPoints(*c.node(bz)));
        CompNode& n = *c.node(bz);
        n.params.setBase(nodekeys::Size, 0.5);
        n.params.setBase(nodekeys::Angle, 20);
        const MaskPath before = n.path.at(0);
        const double cx0 = n.params.value(nodekeys::CenterX, 0, 0.5);
        FollowOptions o;
        o.position = o.angle = true;
        o.points = true;
        QVERIFY(followTrack(n, *c.node(pt), 0, kFrame, o));
        // The node's own controls stay; the shape is keyframed.
        QVERIFY(!n.params.values.value(nodekeys::CenterX).isAnimated());
        QVERIFY(n.path.isAnimated());
        QCOMPARE(n.path.shapes.size(), 2);
        QCOMPARE(n.params.value(nodekeys::CenterX, 0, 0.5), cx0);
        const MaskPath start = n.path.at(0);
        QCOMPARE(start.size(), before.size());
        for (int i = 0; i < start.size(); ++i) {
            QVERIFY(std::abs(start[i].x - before[i].x) < 1e-6);
            QVERIFY(std::abs(start[i].outY - before[i].outY) < 1e-6);
        }
        // Frame 10: the plane turned 90 degrees clockwise about its centre (160, 90) and moved
        // by (30, 10). Where did point 0 go on the picture, and where does the shape show it?
        const MaskPath end = n.path.at(10);
        const auto toPicture = [&](const MaskPath& path, int i) {
            QTransform l;
            l.translate(0.5 * kW, 0.5 * kH);
            l.rotate(-20);
            l.scale(0.5, 0.5);
            return l.map(path[i].pos());
        };
        QTransform plane;
        plane.translate(160 + 30, 90 + 10);
        plane.rotate(90);
        plane.translate(-160, -90);
        for (int i = 0; i < start.size(); ++i) {
            const QPointF want = plane.map(toPicture(start, i));
            const QPointF got = toPicture(end, i);
            QVERIFY2(std::abs(want.x() - got.x()) < 0.05 && std::abs(want.y() - got.y()) < 0.05,
                qPrintable(QString::number(i)));
        }
        // The tangents turned with the points: still a closed smooth curve.
        QVERIFY(end[0].hasTangents());
        QVERIFY(std::abs(std::hypot(end[0].outX, end[0].outY)
                    - std::hypot(start[0].outX, start[0].outY))
            < 1e-6);
        QVERIFY(std::abs(std::hypot(end[0].outX, end[0].outY)
                    - std::hypot(start[0].outX, start[0].outY))
            < 1e-6);
    }

    void thePointFollowerMovesOnlyThePickedPoints()
    {
        Composition c = co::defaultComposition();
        const quint64 t = co::addNode(c, nodetypes::Tracker, {});
        const quint64 poly = co::addNode(c, nodetypes::PolygonMask, {});
        std::vector<TrackSample> samples;
        for (int f = 0; f <= 10; ++f)
            samples.push_back({f, 0.5 + 0.01 * f, 0.5 - 0.02 * f, 1.0});
        QVERIFY(storeTrack(*c.node(t), samples, 0));
        CompNode& n = *c.node(poly);
        n.params.setBase(nodekeys::Size, 2.0);
        const MaskPath start = n.path.at(0);
        QVERIFY(!followPoints(n, {}, *c.node(t), 0, kFrame));
        QVERIFY(!followPoints(n, {99}, *c.node(t), 0, kFrame));
        QVERIFY(followPoints(n, {1, 3}, *c.node(t), 0, kFrame));
        const MaskPath end = n.path.at(10);
        // +0.1 of the width to the right and 0.2 of the height down (track v falls), in
        // mask-local px of a mask at Size 2: half of the picture pixels.
        for (int i = 0; i < start.size(); ++i) {
            const bool moved = i == 1 || i == 3;
            const double dx = moved ? 0.1 * kW / 2.0 : 0.0, dy = moved ? 0.2 * kH / 2.0 : 0.0;
            QVERIFY2(std::abs(end[i].x - (start[i].x + dx)) < 1e-6, qPrintable(QString::number(i)));
            QVERIFY2(std::abs(end[i].y - (start[i].y + dy)) < 1e-6, qPrintable(QString::number(i)));
        }
        // Halfway the points are halfway.
        QVERIFY(std::abs(n.path.at(5)[1].x - (start[1].x + 0.05 * kW / 2.0)) < 1e-6);
    }

    void editPageMaskFollowsAMatchMove()
    {
        // A clip with a Match Move whose point moves 40 px right and 20 px down over 10 frames.
        TimelineData tl = emptyTimeline(kW, kH);
        ClipData clip = solidClip(tl, QColor(0, 0, 0), 0, 11);
        EffectInstance fx;
        fx.id = tl.allocateId();
        fx.type = QLatin1String(fxid::MatchMove);
        std::vector<double> seconds;
        std::vector<float> values;
        for (int f = 0; f <= 10; ++f) {
            seconds.push_back(double(f) / 25.0);
            values.push_back(float((100.0 + 4 * f) / kW));
            values.push_back(float(1.0 - (60.0 + 2 * f) / kH));
        }
        const efftrack::Table table(
            matchmove::signature(fx.params), 0.0, matchmove::kPointStride, seconds, values);
        fx.params.setStatic(fxkeys::TrackData, efftrack::encode(table));
        clip.effects.push_back(fx);
        QVERIFY(matchmove::isActive(clip.effects.front()));

        MaskData mask = makeMask(MaskShape::Rectangle, 1, QSizeF(60, 40));
        mask.params.setBase(maskkeys::PosX, -20);
        mask.params.setBase(maskkeys::PosY, 5);
        FollowOptions o;
        QVERIFY(followMatchMove(mask, clip, clip.effects.front(), 0, kFrame, tl.fps, o));
        QVERIFY(mask.params.values.value(maskkeys::PosX).isAnimated());
        QCOMPARE(mask.params.values.value(maskkeys::PosX).keys.size(), 11);
        QVERIFY(std::abs(mask.value(maskkeys::PosX, 0) - -20) < 1e-3);
        QVERIFY(std::abs(mask.value(maskkeys::PosX, 10) - (-20 + 40)) < 1e-3);
        QVERIFY(std::abs(mask.value(maskkeys::PosY, 10) - (5 + 20)) < 1e-3);
        // Keeps its place where it was put: the same mask put at frame 4.
        MaskData later = makeMask(MaskShape::Rectangle, 2, QSizeF(60, 40));
        later.params.setBase(maskkeys::PosX, 10);
        QVERIFY(followMatchMove(later, clip, clip.effects.front(), 4, kFrame, tl.fps, o));
        QVERIFY(std::abs(later.value(maskkeys::PosX, 4) - 10) < 1e-3);
        QVERIFY(std::abs(later.value(maskkeys::PosX, 10) - (10 + 24)) < 1e-3);
        // Not a Match Move, or no track: refused.
        EffectInstance other = clip.effects.front();
        other.type = QStringLiteral("blur");
        QVERIFY(!followMatchMove(mask, clip, other, 0, kFrame, tl.fps, o));
        EffectInstance noTrack = clip.effects.front();
        noTrack.params.setStatic(fxkeys::TrackData, QString());
        QVERIFY(!followMatchMove(mask, clip, noTrack, 0, kFrame, tl.fps, o));
        // Polygon points on the Edit page.
        MaskData poly = makeMask(MaskShape::Polygon, 3, QSizeF(60, 40));
        poly.path.shapes = {MaskPath {MaskPoint {-10, -10}, MaskPoint {10, -10}, MaskPoint {0, 10}}};
        FollowOptions pts;
        pts.points = true;
        QVERIFY(followMatchMove(poly, clip, clip.effects.front(), 0, kFrame, tl.fps, pts));
        QVERIFY(poly.path.isAnimated());
        QVERIFY(std::abs(poly.path.at(10)[0].x - (-10 + 40)) < 1e-3);
        QVERIFY(std::abs(poly.path.at(10)[0].y - (-10 + 20)) < 1e-3);
        QVERIFY(!poly.params.values.value(maskkeys::PosX).isAnimated());
        QVERIFY(!followMatchMove(mask, clip, clip.effects.front(), 0, kFrame, tl.fps,
            [] { FollowOptions p; p.points = true; return p; }())); // a rectangle has no points
    }

    void theBezierNodeIsAMaskWithTangents()
    {
        Composition c = co::defaultComposition();
        const quint64 bz = co::addNode(c, nodetypes::BezierMask, {});
        const CompNode& n = *c.node(bz);
        QCOMPARE(n.path.pointCount(), 4);
        const MaskData m = render::compmath::maskOf(n, 0, kFrame);
        QCOMPARE(m.shape, MaskShape::Bezier);
        QVERIFY(m.path.at(0)[0].hasTangents());
        QVERIFY(m.isRenderable());
        const NodeDescriptor* d = NodeRegistry::instance().find(nodetypes::BezierMask);
        QVERIFY(d && d->category == NodeCategory::Mask && d->param(nodekeys::RotoAssist));
        // The curve bulges past the polygon of its points (a circle, not a diamond): the
        // flattened curve reaches 216 px from the centre between two points.
        QVector<QPointF> flat;
        flattenPath(m.path.at(0), true, 0.2, [](QPointF p) { return p; }, flat);
        double minR = 1e9, maxR = 0;
        for (const QPointF& p : flat) {
            minR = std::min(minR, std::hypot(p.x(), p.y()));
            maxR = std::max(maxR, std::hypot(p.x(), p.y()));
        }
        QVERIFY(minR > 210 && maxR < 222);
    }

    void rotoAssistSnapsToTheNearestEdge()
    {
        // A white square on black: the edges are at x = 40 and x = 80, y = 30 and y = 70.
        QImage image(120, 100, QImage::Format_RGB32);
        image.fill(Qt::black);
        QPainter p(&image);
        p.fillRect(QRect(40, 30, 40, 40), Qt::white);
        p.end();
        const auto snapped = roto::snapToEdge(image, QPointF(36, 50), 8);
        QVERIFY(snapped);
        QVERIFY(std::abs(snapped->x() - 40) <= 1.6);
        // A corner: both coordinates snap (the strongest edge is nearer to the corner pixel).
        const auto onRight = roto::snapToEdge(image, QPointF(84, 50), 8);
        QVERIFY(onRight && std::abs(onRight->x() - 80) <= 1.6);
        // Nothing within reach: stays (nullopt).
        QVERIFY(!roto::snapToEdge(image, QPointF(10, 10), 5));
        QVERIFY(!roto::snapToEdge(QImage(), QPointF(10, 10), 5));
        // Flat picture: no edge.
        QImage flat(50, 50, QImage::Format_RGB32);
        flat.fill(Qt::gray);
        QVERIFY(!roto::snapToEdge(flat, QPointF(25, 25), 10));
    }
};

QTEST_MAIN(TrackFollowTest)
#include "test_trackfollow.moc"
