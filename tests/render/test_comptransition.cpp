// SPDX-License-Identifier: GPL-3.0-or-later
// Spec 128: a transition built on the Compositing page (MediaIn1 / MediaIn2), and the Loader,
// Saver, Time Speed and Time Stretcher nodes. Pixel cases render on the GPU and on the CPU.
#include "RenderTestUtil.h"
#include "core/CompOps.h"
#include "core/EditOps.h"
#include "core/NodeRegistry.h"
#include "core/Serialization.h"
#include "render/CompLoader.h"
#include "render/CompSaver.h"
#include "render/CompTimeMath.h"
#include "render/Compositor.h"
#include "render/TimelineEval.h"

#include <QDir>
#include <QImageReader>
#include <QImageWriter>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <memory>

using namespace kk;
using namespace kk::test;
namespace co = kk::compops;
namespace tmath = kk::render::timemath;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

/// Frame i of a test sequence: a uniform picture with red 20 * (i + 1), green 40, blue 60.
QColor frameColor(int i)
{
    return QColor(20 * (i + 1), 40, 60);
}

/// Writes @p count PNG files seq_0001.png ... into @p dir; returns the first one's path.
QString writeSequence(const QString& dir, int count)
{
    for (int i = 0; i < count; ++i) {
        QImage image(64, 36, QImage::Format_RGB32);
        image.fill(frameColor(i));
        image.save(QDir(dir).filePath(QStringLiteral("seq_%1.png").arg(i + 1, 4, 10, QLatin1Char('0'))));
    }
    return QDir(dir).filePath(QStringLiteral("seq_0001.png"));
}

TimelineData compTimeline(const Composition& comp)
{
    TimelineData tl = emptyTimeline(kW, kH);
    ClipData c = solidClip(tl, QColor(0, 0, 0), 0, 50);
    c.composition = comp;
    addClip(tl, TrackType::Video, 0, c);
    return tl;
}

/// MediaIn -> @p node -> MediaOut graph pieces: a Loader reading @p path.
quint64 addLoader(Composition& c, const QString& path)
{
    const quint64 id = co::addNode(c, nodetypes::Loader, {});
    c.node(id)->params.setStatic(nodekeys::LoaderPath, path);
    return id;
}

} // namespace

