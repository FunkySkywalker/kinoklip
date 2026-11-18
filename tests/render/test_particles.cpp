// SPDX-License-Identifier: GPL-3.0-or-later
// Spec 129: the particle nodes of the Compositing page (pEmitter, pGravity, pTurbulence, pMerge,
// pRender). The simulation is tested as pure functions; pRender's pixels on the GPU and the CPU.
#include "RenderTestUtil.h"
#include "core/CompOps.h"
#include "core/EditOps.h"
#include "core/NodeRegistry.h"
#include "core/Serialization.h"
#include "render/CompParticles.h"
#include "render/Compositor.h"
#include "render/TimelineEval.h"

#include <QElapsedTimer>
#include <QTest>

#include <cmath>
#include <memory>

using namespace kk;
using namespace kk::test;
namespace co = kk::compops;
namespace pt = kk::render::particles;
namespace k = kk::nodekeys;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;
constexpr double kAspect = double(kH) / kW;

/// A calm emitter: @p number particles per frame, living @p life frames, flying nowhere.
quint64 addEmitter(Composition& c, double number, double life, double velocity = 0.0)
{
    const quint64 id = co::addNode(c, nodetypes::PEmitter, {});
    ParamSet& p = c.node(id)->params;
    p.setBase(k::PNumber, number);
    p.setBase(k::PLife, life);
    p.setBase(k::PVelocity, velocity);
    p.setBase(k::PAngleVar, 0);
    return id;
}

quint64 addRender(Composition& c, quint64 stream)
{
    const quint64 id = co::addNode(c, nodetypes::PRender, {});
    co::connect(c, stream, 0, id, nodeports::Input);
    return id;
}

QVector<pt::Particle> at(const Composition& c, quint64 stream, int64_t frame)
{
    return pt::streamAt(c, stream, frame, kAspect);
}

bool same(const QVector<pt::Particle>& a, const QVector<pt::Particle>& b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].size != b[i].size
            || a[i].rotation != b[i].rotation || a[i].opacity != b[i].opacity
            || a[i].r != b[i].r || a[i].emitter != b[i].emitter)
            return false;
    }
    return true;
}

TimelineData compTimeline(const Composition& comp)
{
    TimelineData tl = emptyTimeline(kW, kH);
    ClipData c = solidClip(tl, QColor(0, 0, 0), 0, 50);
    c.composition = comp;
    addClip(tl, TrackType::Video, 0, c);
    return tl;
}

} // namespace

