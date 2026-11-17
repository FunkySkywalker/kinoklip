// SPDX-License-Identifier: GPL-3.0-or-later
// Color page window tracking (spec section 107): a synthetic textured disc moves across a static
// textured background; a window on it is tracked forward and in reverse with the TrackerRunner and
// the track is merged into the node (WindowTrack), the window follows the disc, the track
// survives the project's JSON round trip, the grade render (the export's CPU reference) puts the
// window where the track says, and the maths of the tracked geometry inverts.
#include "compositing/tracking/WindowTrack.h"
#include "core/GradeMath.h"
#include "core/Serialization.h"
#include "render/GradeEval.h"

#include <QSignalSpy>
#include <QTest>

#include <cmath>
#include <future>
#include <map>
#include <random>

using namespace kk;
using namespace kk::tracking;
namespace go = kk::gradeops;
namespace gk = kk::gradekeys;
namespace wk = kk::gradewindowkeys;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

struct Wave
{
    double kx, ky, phase, amplitude;
};

const std::vector<Wave>& waves(unsigned seed)
{
    static std::map<unsigned, std::vector<Wave>> cache;
    auto it = cache.find(seed);
    if (it == cache.end()) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> wl(5.0, 20.0), dir(0.0, 2 * M_PI), ph(0.0, 2 * M_PI);
        std::vector<Wave> w;
        for (int i = 0; i < 30; ++i) {
            const double k = 2 * M_PI / wl(rng), a = dir(rng);
            w.push_back({k * std::cos(a), k * std::sin(a), ph(rng), 0.07});
        }
        it = cache.emplace(seed, std::move(w)).first;
    }
    return it->second;
}

double texture(unsigned seed, double x, double y)
{
    double v = 0.5;
    for (const Wave& w : waves(seed))
        v += w.amplitude * std::sin(w.kx * x + w.ky * y + w.phase);
    return v;
}

/// Disc centre at frame t (pixels) and its radius.
QPointF discCentre(int t)
{
    return {60.0 + 4.0 * t, 50.0 + 1.5 * t};
}
double discRadius(int t, bool grows)
{
    return grows ? 22.0 + 0.35 * t : 22.0;
}

QImage makeFrame(int t, bool grows)
{
    QImage img(kW, kH, QImage::Format_ARGB32_Premultiplied);
    const QPointF c = discCentre(t);
    const double r = discRadius(t, grows);
    const double s = r / 22.0;
    for (int y = 0; y < kH; ++y) {
        auto* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < kW; ++x) {
            double v;
            if (std::hypot(x - c.x(), y - c.y()) <= r)
                v = 0.25 + 1.8 * (texture(7, (x - c.x()) / s, (y - c.y()) / s) - 0.5) + 0.2;
            else
                v = 0.55 + (texture(3, x, y) - 0.5);
            const int g = std::clamp(int(v * 255.0), 0, 255);
            row[x] = qRgb(g, g, g);
        }
    }
    return img;
}

/// Frame source of the synthetic clip: grade frame f is picture f (timeline offset 0).
std::function<std::future<RenderedFramePtr>(int64_t)> source(bool grows)
{
    return [grows](int64_t f) {
        auto frame = std::make_shared<RenderedFrame>();
        frame->frame = f;
        frame->size = QSize(kW, kH);
        frame->ok = true;
        frame->image = makeFrame(int(f), grows);
        std::promise<RenderedFramePtr> p;
        p.set_value(frame);
        return p.get_future();
    };
}

GradeNode nodeWithWindow(QPointF centre, double w, double h, GradeWindowShape shape)
{
    ClipGrade g;
    const quint64 n = go::initialize(g);
    const quint64 win = go::addWindow(g, n, shape);
    g.window(win)->params.setBase(wk::CenterX, centre.x() / kW);
    g.window(win)->params.setBase(wk::CenterY, centre.y() / kH);
    g.window(win)->params.setBase(wk::Width, w / kW);
    g.window(win)->params.setBase(wk::Height, h / kH);
    return *g.node(n);
}