class TestCompTransition : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;

    QVector<Compositor*> backends() { return {m_gpu.get(), m_cpu.get()}; }

    QImage nodeImage(Compositor* c, const TimelineData& tl, quint64 clipId, quint64 id, int64_t frame,
        int transition = 0)
    {
        const RenderedFramePtr f = c->renderCompositionNode(snap(tl), clipId, id, frame, {},
                                        Quality::Full, OutputImage, nullptr, nullptr, transition)
                                       .get();
        return f && f->ok ? f->image : QImage();
    }
    static quint64 clipIdOf(const TimelineData& tl) { return tl.videoTracks[0].clips[0].id; }

    /// The centre pixel of the composition's output at @p frame on both backends must be @p want
    /// (alpha 0 = nothing shown).
    void expectColor(const TimelineData& tl, int64_t frame, QColor want, int tol = 3, quint64 node = 0)
    {
        for (Compositor* c : backends()) {
            const QImage image = nodeImage(c, tl, clipIdOf(tl), node, frame);
            QVERIFY(!image.isNull());
            const Px p = px(image, kW / 2, kH / 2);
            const bool ok = want.alpha() == 0
                ? p.a <= tol
                : near(p, want.red(), want.green(), want.blue(), 255, tol);
            QVERIFY2(ok,
                qPrintable(QStringLiteral("%1 frame %2: got %3")
                               .arg(c == m_gpu.get() ? "GPU" : "CPU")
                               .arg(frame)
                               .arg(QString::fromLatin1(describe(p)))));
        }
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

    // ---- Frame mapping (pure) ----

    void timeSpeedMapping()
    {
        ParamSet p;
        p.setBase(nodekeys::TimeSpeedKey, 2.0);
        p.setBase(nodekeys::TimeDelay, 3.0);
        QCOMPARE(tmath::speedSourceTime(p, 10), 14.0); // (10 - 3) * 2
        QCOMPARE(tmath::speedSourceTime(p, 3), 0.0);
        p.setBase(nodekeys::TimeSpeedKey, -1.0);
        p.setBase(nodekeys::TimeDelay, 0.0);
        QCOMPARE(tmath::speedSourceTime(p, 4), -4.0);
        p.setBase(nodekeys::TimeSpeedKey, 0.5);
        QCOMPARE(tmath::speedSourceTime(p, 5), 2.5);
    }

    void interpolationTaps()
    {
        const tmath::Taps nearest = tmath::taps(2.5, false);
        QCOMPARE(nearest.from, 3);
        QVERIFY(nearest.single());
        const tmath::Taps blend = tmath::taps(2.25, true);
        QCOMPARE(blend.from, 2);
        QCOMPARE(blend.to, 3);
        QVERIFY(qAbs(blend.blend - 0.25) < 1e-9);
        QVERIFY(tmath::taps(3.0, true).single());
        QCOMPARE(tmath::taps(-0.5, true).from, -1);
    }

    void timeStretcherMapping()
    {
        ParamSet p;
        AnimatedValue& v = p.animated(nodekeys::SourceTime, 0.0);
        Keyframe a;
        a.frame = 0;
        a.value = 100;
        Keyframe b;
        b.frame = 10;
        b.value = 0;
        v.keys = {a, b};
        QCOMPARE(tmath::stretchSourceTime(p, 0), 100.0);
        QVERIFY(qAbs(tmath::stretchSourceTime(p, 5) - 50.0) < 1e-6);
        QCOMPARE(tmath::stretchSourceTime(p, 10), 0.0);
    }

    void loaderTiming()
    {
        tmath::LoaderTiming t;
        // Four frames from comp frame 1, held two before and one after.
        t.globalIn = 1;
        t.holdFirst = 2;
        t.holdLast = 1;
        const int want[] = {-1, 0, 0, 0, 1, 2, 3, 3, -1};
        for (int f = 0; f < 9; ++f)
            QCOMPARE(tmath::loaderFrame(t, 4, false, f), want[f]);
        // Loop repeats the clip with its holds.
        t.loop = true;
        QCOMPARE(tmath::loaderFrame(t, 4, false, 8), 0);  // at 7 % 7 = 0 -> held first
        QCOMPARE(tmath::loaderFrame(t, 4, false, -2), 2); // at -3 wraps to 4, two held: frame 2
        // Trims: frames 1 and 2 only.
        tmath::LoaderTiming u;
        u.trimIn = 1;
        u.trimOut = 1;
        QCOMPARE(tmath::loaderFrame(u, 4, false, 0), 1);
        QCOMPARE(tmath::loaderFrame(u, 4, false, 1), 2);
        QCOMPARE(tmath::loaderFrame(u, 4, false, 2), -1);
        // Reverse.
        u.reverse = true;
        QCOMPARE(tmath::loaderFrame(u, 4, false, 0), 2);
        QCOMPARE(tmath::loaderFrame(u, 4, false, 1), 1);
        // A still image shows everywhere.
        QCOMPARE(tmath::loaderFrame(tmath::LoaderTiming {}, 1, true, 500), 0);
        QCOMPARE(tmath::loaderFrame(tmath::LoaderTiming {}, 0, false, 0), -1);
    }

    // ---- Loader ----

    void loaderReadsASequence()
    {
        QTemporaryDir dir;
        const QString first = writeSequence(dir.path(), 4);
        const render::LoaderClip clip = render::CompLoader::describe(first);
        QCOMPARE(int(clip.kind), int(render::LoaderClip::Kind::Sequence));
        QCOMPARE(clip.count, 4);

        Composition c = co::defaultComposition();
        const quint64 ld = addLoader(c, first);
        co::connect(c, ld, 0, c.mediaOutId, nodeports::Input);
        ParamSet& p = c.node(ld)->params;
        p.setBase(nodekeys::GlobalIn, 1);
        p.setBase(nodekeys::HoldFirst, 2);
        p.setBase(nodekeys::HoldLast, 1);
        const TimelineData tl = compTimeline(c);
        const int want[] = {-1, 0, 0, 0, 1, 2, 3, 3, -1};
        for (int f = 0; f < 9; ++f)
            expectColor(tl, f, want[f] < 0 ? QColor(0, 0, 0, 0) : frameColor(want[f]));
    }

    void loaderLoopsAndReverses()
    {
        QTemporaryDir dir;
        const QString first = writeSequence(dir.path(), 4);
        Composition c = co::defaultComposition();
        const quint64 ld = addLoader(c, first);
        co::connect(c, ld, 0, c.mediaOutId, nodeports::Input);
        c.node(ld)->params.setStatic(nodekeys::Loop, true);
        TimelineData tl = compTimeline(c);
        for (int f : {0, 1, 3, 4, 5, 9, 10})
            expectColor(tl, f, frameColor(f % 4));
        c.node(ld)->params.setStatic(nodekeys::Reverse, true);
        tl = compTimeline(c);
        for (int f : {0, 1, 4, 6})
            expectColor(tl, f, frameColor(3 - f % 4));
    }

    void loaderShowsAStillEverywhereAndFitsIt()
    {
        QTemporaryDir dir;
        QImage image(100, 100, QImage::Format_RGB32); // square: fitted with bars left and right
        image.fill(QColor(200, 100, 50));
        const QString path = dir.filePath(QStringLiteral("still.png"));
        QVERIFY(image.save(path));
        Composition c = co::defaultComposition();
        const quint64 ld = addLoader(c, path);
        co::connect(c, ld, 0, c.mediaOutId, nodeports::Input);
        const TimelineData tl = compTimeline(c);
        expectColor(tl, 0, QColor(200, 100, 50));
        expectColor(tl, 40, QColor(200, 100, 50));
        for (Compositor* b : backends()) {
            const QImage out = nodeImage(b, tl, clipIdOf(tl), 0, 3);
            QVERIFY(px(out, 5, kH / 2).a <= 3); // the bar
        }
    }

    void loaderWithoutAFileShowsNothing()
    {
        Composition c = co::defaultComposition();
        const quint64 ld = addLoader(c, QStringLiteral("/nonexistent/none_0001.png"));
        co::connect(c, ld, 0, c.mediaOutId, nodeports::Input);
        expectColor(compTimeline(c), 2, QColor(0, 0, 0, 0));
    }

    // ---- Time nodes ----

    void timeSpeedAndStretcherShowTheMappedFrames()
    {
        QTemporaryDir dir;
        const QString first = writeSequence(dir.path(), 10);
        Composition c = co::defaultComposition();
        const quint64 ld = addLoader(c, first);
        const quint64 sp = co::addNode(c, nodetypes::TimeSpeed, {});
        co::connect(c, ld, 0, sp, nodeports::Input);
        co::connect(c, sp, 0, c.mediaOutId, nodeports::Input);
        c.node(sp)->params.setBase(nodekeys::TimeSpeedKey, 2.0);
        TimelineData tl = compTimeline(c);
        expectColor(tl, 0, frameColor(0));
        expectColor(tl, 2, frameColor(4));
        expectColor(tl, 4, frameColor(8));
        expectColor(tl, 5, QColor(0, 0, 0, 0)); // frame 10 of the clip: past its end
        // Delay: the clip starts later (frames before it map to a negative time: nothing).
        c.node(sp)->params.setBase(nodekeys::TimeSpeedKey, 1.0);
        c.node(sp)->params.setBase(nodekeys::TimeDelay, 3.0);
        tl = compTimeline(c);
        expectColor(tl, 2, QColor(0, 0, 0, 0));
        expectColor(tl, 3, frameColor(0));
        expectColor(tl, 5, frameColor(2));
        // Blend at half speed: frame 1 is between frames 0 and 0.5 -> mix of 0 and 1.
        c.node(sp)->params.setBase(nodekeys::TimeSpeedKey, 0.5);
        c.node(sp)->params.setBase(nodekeys::TimeDelay, 0.0);
        c.node(sp)->params.setStatic(nodekeys::TimeInterp, timeinterps::Blend);
        tl = compTimeline(c);
        const QColor mix((frameColor(0).red() + frameColor(1).red()) / 2, 40, 60);
        expectColor(tl, 1, mix, 4);
        expectColor(tl, 2, frameColor(1), 3);
        // Nearest at half speed: frame 1 is 0.5 -> rounds to frame 1.
        c.node(sp)->params.setStatic(nodekeys::TimeInterp, timeinterps::Nearest);
        tl = compTimeline(c);
        expectColor(tl, 1, frameColor(1));
        expectColor(tl, 3, frameColor(2)); // 1.5 -> 2

        // Time Stretcher: Source Time keyed from 9 down to 0 over comp frames 0 to 9 (reversed).
        Composition d = co::defaultComposition();
        const quint64 ld2 = addLoader(d, first);
        const quint64 st = co::addNode(d, nodetypes::TimeStretcher, {});
        co::connect(d, ld2, 0, st, nodeports::Input);
        co::connect(d, st, 0, d.mediaOutId, nodeports::Input);
        AnimatedValue& v = d.node(st)->params.animated(nodekeys::SourceTime, 0.0);
        Keyframe a;
        a.frame = 0;
        a.value = 9;
        Keyframe b;
        b.frame = 9;
        b.value = 0;
        v.keys = {a, b};
        tl = compTimeline(d);
        expectColor(tl, 0, frameColor(9));
        expectColor(tl, 4, frameColor(5));
        expectColor(tl, 9, frameColor(0));
    }

    void timeNodeOnTheClipsOwnPicture()
    {
        // The clip's own picture (a solid) through a Time Speed: the same on every frame, and the
        // evaluation must not break (GPU and CPU re-pull the input at another frame).
        Composition c = co::defaultComposition();
        const quint64 sp = co::addNode(c, nodetypes::TimeSpeed, {});
        const CompNode* in = c.node(c.mediaOutId);
        const NodeLink link = in->inputs.value(nodeports::Input);
        co::connect(c, link.nodeId, 0, sp, nodeports::Input);
        co::connect(c, sp, 0, c.mediaOutId, nodeports::Input);
        c.node(sp)->params.setBase(nodekeys::TimeSpeedKey, 3.0);
        TimelineData tl = emptyTimeline(kW, kH);
        ClipData clip = solidClip(tl, QColor(10, 200, 30), 0, 50);
        clip.composition = c;
        addClip(tl, TrackType::Video, 0, clip);
        expectColor(tl, 7, QColor(10, 200, 30));
    }

    // ---- Saver ----

    void saverWritesFilesOnlyWhenRendered()
    {
        QTemporaryDir src, out;
        const QString first = writeSequence(src.path(), 4);
        Composition c = co::defaultComposition();
        const quint64 ld = addLoader(c, first);
        const quint64 sv = co::addNode(c, nodetypes::Saver, {});
        co::connect(c, ld, 0, sv, nodeports::Input);
        co::connect(c, sv, 0, c.mediaOutId, nodeports::Input);
        const QString base = QDir(out.path()).filePath(QStringLiteral("shot.png"));
        c.node(sv)->params.setStatic(nodekeys::SaverPath, base);
        c.node(sv)->params.setBase(nodekeys::SaverStart, 10);
        const TimelineData tl = compTimeline(c);

        // Drawing frames (viewer, playback) writes nothing and shows the input unchanged.
        for (Compositor* b : backends()) {
            for (int f = 0; f < 4; ++f) {
                const QImage image = nodeImage(b, tl, clipIdOf(tl), 0, f);
                QVERIFY(near(px(image, kW / 2, kH / 2), frameColor(f).red(), 40, 60, 255, 3));
            }
            const RenderedFramePtr whole = b->render(snap(tl), 2, {}, Quality::Full, OutputImage);
            QVERIFY(whole && whole->ok);
        }
        QCOMPARE(QDir(out.path()).entryList(QDir::Files).size(), 0);
        QCOMPARE(render::compsaver::activeSavers(*tl.videoTracks[0].clips[0].composition).size(), 1);

        // Rendering the composition writes one file per frame, numbered from First Number.
        for (Compositor* b : backends()) {
            QDir(out.path()).removeRecursively();
            const render::compsaver::Result r = render::compsaver::run(
                *b, snap(tl), clipIdOf(tl), 0, 3);
            QCOMPARE(r.files, 4);
            QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(QLatin1Char(';'))));
            const QStringList files = QDir(out.path()).entryList(QDir::Files, QDir::Name);
            QCOMPARE(files,
                QStringList({QStringLiteral("shot0010.png"), QStringLiteral("shot0011.png"),
                    QStringLiteral("shot0012.png"), QStringLiteral("shot0013.png")}));
            for (int i = 0; i < 4; ++i) {
                const QImage file(QDir(out.path()).filePath(files[i]));
                QCOMPARE(file.size(), QSize(kW, kH));
                const QRgb v = file.pixel(kW / 2, kH / 2);
                QVERIFY(std::abs(qRed(v) - frameColor(i).red()) <= 3);
                QVERIFY(std::abs(qGreen(v) - 40) <= 3);
                QVERIFY(std::abs(qBlue(v) - 60) <= 3);
                QCOMPARE(qAlpha(v), 255);
            }
        }

        // Switched off, a Saver writes nothing.
        QDir(out.path()).removeRecursively();
        QDir().mkpath(out.path());
        Composition off = *tl.videoTracks[0].clips[0].composition;
        off.node(sv)->params.setStatic(nodekeys::SaverActive, false);
        const TimelineData offTl = compTimeline(off);
        const render::compsaver::Result none
            = render::compsaver::run(*m_cpu, snap(offTl), clipIdOf(offTl), 0, 3);
        QCOMPARE(none.files, 0);
    }

    void saverWritesTiff()
    {
        if (!QImageWriter::supportedImageFormats().contains("tiff"))
            QSKIP("no TIFF writer in this Qt");
        QTemporaryDir out;
        QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor(10, 20, 30));
        const QString file = render::compsaver::fileName(
            QDir(out.path()).filePath(QStringLiteral("a.png")), 3, saverformats::Tiff);
        QCOMPARE(QFileInfo(file).fileName(), QStringLiteral("a0003.tif"));
        QString error;
        QVERIFY2(render::compsaver::writeImage(image, file, saverformats::Tiff, &error), qPrintable(error));
        const QImage back(file);
        QCOMPARE(QColor(back.pixel(2, 2)), QColor(10, 20, 30));
    }

    // ---- Transition built on the page ----

    /// V1: A (red) [0, 50), B (blue) [50, 100) with a Fusion Composition transition of 10 frames
    /// centred on the cut: the window is frames 45 .. 54.
    TimelineData transitionTimeline(std::optional<Composition> comp)
    {
        TimelineData tl = emptyTimeline(kW, kH);
        ClipData a = solidClip(tl, QColor(255, 0, 0), 0, 50);
        ClipData b = solidClip(tl, QColor(0, 0, 255), 50, 50);
        TransitionData t;
        t.type = kCompositionTransition;
        t.duration = 10;
        t.composition = std::move(comp);
        b.transitionIn = t;
        addClip(tl, TrackType::Video, 0, a);
        addClip(tl, TrackType::Video, 0, b);
        return tl;
    }

    void expectTimelineColor(const TimelineData& tl, int64_t frame, QColor want, int tol = 4)
    {
        for (Compositor* c : backends()) {
            const RenderedFramePtr f = c->render(snap(tl), frame, {}, Quality::Full, OutputImage);
            QVERIFY(f && f->ok);
            const Px p = px(f->image, kW / 2, kH / 2);
            QVERIFY2(near(p, want.red(), want.green(), want.blue(), 255, tol),
                qPrintable(QStringLiteral("%1 frame %2: got %3, want %4,%5,%6")
                               .arg(c == m_gpu.get() ? "GPU" : "CPU")
                               .arg(frame)
                               .arg(QString::fromLatin1(describe(p)))
                               .arg(want.red())
                               .arg(want.green())
                               .arg(want.blue())));
        }
    }

    static QColor blended(double t)
    {
        return QColor(int(std::lround(255 * (1 - t))), 0, int(std::lround(255 * t)));
    }

    void defaultTransitionBlendsLikeADissolve()
    {
        // No stored graph: the default (a Merge whose Blend runs over the window).
        const TimelineData tl = transitionTimeline(std::nullopt);
        expectTimelineColor(tl, 44, blended(0));
        expectTimelineColor(tl, 45, blended(1.0 / 11));
        expectTimelineColor(tl, 50, blended(6.0 / 11));
        expectTimelineColor(tl, 54, blended(10.0 / 11));
        expectTimelineColor(tl, 55, blended(1));
        // The stored default is the same picture.
        const TimelineData stored = transitionTimeline(co::defaultTransitionComposition(10));
        expectTimelineColor(stored, 50, blended(6.0 / 11));
    }

    void aCustomGraphIsTheTransition()
    {
        // MediaIn2 over MediaIn1 stays at Blend 1: the window shows the incoming clip only.
        Composition comp = co::defaultTransitionComposition(10);
        quint64 merge = 0;
        for (const CompNode& n : comp.nodes)
            if (n.type == nodetypes::Merge)
                merge = n.id;
        comp.node(merge)->params.animated(nodekeys::Opacity, 1.0).keys.clear();
        comp.node(merge)->params.setBase(nodekeys::Opacity, 1.0);
        const TimelineData tl = transitionTimeline(comp);
        expectTimelineColor(tl, 44, blended(0));
        expectTimelineColor(tl, 46, blended(1));
        expectTimelineColor(tl, 53, blended(1));
        // The Merge Blend animated by hand: 0 at frame 0, 1 at frame 9, halfway at frame 4.5.
        Composition keyed = co::defaultTransitionComposition(10);
        AnimatedValue& v = keyed.node(merge)->params.animated(nodekeys::Opacity, 0.0);
        Keyframe a;
        a.frame = 0;
        a.value = 0;
        Keyframe b;
        b.frame = 9;
        b.value = 1;
        v.keys = {a, b};
        const TimelineData tl2 = transitionTimeline(keyed);
        expectTimelineColor(tl2, 45, blended(0));
        expectTimelineColor(tl2, 54, blended(1));
        expectTimelineColor(tl2, 49, blended(4.0 / 9));
    }

    void mediaIn1AndMediaIn2ShowTheTwoClipsAtStartMiddleAndEnd()
    {
        const Composition comp = co::defaultTransitionComposition(10);
        quint64 in1 = 0, in2 = 0;
        for (const CompNode& n : comp.nodes) {
            if (n.transitionSide == 1)
                in1 = n.id;
            if (n.transitionSide == 2)
                in2 = n.id;
        }
        QVERIFY(in1 && in2);
        QCOMPARE(comp.node(in1)->name, QStringLiteral("MediaIn1"));
        const TimelineData tl = transitionTimeline(comp);
        const quint64 owner = tl.videoTracks[0].clips[1].id; // B holds the transitionIn
        for (Compositor* c : backends()) {
            for (int64_t f : {0, 5, 9}) {
                const Px outgoing = px(nodeImage(c, tl, owner, in1, f, 1), kW / 2, kH / 2);
                const Px incoming = px(nodeImage(c, tl, owner, in2, f, 1), kW / 2, kH / 2);
                QVERIFY2(near(outgoing, 255, 0, 0, 255, 3), describe(outgoing));
                QVERIFY2(near(incoming, 0, 0, 255, 255, 3), describe(incoming));
            }
            // MediaOut of the graph at the middle: the blend.
            const Px mid = px(nodeImage(c, tl, owner, 0, 5, 1), kW / 2, kH / 2);
            const QColor want = blended(6.0 / 11);
            QVERIFY2(near(mid, want.red(), 0, want.blue(), 255, 4), describe(mid));
        }
        // The window is the transition's 10 frames: comp frame 0 is timeline frame 45.
        const auto span = render::ownTransition(tl.videoTracks[0], 1, false);
        QVERIFY(span);
        QCOMPARE(span->start, 45);
        QCOMPARE(span->length(), 10);
    }

    void aTransitionGraphSurvivesSaveAndLoad()
    {
        Composition comp = co::defaultTransitionComposition(10);
        const quint64 extra = co::addNode(comp, nodetypes::Background, QPointF(0, 6));
        comp.node(extra)->params.setStatic(nodekeys::Color, QColor(0, 255, 0));
        const TimelineData tl = transitionTimeline(comp);
        const QJsonObject json = serialization::timelineToJson(tl);
        TimelineData loaded;
        QString error;
        QVERIFY2(serialization::timelineFromJson(json, loaded, &error), qPrintable(error));
        const ClipData& b = loaded.videoTracks[0].clips[1];
        QVERIFY(b.transitionIn && b.transitionIn->composition);
        QCOMPARE(*b.transitionIn->composition, comp);
        QCOMPARE(b.transitionIn->type, kCompositionTransition);
        int sides = 0;
        for (const CompNode& n : b.transitionIn->composition->nodes)
            sides += n.transitionSide != 0;
        QCOMPARE(sides, 2);
        expectTimelineColor(loaded, 50, blended(6.0 / 11));
        // A transition without a stored graph saves and loads without one.
        TimelineData plain;
        QVERIFY(serialization::timelineFromJson(
            serialization::timelineToJson(transitionTimeline(std::nullopt)), plain, &error));
        QVERIFY(!plain.videoTracks[0].clips[1].transitionIn->composition);
    }

    void aTransitionGraphIsStoredByTheEditOps()
    {
        TimelineData tl = transitionTimeline(std::nullopt);
        const quint64 b = tl.videoTracks[0].clips[1].id;
        QVERIFY(editops::setTransitionComposition(tl, b, false, co::defaultTransitionComposition(10)));
        QVERIFY(tl.videoTracks[0].clips[1].transitionIn->composition);
        QVERIFY(!editops::setTransitionComposition(tl, b, true, co::defaultTransitionComposition(10)));
        QVERIFY(editops::setTransitionComposition(tl, b, false, std::nullopt));
        QVERIFY(!tl.videoTracks[0].clips[1].transitionIn->composition);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestCompTransition>(argc, argv);
}
#include "test_comptransition.moc"