class TestParticles : public QObject
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

    // ---- Port types ----

    void particleStreamsConnectOnlyToParticleInputs()
    {
        Composition c = co::defaultComposition();
        const quint64 em = addEmitter(c, 1, 10);
        const quint64 gr = co::addNode(c, nodetypes::PGravity, {});
        const quint64 mg = co::addNode(c, nodetypes::PMerge, {});
        const quint64 rn = co::addNode(c, nodetypes::PRender, {});
        const quint64 bg = co::addNode(c, nodetypes::Background, {});
        QCOMPARE(int(co::connect(c, em, 0, gr, nodeports::Input)), int(co::ConnectError::None));
        QCOMPARE(int(co::connect(c, gr, 0, mg, nodeports::Input2)), int(co::ConnectError::None));
        QCOMPARE(int(co::connect(c, mg, 0, rn, nodeports::Input)), int(co::ConnectError::None));
        // A particle stream into a picture input, a picture into a particle input: refused.
        QCOMPARE(int(co::connect(c, em, 0, c.mediaOutId, nodeports::Input)),
            int(co::ConnectError::WrongKind));
        QCOMPARE(int(co::connect(c, bg, 0, rn, nodeports::Input)), int(co::ConnectError::WrongKind));
        QCOMPARE(int(co::connect(c, bg, 0, gr, nodeports::Input)), int(co::ConnectError::WrongKind));
        QCOMPARE(int(co::connect(c, rn, 0, gr, nodeports::Input)), int(co::ConnectError::WrongKind));
        QCOMPARE(int(co::connect(c, em, 0, rn, nodeports::Mask)), int(co::ConnectError::UnknownPort));
        // pRender's picture goes on into the picture graph, and a picture feeds a Bitmap Sprite.
        QCOMPARE(int(co::connect(c, rn, 0, c.mediaOutId, nodeports::Input)),
            int(co::ConnectError::None));
        QCOMPARE(int(co::connect(c, bg, 0, em, nodeports::Sprite)), int(co::ConnectError::None));
        QVERIFY(!co::kindsCompatible(PortKind::Particles, PortKind::Image));
        QVERIFY(!co::kindsCompatible(PortKind::Image, PortKind::Particles));
        QVERIFY(!co::kindsCompatible(PortKind::Particles, PortKind::Mask));
        QVERIFY(co::kindsCompatible(PortKind::Particles, PortKind::Particles));
    }

    void addingParticleNodesFromTheToolbar()
    {
        Composition c = co::defaultComposition();
        const co::AddResult e = co::addNodeAfter(c, nodetypes::PEmitter, c.mediaOutId);
        QVERIFY(e.node != 0);
        QCOMPARE(e.merge, quint64(0)); // no Merge is made for a particle source
        const co::AddResult g = co::addNodeAfter(c, nodetypes::PGravity, e.node);
        QCOMPARE(c.node(g.node)->inputs.value(nodeports::Input).nodeId, e.node);
        const co::AddResult r = co::addNodeAfter(c, nodetypes::PRender, g.node);
        QCOMPARE(c.node(r.node)->inputs.value(nodeports::Input).nodeId, g.node);
    }

    void everyControlHasATooltip()
    {
        for (const QString& type : {nodetypes::PEmitter, nodetypes::PGravity, nodetypes::PTurbulence,
                 nodetypes::PMerge, nodetypes::PRender}) {
            const NodeDescriptor* d = NodeRegistry::instance().find(type);
            QVERIFY(d);
            QVERIFY(!d->description.isEmpty());
            for (const ParamDescriptor& p : d->params)
                QVERIFY2(!p.tooltip.isEmpty(), qPrintable(type + "/" + p.key));
        }
    }

    // ---- Emitter maths ----

    void emitterCountAndLifespan()
    {
        Composition c = co::defaultComposition();
        const quint64 em = addEmitter(c, 10, 20);
        QCOMPARE(at(c, em, -1).size(), 0);
        QCOMPARE(at(c, em, 0).size(), 10);
        QCOMPARE(at(c, em, 5).size(), 60); // frames 0..5 each made 10
        QCOMPARE(at(c, em, 19).size(), 200); // a particle lives Lifespan frames: ages 0..19
        QCOMPARE(at(c, em, 20).size(), 200); // the first batch is gone, the 21st batch is born
        QCOMPARE(at(c, em, 500).size(), 200);
        // Start and End frames.
        c.node(em)->params.setBase(k::PStart, 3);
        c.node(em)->params.setBase(k::PEnd, 7);
        QCOMPARE(at(c, em, 2).size(), 0);
        QCOMPARE(at(c, em, 3).size(), 10);
        QCOMPARE(at(c, em, 7).size(), 50);
        QCOMPARE(at(c, em, 26).size(), 10); // only the batch of frame 7 is left
        QCOMPARE(at(c, em, 27).size(), 0);
    }

    void numberAndLifespanVarianceStayInRange()
    {
        Composition c = co::defaultComposition();
        const quint64 em = addEmitter(c, 10, 20);
        c.node(em)->params.setBase(k::PNumberVar, 4);
        c.node(em)->params.setBase(k::PLifeVar, 10);
        int minBorn = 1000, maxBorn = 0;
        // Particles of the newest frame all have age 0: count them as the batch size.
        for (int f = 0; f < 60; ++f) {
            const QVector<pt::Particle> now = at(c, em, f);
            // Batch of frame f: the particles that do not exist one frame earlier at all
            // cannot be told apart, so use the emitter alone at frame f (Start = End = f).
            Composition one = c;
            one.node(em)->params.setBase(k::PStart, f);
            one.node(em)->params.setBase(k::PEnd, f);
            const int born = at(one, em, f).size();
            minBorn = std::min(minBorn, born);
            maxBorn = std::max(maxBorn, born);
            QVERIFY(now.size() >= born);
        }
        QVERIFY(minBorn >= 6 && maxBorn <= 14);
        QVERIFY(minBorn < maxBorn); // it does vary
        // A lifespan of 20 +- 10 frames: nothing older than 30 frames is left.
        Composition one = c;
        one.node(em)->params.setBase(k::PStart, 0);
        one.node(em)->params.setBase(k::PEnd, 0);
        QVERIFY(at(one, em, 29).size() >= 0);
        QCOMPARE(at(one, em, 30).size(), 0);
        QVERIFY(at(one, em, 5).size() > at(one, em, 25).size());
    }

    void regionsPlaceTheBirths()
    {
        Composition c = co::defaultComposition();
        const quint64 em = addEmitter(c, 200, 5);
        ParamSet& p = c.node(em)->params;
        p.setBase(k::CenterX, 0.3);
        p.setBase(k::CenterY, 0.6);
        p.setBase(k::Width, 0.2);
        p.setBase(k::Height, 0.1);
        const auto spread = [&](const QString& region, double* minX, double* maxX, double* minY,
                                double* maxY) {
            p.setStatic(k::PRegion, region);
            *minX = *minY = 10;
            *maxX = *maxY = -10;
            for (const pt::Particle& q : at(c, em, 0)) {
                *minX = std::min<double>(*minX, q.x);
                *maxX = std::max<double>(*maxX, q.x);
                *minY = std::min<double>(*minY, q.y);
                *maxY = std::max<double>(*maxY, q.y);
            }
        };
        double x0, x1, y0, y1;
        const double cy = 0.6 * kAspect;
        spread(particleregions::Point, &x0, &x1, &y0, &y1);
        QCOMPARE(x0, x1);
        QVERIFY(qAbs(x0 - 0.3) < 1e-6 && qAbs(y0 - cy) < 1e-6);
        spread(particleregions::Line, &x0, &x1, &y0, &y1);
        QVERIFY(x0 >= 0.2 - 1e-6 && x1 <= 0.4 + 1e-6 && x1 - x0 > 0.15);
        QCOMPARE(y0, y1);
        spread(particleregions::Rectangle, &x0, &x1, &y0, &y1);
        QVERIFY(x0 >= 0.2 - 1e-6 && x1 <= 0.4 + 1e-6 && y0 >= cy - 0.05 - 1e-6 && y1 <= cy + 0.05 + 1e-6);
        QVERIFY(y1 - y0 > 0.07);
        spread(particleregions::Circle, &x0, &x1, &y0, &y1);
        QVERIFY(x0 >= 0.2 - 1e-6 && x1 <= 0.4 + 1e-6 && y0 >= cy - 0.1 - 1e-6 && y1 <= cy + 0.1 + 1e-6);
        for (const pt::Particle& q : at(c, em, 0))
            QVERIFY(std::hypot(q.x - 0.3, q.y - cy) <= 0.1 + 1e-6);
    }

    void overLifeValuesFollowTheAge()
    {
        Composition c = co::defaultComposition();
        const quint64 em = addEmitter(c, 1, 10, 0.01);
        ParamSet& p = c.node(em)->params;
        p.setBase(k::PSize, 0.1);
        p.setBase(k::PSizeEnd, 0.05);
        p.setBase(k::POpacity, 1.0);
        p.setBase(k::POpacityEnd, 0.0);
        p.setStatic(k::Color, QColor(255, 0, 0));
        p.setStatic(k::PColorEnd, QColor(0, 0, 255));
        p.setBase(k::Angle, 0);
        p.setBase(k::PSpin, 10);
        const QVector<pt::Particle> now = at(c, em, 5); // ages 0..5 of a 10 frame life
        QCOMPARE(now.size(), 6);
        const pt::Particle& oldest = now.first(); // age 5: halfway
        QVERIFY(qAbs(oldest.size - 0.075) < 1e-6);
        QVERIFY(qAbs(oldest.opacity - 0.5) < 1e-6);
        QVERIFY(qAbs(oldest.r - 0.5) < 1e-3 && qAbs(oldest.b - 0.5) < 1e-3);
        QVERIFY(qAbs(oldest.rotation - 50.0) < 1e-4);
        QVERIFY(qAbs(oldest.x - (0.5 + 5 * 0.01)) < 1e-5); // flew 5 frames to the right
        QVERIFY(qAbs(now.last().size - 0.1) < 1e-6); // the newest is at birth
    }

    // ---- Determinism ----

    void sameSeedAndFrameGiveTheSameParticles()
    {
        Composition c = co::defaultComposition();
        const quint64 em = addEmitter(c, 7.5, 30, 0.01);
        ParamSet& p = c.node(em)->params;
        p.setBase(k::PNumberVar, 3);
        p.setBase(k::PLifeVar, 8);
        p.setBase(k::PAngleVar, 40);
        p.setBase(k::PVelocityVar, 0.004);
        p.setBase(k::PSizeVar, 0.3);
        p.setStatic(k::PRegion, particleregions::Circle);
        const quint64 tb = co::addNode(c, nodetypes::PTurbulence, {});
        const quint64 gr = co::addNode(c, nodetypes::PGravity, {});
        co::connect(c, em, 0, tb, nodeports::Input);
        co::connect(c, tb, 0, gr, nodeports::Input);
        const QVector<pt::Particle> a = at(c, gr, 40);
        QVERIFY(a.size() > 100);
        QVERIFY(same(a, at(c, gr, 40)));
        // Playing up to the frame, then jumping back and forward, gives the same particles.
        for (int f = 0; f <= 40; ++f)
            (void)at(c, gr, f);
        (void)at(c, gr, 3);
        QVERIFY(same(a, at(c, gr, 40)));
        // Another seed gives other particles; so does another frame.
        Composition d = c;
        d.node(em)->params.setBase(k::Seed, 1);
        QVERIFY(!same(a, at(d, gr, 40)));
        QVERIFY(!same(a, at(c, gr, 41)));
        // The size of the composition does not change the stream, only its aspect does.
        QVERIFY(same(at(c, gr, 12), pt::streamAt(c, gr, 12, kAspect)));
    }

    void parametersOfTheBirthFrameAreUsed()
    {
        // Number is keyframed: 0 until frame 10, then 4 per frame.
        Composition c = co::defaultComposition();
        const quint64 em = addEmitter(c, 0, 100);
        AnimatedValue& v = c.node(em)->params.animated(k::PNumber, 0.0);
        Keyframe a;
        a.frame = 9;
        a.value = 0;
        a.hold = true;
        Keyframe b;
        b.frame = 10;
        b.value = 4;
        b.hold = true;
        v.keys = {a, b};
        QCOMPARE(at(c, em, 9).size(), 0);
        QCOMPARE(at(c, em, 10).size(), 4);
        QCOMPARE(at(c, em, 14).size(), 20);
    }

    // ---- Forces ----

    void gravityMovesParticlesTheExpectedDistance()
    {
        Composition c = co::defaultComposition();
        const quint64 em = addEmitter(c, 1, 100);
        c.node(em)->params.setBase(k::PStart, 0);
        c.node(em)->params.setBase(k::PEnd, 0);
        const quint64 gr = co::addNode(c, nodetypes::PGravity, {});
        co::connect(c, em, 0, gr, nodeports::Input);
        c.node(gr)->params.setBase(k::PStrength, 0.001);
        c.node(gr)->params.setBase(k::Angle, 270);
        const double y0 = 0.5 * kAspect;
        for (int n : {0, 1, 10, 40}) {
            const QVector<pt::Particle> now = at(c, gr, n);
            QCOMPARE(now.size(), 1);
            // The speed grows by the strength every frame: n frames move it g n (n + 1) / 2.
            QVERIFY(qAbs(now[0].y - (y0 - 0.001 * n * (n + 1) / 2.0)) < 1e-5);
            QVERIFY(qAbs(now[0].x - 0.5) < 1e-6);
        }
        // Sideways, and with an initial speed on top.
        c.node(gr)->params.setBase(k::Angle, 0);
        c.node(em)->params.setBase(k::PVelocity, 0.002);
        c.node(em)->params.setBase(k::Angle, 90);
        const pt::Particle p = at(c, gr, 10)[0];
        QVERIFY(qAbs(p.x - (0.5 + 0.001 * 55)) < 1e-5);
        QVERIFY(qAbs(p.y - (y0 + 0.002 * 10)) < 1e-5);
        // Inactive: no force.
        c.node(gr)->params.setStatic(k::PActive, false);
        QVERIFY(qAbs(at(c, gr, 10)[0].x - 0.5) < 1e-6);
        // Disabled: the stream passes through unchanged.
        c.node(gr)->params.setStatic(k::PActive, true);
        c.node(gr)->enabled = false;
        QVERIFY(qAbs(at(c, gr, 10)[0].x - 0.5) < 1e-6);
    }

    void turbulenceIsBoundedAndDeterministic()
    {
        Composition c = co::defaultComposition();
        const quint64 em = addEmitter(c, 3, 100);
        const quint64 tb = co::addNode(c, nodetypes::PTurbulence, {});
        co::connect(c, em, 0, tb, nodeports::Input);
        ParamSet& p = c.node(tb)->params;
        p.setBase(k::PStrength, 0.002);
        p.setBase(k::PDensity, 6);
        const int frame = 30;
        const QVector<pt::Particle> a = at(c, tb, frame);
        QVERIFY(same(a, at(c, tb, frame)));
        // One step changes the speed by at most strength per axis; n steps move at most
        // strength * n (n + 1) / 2 per axis.
        const double y0 = 0.5 * kAspect;
        double farthest = 0;
        for (const pt::Particle& q : a) {
            const double bound = 0.002 * frame * (frame + 1) / 2.0;
            QVERIFY(std::abs(q.x - 0.5) <= bound + 1e-6);
            QVERIFY(std::abs(q.y - y0) <= bound + 1e-6);
            farthest = std::max(farthest, std::hypot(q.x - 0.5, q.y - y0));
        }
        QVERIFY(farthest > 1e-4); // it does move them
        // Another seed gives another noise; strength 0 moves nothing; same seed repeats.
        Composition d = c;
        d.node(tb)->params.setBase(k::Seed, 5);
        QVERIFY(!same(a, at(d, tb, frame)));
        d = c;
        d.node(tb)->params.setBase(k::PStrength, 0);
        for (const pt::Particle& q : at(d, tb, frame))
            QVERIFY(std::abs(q.x - 0.5) < 1e-6 && std::abs(q.y - y0) < 1e-6);
    }

    void aForceOnlyActsOnTheStreamsThatPassItsInput()
    {
        Composition c = co::defaultComposition();
        const quint64 a = addEmitter(c, 2, 50);
        const quint64 b = addEmitter(c, 3, 50);
        c.node(b)->params.setBase(k::CenterX, 0.8);
        const quint64 gr = co::addNode(c, nodetypes::PGravity, {});
        const quint64 mg = co::addNode(c, nodetypes::PMerge, {});
        co::connect(c, a, 0, gr, nodeports::Input);
        co::connect(c, gr, 0, mg, nodeports::Input);
        co::connect(c, b, 0, mg, nodeports::Input2);
        c.node(gr)->params.setBase(k::PStrength, 0.001);
        // pMerge counts: both streams, a's through the gravity.
        QCOMPARE(at(c, mg, 9).size(), (2 + 3) * 10);
        QCOMPARE(at(c, a, 9).size() + at(c, b, 9).size(), at(c, mg, 9).size());
        int falling = 0, still = 0;
        for (const pt::Particle& q : at(c, mg, 9)) {
            if (q.emitter == a)
                falling += q.y < 0.5 * kAspect - 1e-6;
            else
                still += std::abs(q.y - 0.5 * kAspect) < 1e-6;
        }
        QCOMPARE(falling, 18); // the 10 batches except the one born now (no age, no fall)
        QCOMPARE(still, 30);
        // A disabled pMerge passes only its first input; an unconnected one is that input alone.
        c.node(mg)->enabled = false;
        QCOMPARE(at(c, mg, 9).size(), 2 * 10);
        c.node(mg)->enabled = true;
        c.node(mg)->inputs.remove(nodeports::Input2);
        QCOMPARE(at(c, mg, 9).size(), 2 * 10);
        // A force after the merge acts on both streams.
        Composition d = co::defaultComposition();
        const quint64 e1 = addEmitter(d, 1, 50);
        const quint64 e2 = addEmitter(d, 1, 50);
        const quint64 m2 = co::addNode(d, nodetypes::PMerge, {});
        const quint64 g2 = co::addNode(d, nodetypes::PGravity, {});
        co::connect(d, e1, 0, m2, nodeports::Input);
        co::connect(d, e2, 0, m2, nodeports::Input2);
        co::connect(d, m2, 0, g2, nodeports::Input);
        d.node(g2)->params.setBase(k::PStrength, 0.001);
        const QVector<pt::Particle> both = at(d, g2, 5);
        QCOMPARE(both.size(), 12);
        int moved = 0;
        for (const pt::Particle& q : both)
            moved += q.y < 0.5 * kAspect - 1e-6;
        QCOMPARE(moved, 10);
    }

    void aThousandsOfParticlesAreFastEnough()
    {
        Composition c = co::defaultComposition();
        const quint64 em = addEmitter(c, 50, 60, 0.005);
        c.node(em)->params.setBase(k::PAngleVar, 60);
        const quint64 tb = co::addNode(c, nodetypes::PTurbulence, {});
        const quint64 gr = co::addNode(c, nodetypes::PGravity, {});
        co::connect(c, em, 0, tb, nodeports::Input);
        co::connect(c, tb, 0, gr, nodeports::Input);
        const quint64 rn = addRender(c, gr);
        QElapsedTimer t;
        t.start();
        const QImage image = pt::renderNode(c, *c.node(rn), 200, QSize(1920, 1080), {});
        const qint64 ms = t.elapsed();
        qInfo() << "3000 particles with two forces at 1080p:" << ms << "ms";
        QCOMPARE(at(c, gr, 200).size(), 3000);
        QVERIFY2(ms < 3000, qPrintable(QString::number(ms)));
        QVERIFY(image.pixelColor(960, 540).alpha() >= 0);
    }

    // ---- pRender ----

    void pRenderDrawsAPointOnBothRenderers()
    {
        Composition c = co::defaultComposition();
        const quint64 em = addEmitter(c, 1, 100);
        ParamSet& p = c.node(em)->params;
        p.setStatic(k::PStyle, particlestyles::Point);
        p.setStatic(k::Color, QColor(255, 0, 0));
        p.setStatic(k::PColorEnd, QColor(255, 0, 0));
        p.setBase(k::PSize, 0.1); // 32 px across
        p.setBase(k::PSizeEnd, 0.1);
        p.setBase(k::POpacityEnd, 1);
        p.setBase(k::PStart, 0);
        p.setBase(k::PEnd, 0);
        const quint64 rn = addRender(c, em);
        co::connect(c, rn, 0, c.mediaOutId, nodeports::Input);
        const TimelineData tl = compTimeline(c);
        QImage first;
        for (Compositor* b : backends()) {
            const QString who = b == m_gpu.get() ? "GPU" : "CPU";
            const QImage img = nodeImage(b, tl, rn, 0);
            QVERIFY(!img.isNull());
            const Px mid = px(img, kW / 2, kH / 2);
            QVERIFY2(near(mid, 255, 0, 0, 255, 3), qPrintable(who + ": " + describe(mid)));
            QVERIFY2(px(img, kW / 2 + 12, kH / 2).a >= 250, qPrintable(who)); // inside the 16 px radius
            QVERIFY2(px(img, kW / 2 + 20, kH / 2).a <= 3, qPrintable(who)); // outside
            QVERIFY2(px(img, 10, 10).a <= 3, qPrintable(who));
            if (first.isNull()) {
                first = img;
            } else {
                int worst = 0;
                for (int y = 0; y < kH; ++y)
                    for (int x = 0; x < kW; ++x) {
                        const Px a = px(first, x, y), q = px(img, x, y);
                        worst = std::max({worst, std::abs(a.r - q.r), std::abs(a.g - q.g),
                            std::abs(a.b - q.b), std::abs(a.a - q.a)});
                    }
                QVERIFY2(worst <= 2, qPrintable(QString::number(worst)));
            }
        }
        // The frame after the emitter stopped still has the particle (100 frames of life), and
        // a frame before the start has none.
        for (Compositor* b : backends()) {
            QVERIFY(px(nodeImage(b, tl, rn, 7), kW / 2, kH / 2).a >= 250);
            QVERIFY(px(nodeImage(b, tl, rn, -1), kW / 2, kH / 2).a <= 3);
        }
        // Disabled: nothing is drawn.
        c.node(rn)->enabled = false;
        const TimelineData off = compTimeline(c);
        for (Compositor* b : backends())
            QVERIFY(px(nodeImage(b, off, rn, 0), kW / 2, kH / 2).a <= 3);
    }

    void pRenderBlobIsSoftAndOpacityFades()
    {
        Composition c = co::defaultComposition();
        const quint64 em = addEmitter(c, 1, 10);
        ParamSet& p = c.node(em)->params;
        p.setStatic(k::PStyle, particlestyles::Blob);
        p.setBase(k::PSize, 0.2); // 64 px across
        p.setBase(k::PSizeEnd, 0.2);
        p.setBase(k::POpacity, 1);
        p.setBase(k::POpacityEnd, 0);
        p.setBase(k::PStart, 0);
        p.setBase(k::PEnd, 0);
        const quint64 rn = addRender(c, em);
        const TimelineData tl = compTimeline(c);
        for (Compositor* b : backends()) {
            const Px born = px(nodeImage(b, tl, rn, 0), kW / 2, kH / 2);
            const Px edge = px(nodeImage(b, tl, rn, 0), kW / 2 + 24, kH / 2);
            const Px half = px(nodeImage(b, tl, rn, 5), kW / 2, kH / 2);
            QVERIFY(born.a >= 250);
            QVERIFY(edge.a > 3 && edge.a < 200); // soft towards the rim
            QVERIFY2(half.a > 110 && half.a < 145, describe(half)); // opacity 0.5 at half life
        }
    }

    void pRenderBitmapDrawsTheSpriteInput()
    {
        Composition c = co::defaultComposition();
        const quint64 bg = co::addNode(c, nodetypes::Background, {});
        c.node(bg)->params.setStatic(k::Color, QColor(0, 255, 0));
        const quint64 em = addEmitter(c, 1, 100);
        ParamSet& p = c.node(em)->params;
        p.setStatic(k::PStyle, particlestyles::Bitmap);
        p.setBase(k::PSize, 0.2); // 64 px wide, 36 px high (the sprite is the whole frame)
        p.setBase(k::PSizeEnd, 0.2);
        p.setBase(k::POpacityEnd, 1);
        p.setBase(k::PStart, 0);
        p.setBase(k::PEnd, 0);
        co::connect(c, bg, 0, em, nodeports::Sprite);
        const quint64 rn = addRender(c, em);
        const TimelineData tl = compTimeline(c);
        for (Compositor* b : backends()) {
            const QString who = b == m_gpu.get() ? "GPU" : "CPU";
            const QImage img = nodeImage(b, tl, rn, 0);
            QVERIFY(!img.isNull());
            QVERIFY2(near(px(img, kW / 2, kH / 2), 0, 255, 0, 255, 4), qPrintable(who));
            QVERIFY2(near(px(img, kW / 2 + 28, kH / 2), 0, 255, 0, 255, 6), qPrintable(who));
            QVERIFY2(px(img, kW / 2 + 36, kH / 2).a <= 3, qPrintable(who));
            QVERIFY2(near(px(img, kW / 2, kH / 2 + 14), 0, 255, 0, 255, 6), qPrintable(who));
            QVERIFY2(px(img, kW / 2, kH / 2 + 22).a <= 3, qPrintable(who));
        }
        // Without a Sprite picture a Bitmap particle draws nothing.
        co::disconnect(c, em, nodeports::Sprite);
        const TimelineData none = compTimeline(c);
        for (Compositor* b : backends())
            QVERIFY(px(nodeImage(b, none, rn, 0), kW / 2, kH / 2).a <= 3);
    }

    void pRenderOnTheMergeWithAPicture()
    {
        // pRender feeds the normal picture graph: Merge over a Background.
        Composition c = co::defaultComposition();
        const quint64 bg = co::addNode(c, nodetypes::Background, {});
        c.node(bg)->params.setStatic(k::Color, QColor(0, 0, 255));
        const quint64 em = addEmitter(c, 1, 100);
        c.node(em)->params.setStatic(k::PStyle, particlestyles::Point);
        c.node(em)->params.setBase(k::PSize, 0.1);
        c.node(em)->params.setBase(k::PSizeEnd, 0.1);
        c.node(em)->params.setBase(k::POpacityEnd, 1);
        const quint64 rn = addRender(c, em);
        const quint64 mg = co::addNode(c, nodetypes::Merge, {});
        co::connect(c, bg, 0, mg, nodeports::Background);
        co::connect(c, rn, 0, mg, nodeports::Foreground);
        co::connect(c, mg, 0, c.mediaOutId, nodeports::Input);
        const TimelineData tl = compTimeline(c);
        for (Compositor* b : backends()) {
            const QImage img = nodeImage(b, tl, mg, 0);
            QVERIFY(near(px(img, kW / 2, kH / 2), 255, 255, 255, 255, 4));
            QVERIFY(near(px(img, 10, 10), 0, 0, 255, 255, 4));
        }
    }

    // ---- Project ----

    void particleGraphSurvivesSaveAndLoad()
    {
        Composition c = co::defaultComposition();
        const quint64 em = addEmitter(c, 4.5, 33, 0.012);
        ParamSet& p = c.node(em)->params;
        p.setBase(k::Seed, 17);
        p.setBase(k::PAngleVar, 22);
        p.setStatic(k::PRegion, particleregions::Circle);
        p.setStatic(k::PStyle, particlestyles::Point);
        p.setStatic(k::Color, QColor(10, 200, 30));
        const quint64 tb = co::addNode(c, nodetypes::PTurbulence, {});
        const quint64 gr = co::addNode(c, nodetypes::PGravity, {});
        const quint64 mg = co::addNode(c, nodetypes::PMerge, {});
        co::connect(c, em, 0, tb, nodeports::Input);
        co::connect(c, tb, 0, gr, nodeports::Input);
        co::connect(c, gr, 0, mg, nodeports::Input);
        const quint64 rn = addRender(c, mg);
        co::connect(c, rn, 0, c.mediaOutId, nodeports::Input);
        const Composition back = serialization::compositionFromJson(serialization::compositionToJson(c));
        QCOMPARE(back.nodes.size(), c.nodes.size());
        QCOMPARE(back.node(mg)->inputs.value(nodeports::Input).nodeId, gr);
        QCOMPARE(back.node(rn)->inputs.value(nodeports::Input).nodeId, mg);
        QCOMPARE(back.node(em)->params.staticValue(k::PRegion).toString(), particleregions::Circle);
        QCOMPARE(back.node(em)->params.staticValue(k::Color).value<QColor>(), QColor(10, 200, 30));
        QVERIFY(same(at(c, mg, 25), at(back, mg, 25)));
        QVERIFY(at(back, mg, 25).size() > 50);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestParticles>(argc, argv);
}
#include "test_particles.moc"