constexpr double kAspect = double(kW) / kH;

} // namespace

class TestWindowTrack : public QObject
{
    Q_OBJECT

    /// Runs @p plan on the synthetic clip and merges it into @p node's track.
    static bool run(GradeNode& node, const WindowTrackPlan& plan, bool grows)
    {
        TrackJob job = plan.job;
        job.source = source(grows);
        TrackerRunner runner;
        QSignalSpy done(&runner, &TrackerRunner::finished);
        if (!runner.start(job) || !done.wait(60000))
            return false;
        mergeSamples(node.track, plan, runner.samples());
        return runner.samples().size() >= 2;
    }

    /// Where window @p index of @p node is at @p frame, in pixels.
    static QPointF windowAt(const GradeNode& node, int frame)
    {
        const auto g = grademath::trackedWindowGeometry(node, node.windows[0], frame, kAspect);
        return {g.cx * kW, g.cy * kH};
    }

private slots:
    void forwardFollowsTheDisc()
    {
        GradeNode node = nodeWithWindow(discCentre(0), 60, 60, GradeWindowShape::Circle);
        QString why;
        const auto plan = planWindowTrack(node, node.windows[0], 0, 30, kAspect, &why);
        QVERIFY2(plan, qPrintable(why));
        QVERIFY(!plan->planar);
        QVERIFY(run(node, *plan, false));
        QVERIFY(node.track.hasData());
        QCOMPARE(node.track.firstFrame, int64_t(0));
        QCOMPARE(node.track.samples.size(), 31);
        for (const int t : {0, 5, 12, 30}) {
            const QPointF at = windowAt(node, t), want = discCentre(t);
            QVERIFY2(std::hypot(at.x() - want.x(), at.y() - want.y()) < 2.0,
                qPrintable(QStringLiteral("frame %1: %2,%3 want %4,%5")
                        .arg(t).arg(at.x()).arg(at.y()).arg(want.x()).arg(want.y())));
        }
        // Held at both ends.
        QCOMPARE(node.track.at(-5), node.track.at(0));
        QCOMPARE(node.track.at(99), node.track.at(30));
    }

    void reverseFollowsTheDisc()
    {
        // The window is put on the disc at the last frame; Track Reverse walks back.
        GradeNode node = nodeWithWindow(discCentre(30), 60, 60, GradeWindowShape::Circle);
        const auto plan = planWindowTrack(node, node.windows[0], 30, 0, kAspect, nullptr);
        QVERIFY(plan);
        QVERIFY(run(node, *plan, false));
        QCOMPARE(node.track.firstFrame, int64_t(0));
        QCOMPARE(node.track.samples.size(), 31);
        for (const int t : {30, 20, 7, 0}) {
            const QPointF at = windowAt(node, t), want = discCentre(t);
            QVERIFY2(std::hypot(at.x() - want.x(), at.y() - want.y()) < 2.0,
                qPrintable(QStringLiteral("frame %1").arg(t)));
        }
    }

    void secondRunContinuesTheTrack()
    {
        // Forward 0..15, then forward again from the playhead at 15 on the tracked window.
        GradeNode node = nodeWithWindow(discCentre(0), 60, 60, GradeWindowShape::Circle);
        auto plan = planWindowTrack(node, node.windows[0], 0, 15, kAspect, nullptr);
        QVERIFY(plan && run(node, *plan, false));
        QCOMPARE(node.track.lastFrame(), int64_t(15));
        plan = planWindowTrack(node, node.windows[0], 15, 30, kAspect, nullptr);
        QVERIFY(plan && plan->hadData);
        QVERIFY(run(node, *plan, false));
        QCOMPARE(node.track.firstFrame, int64_t(0));
        QCOMPARE(node.track.lastFrame(), int64_t(30));
        for (const int t : {0, 15, 22, 30}) {
            const QPointF at = windowAt(node, t), want = discCentre(t);
            QVERIFY2(std::hypot(at.x() - want.x(), at.y() - want.y()) < 2.5,
                qPrintable(QStringLiteral("frame %1").arg(t)));
        }
        // Reverse from the middle on the same node extends nothing but keeps the track whole.
        plan = planWindowTrack(node, node.windows[0], 15, 0, kAspect, nullptr);
        QVERIFY(plan && run(node, *plan, false));
        QCOMPARE(node.track.samples.size(), 31);
        QVERIFY(std::hypot(windowAt(node, 3).x() - discCentre(3).x(),
                    windowAt(node, 3).y() - discCentre(3).y())
            < 2.5);
    }

    void zoomFollowsTheGrowingDisc()
    {
        GradeNode node = nodeWithWindow(discCentre(0), 30, 30, GradeWindowShape::Linear);
        node.track.zoom = true;
        const auto plan = planWindowTrack(node, node.windows[0], 0, 20, kAspect, nullptr);
        QVERIFY(plan && plan->planar);
        QVERIFY(run(node, *plan, true));
        const QPointF at = windowAt(node, 20), want = discCentre(20);
        QVERIFY(std::hypot(at.x() - want.x(), at.y() - want.y()) < 3.0);
        const auto g = grademath::trackedWindowGeometry(node, node.windows[0], 20, kAspect);
        const double grown = g.width / (30.0 / kW); // the disc grew by (22 + 7) / 22 = 1.32
        QVERIFY2(grown > 1.15 && grown < 1.5, qPrintable(QString::number(grown)));
        // Zoom unchecked: the same data leaves the size alone.
        node.track.zoom = false;
        const auto fixed = grademath::trackedWindowGeometry(node, node.windows[0], 20, kAspect);
        QVERIFY(std::abs(fixed.width - 30.0 / kW) < 1e-9);
    }

    void checkboxesChooseWhatIsApplied()
    {
        GradeNode node = nodeWithWindow({100, 90}, 60, 60, GradeWindowShape::Circle);
        node.track.firstFrame = 0;
        node.track.pivot = QPointF(100.0 / kW, 0.5);
        node.track.zoom = node.track.rotate = true;
        node.track.samples = {{0.1, 0.2, 1.5, 1.5, 30.0}};
        const auto full = grademath::trackedWindowGeometry(node, node.windows[0], 0, kAspect);
        QCOMPARE(full.rotation, 30.0);
        QVERIFY(std::abs(full.width - 1.5 * 60.0 / kW) < 1e-9);
        node.track.pan = false;
        node.track.tilt = false;
        node.track.zoom = false;
        node.track.rotate = false;
        const auto none = grademath::trackedWindowGeometry(node, node.windows[0], 0, kAspect);
        QVERIFY(std::abs(none.cx - 100.0 / kW) < 1e-9 && std::abs(none.cy - 0.5) < 1e-9);
        QCOMPARE(none.rotation, 0.0);
        node.track.tilt = true;
        const auto tilt = grademath::trackedWindowGeometry(node, node.windows[0], 0, kAspect);
        QVERIFY(std::abs(tilt.cy - 0.7) < 1e-9 && std::abs(tilt.cx - 100.0 / kW) < 1e-9);
    }

    void geometryInverts()
    {
        GradeNode node = nodeWithWindow({150, 70}, 80, 50, GradeWindowShape::Linear);
        node.track.pivot = QPointF(0.4, 0.45);
        node.track.zoom = node.track.rotate = node.track.perspective = true;
        node.track.samples = {{0.05, -0.08, 1.4, 0.8, 37.0}, {0, 0, 1, 1, 0}};
        const auto shown = grademath::trackedWindowGeometry(node, node.windows[0], 0, kAspect);
        const auto back = grademath::untrackedWindowGeometry(node.track, shown, 0, kAspect);
        QVERIFY(std::abs(back.cx - 150.0 / kW) < 1e-9);
        QVERIFY(std::abs(back.cy - 70.0 / kH) < 1e-9);
        QVERIFY(std::abs(back.width - 80.0 / kW) < 1e-9);
        QVERIFY(std::abs(back.height - 50.0 / kH) < 1e-9);
        QVERIFY(std::abs(back.rotation) < 1e-9);
        // trackedWindow() bakes the shown values into constants.
        const GradeWindow baked = grademath::trackedWindow(node, node.windows[0], 0, kAspect);
        QVERIFY(std::abs(baked.value(wk::CenterX, 0) - shown.cx) < 1e-12);
        QVERIFY(std::abs(baked.value(wk::Rotation, 5) - shown.rotation) < 1e-12);
    }

    void serializationRoundTrip()
    {
        ClipGrade g;
        const quint64 n = go::initialize(g);
        go::addWindow(g, n, GradeWindowShape::Circle);
        GradeTrack& t = g.node(n)->track;
        t.firstFrame = 12;
        t.pivot = QPointF(0.31, 0.62);
        t.zoom = true;
        t.perspective = true;
        t.tilt = false;
        t.samples = {{0.01, 0.02, 1.0, 1.0, 0.0}, {0.125, -0.5, 1.25, 0.75, 33.5}};
        const ClipGrade back = serialization::gradeFromJson(serialization::gradeToJson(g));
        QCOMPARE(back, g);
        QCOMPARE(back.nodes[0].track.samples.size(), 2);
        QCOMPARE(back.nodes[0].track.firstFrame, int64_t(12));
        // A node without tracker data writes nothing for it.
        ClipGrade plain;
        go::initialize(plain);
        QVERIFY(!serialization::gradeToJson(plain)
                     .value(QLatin1String("nodes")).toArray()[0].toObject()
                     .contains(QLatin1String("track")));
        // shiftKeys moves the track with the keys.
        g.shiftKeys(-4);
        QCOMPARE(g.node(n)->track.firstFrame, int64_t(8));
    }

    void renderUsesTheTrackedPosition()
    {
        // The CPU reference render (what the export draws): a gain window brightens the picture
        // where the track puts it on each frame, not where it was drawn.
        ClipGrade g;
        const quint64 n = go::initialize(g);
        g.node(n)->params.setBase(gk::GainR, 1.8);
        g.node(n)->params.setBase(gk::GainG, 1.8);
        g.node(n)->params.setBase(gk::GainB, 1.8);
        const quint64 win = go::addWindow(g, n, GradeWindowShape::Circle);
        g.window(win)->params.setBase(wk::CenterX, 60.0 / kW);
        g.window(win)->params.setBase(wk::CenterY, 50.0 / kH);
        g.window(win)->params.setBase(wk::Width, 40.0 / kW);
        g.window(win)->params.setBase(wk::Height, 40.0 / kH);
        g.window(win)->params.setBase(wk::SoftOut, 0.01);
        QVector<GradeTrackSample> samples;
        for (int t = 0; t <= 30; ++t)
            samples.append({4.0 * t / kW, 1.5 * t / kH, 1.0, 1.0, 0.0});
        g.node(n)->track.samples = samples;
        g.node(n)->track.pivot = QPointF(60.0 / kW, 50.0 / kH);
        QImage base(kW, kH, QImage::Format_ARGB32_Premultiplied);
        base.fill(QColor(90, 90, 90));
        for (const int t : {0, 10, 30}) {
            QImage out = base.copy();
            render::applyGrade(g, t, out);
            const QPoint onWindow = discCentre(t).toPoint();
            const QPoint oldSpot(60, 50);
            QVERIFY2(qRed(out.pixel(onWindow)) > 140, qPrintable(QString::number(t)));
            if (t > 0)
                QCOMPARE(qRed(out.pixel(oldSpot)), 90);
        }
    }
};

QTEST_GUILESS_MAIN(TestWindowTrack)
#include "test_windowtrack.moc"
