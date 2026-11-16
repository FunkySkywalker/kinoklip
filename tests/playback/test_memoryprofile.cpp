// SPDX-License-Identifier: GPL-3.0-or-later
// Memory profile: a repeatable measurement of where Kinoklip's memory goes in a typical session.
// The real main window opens a project with three 1080p clips (with audio), one 4K HEVC clip,
// effects, a compound clip, thumbnails and waveforms; then plays, scrubs, edits (many undo
// steps), switches pages and goes idle. After every stage one table row is printed:
//
//   RSS (anon / file / shmem), the GPU driver's own count where there is one (DRM fdinfo, or
//   GL_NVX_gpu_memory_info), and the big holders we can read: tile (thumbnail) cache, waveform
//   cache, still image cache, render decoders, output frame slots, render targets, idle
//   decoders, undo steps.
//
// It asserts only that the numbers stay sane (RSS below the "low-end" ceiling kRssCeilingMiB at
// the end); the point is the table in the test log (ctest --output-on-failure -V, or run
// the binary). Run alone:  scripts/test.sh release -R memoryprofile -V
// Environment: KK_MEMPROFILE_4K=0 leaves out the 4K clip; KK_MEMPROFILE_EDITS=n edit steps
// (default 120); KK_MEMPROFILE_OUT=<file> also appends the table to a file.
#include "../media/MediaTestUtil.h"
#include "../render/RenderTestUtil.h"
#include "../timeline/TimelineTestUtils.h"
#include "app/MainWindow.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/MemoryBudget.h"
#include "core/MemoryReport.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "media/ImageLoader.h"
#include "media/MediaCache.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/StandardActions.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <malloc.h>

using namespace kk;

namespace {

constexpr double kRssCeilingMiB = 2500.0;
constexpr int kFps = 24;

struct Rss
{
    double total = 0, anon = 0, file = 0, shmem = 0;
};

Rss readRss()
{
    Rss r;
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly))
        return r;
    for (const QByteArray& line : f.readAll().split('\n')) {
        const double mib = line.mid(line.indexOf(':') + 1).simplified().split(' ').value(0).toDouble()
            / 1024.0;
        if (line.startsWith("VmRSS:"))
            r.total = mib;
        else if (line.startsWith("RssAnon:"))
            r.anon = mib;
        else if (line.startsWith("RssFile:"))
            r.file = mib;
        else if (line.startsWith("RssShmem:"))
            r.shmem = mib;
    }
    return r;
}

double mib(int64_t bytes)
{
    return double(bytes) / (1 << 20);
}

bool makeClip(const QString& path, const QString& size, int seconds, bool hevc)
{
    QProcess ffmpeg;
    ffmpeg.setProcessChannelMode(QProcess::ForwardedChannels);
    QStringList args {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
        QStringLiteral("testsrc2=s=%1:r=%2:d=%3").arg(size).arg(kFps).arg(seconds), "-f", "lavfi",
        "-i", QStringLiteral("sine=frequency=440:sample_rate=48000:d=%1").arg(seconds), "-c:v",
        hevc ? "libx265" : "libx264", "-preset", "ultrafast", "-crf", "28", "-g", "24", "-pix_fmt",
        "yuv420p", "-c:a", "aac", "-b:a", "128k", "-shortest"};
    if (hevc)
        args << "-x265-params" << "log-level=error:pools=2" << "-tag:v" << "hvc1";
    args << path;
    ffmpeg.start(QStringLiteral("ffmpeg"), args);
    return ffmpeg.waitForFinished(300000) && ffmpeg.exitCode() == 0;
}

} // namespace

class TestMemoryProfile : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { kk::test::initTimelineTestCase("test_memoryprofile"); }
    void cleanupTestCase() { media::MediaCache::instance().shutdown(); }

    void sessionProfile()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("no ffmpeg");
        const bool with4k = qEnvironmentVariable("KK_MEMPROFILE_4K", QStringLiteral("1")) != QLatin1String("0");
        const int edits = qEnvironmentVariable("KK_MEMPROFILE_EDITS", QStringLiteral("120")).toInt();
        QTemporaryDir dir;
        QStringList files;
        for (int i = 0; i < 3; ++i) {
            files << dir.path() + QStringLiteral("/hd%1.mp4").arg(i);
            QVERIFY(makeClip(files.last(), QStringLiteral("1920x1080"), 8, false));
        }
        if (with4k) {
            files << dir.path() + QStringLiteral("/uhd.mp4");
            QVERIFY(makeClip(files.last(), QStringLiteral("3840x2160"), 4, true));
        }

        QStringList table;
        Rss baseRss;
        auto row = [&](const QString& stage, const render::ResourceStats& s) {
            media::MediaCache& cache = media::MediaCache::instance();
            const Rss r = readRss();
            QString gpu;
            if (s.gpuUsedMiB >= 0)
                gpu = QStringLiteral(" | GL gpu %1/%2 MiB").arg(s.gpuUsedMiB).arg(s.gpuTotalMiB);
            QString line = QStringLiteral("%1 | RSS %2 (anon %3, file %4, shmem %5)%6 | tiles %7 "
                                          "(%8 MiB) | waveforms %9 MiB | stills %10 MiB | render "
                                          "decoders %11 | idle decoders %12 | slots %13 (%14 MiB) "
                                          "| targets %15 MiB | frames alive %16")
                               .arg(stage.leftJustified(16))
                               .arg(r.total, 0, 'f', 0)
                               .arg(r.anon, 0, 'f', 0)
                               .arg(r.file, 0, 'f', 0)
                               .arg(r.shmem, 0, 'f', 0)
                               .arg(gpu)
                               .arg(cache.cachedImageCount())
                               .arg(mib(cache.cachedImageBytes()), 0, 'f', 1)
                               .arg(mib(cache.cachedWaveformBytes()), 0, 'f', 1)
                               .arg(mib(media::ImageLoader::cachedBytes()), 0, 'f', 1)
                               .arg(s.decoders)
                               .arg(cache.idleDecoderCount())
                               .arg(s.outputSlots)
                               .arg(mib(s.outputSlotBytes), 0, 'f', 1)
                               .arg(mib(s.targetBytes), 0, 'f', 1)
                               .arg(RenderedFrame::aliveCount());
            for (const QString& l : memreport::lines())
                if (l.startsWith(QLatin1String("drm")))
                    line += QStringLiteral(" | ") + l;
            table << line;
            qInfo().noquote() << "[memprofile]" << line;
        };

        Project project;
        SelectionModel selection;
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        PlaybackEngine::Options options;
        options.noAudio = true;
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        QObject::connect(&engine, &PlaybackEngine::rateChanged, &engine,
            [](double rate) { media::MediaCache::instance().setPlaybackActive(rate != 0.0); });
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ctx.playback = &engine;
        QObject::connect(&project, &Project::editCommitted, &engine, &PlaybackEngine::noteEditCommitted);

        MainWindowOptions wo;
        wo.autosave = false;
        wo.restoreLayout = false;
        wo.saveLayout = false;
        MainWindow window(ctx, wo);
        window.resize(1400, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto settle = [&](int ms) {
            QTest::qWait(ms);
            QCoreApplication::processEvents();
        };
        const std::shared_ptr<Compositor> compositor = engine.compositor();
        QVERIFY(compositor);
        settle(500);
        baseRss = readRss();
        row(QStringLiteral("start"), compositor->resourceStats());

        // ---- the project -----------------------------------------------------------------
        const ImportResult imported = project.importMedia(files);
        QCOMPARE(imported.importedIds.size(), files.size());
        settle(1500);
        row(QStringLiteral("imported"), compositor->resourceStats());

        project.beginEdit(QStringLiteral("project"));
        TimelineData& tl = project.timeline()->mutableData();
        tl = makeDefaultTimeline(QStringLiteral("profile"), {kFps, 1}, 1920, 1080, 3, 3);
        QVector<quint64> firstIds;
        auto addClip = [&](TrackData& track, const QString& mediaId, int64_t start, int64_t frames,
                            bool effects) {
            const media::MediaInfo info = project.mediaPool()->item(mediaId)->info;
            ClipData c;
            c.id = tl.allocateId();
            c.mediaId = mediaId;
            c.start = start;
            c.sourceDurationUs = info.durationUs;
            c.duration = frames;
            if (effects) {
                c.effects.append(EffectInstance {tl.allocateId(), QStringLiteral("gaussian_blur"), true, {}});
                c.effects.append(EffectInstance {tl.allocateId(), QStringLiteral("brightness_contrast"), true, {}});
            }
            track.clips.push_back(c);
            return c.id;
        };
        for (int i = 0; i < 3; ++i) {
            const quint64 id = addClip(tl.videoTracks[0], imported.importedIds[i], i * 96, 96, i != 1);
            addClip(tl.audioTracks[0], imported.importedIds[i], i * 96, 96, false);
            if (i == 0)
                firstIds.append(id);
        }
        if (with4k)
            addClip(tl.videoTracks[1], imported.importedIds[3], 40, 80, false);
        project.endEdit();
        QVERIFY(project.createCompoundClip(firstIds, QStringLiteral("Compound")));
        settle(2500); // thumbnails and waveforms of the timeline
        row(QStringLiteral("project open"), compositor->resourceStats());

        // ---- play, scrub, edit, pages ---------------------------------------------------
        engine.seek(30);
        settle(300);
        engine.play();
        settle(3500);
        row(QStringLiteral("playing 3.5 s"), compositor->resourceStats());
        engine.stop();
        for (int i = 0; i < 60; ++i) { // scrub across the whole timeline
            engine.seek(int64_t(i * 5) % 280);
            settle(25);
        }
        row(QStringLiteral("scrubbed"), compositor->resourceStats());

        const quint64 clipId = project.timeline()->data().videoTracks[0].clips.size() > 1
            ? project.timeline()->data().videoTracks[0].clips[1].id
            : 0;
        for (int i = 0; i < edits; ++i) {
            project.beginEdit(QStringLiteral("Nudge"));
            if (ClipData* c = editops::clipById(project.timeline()->mutableData(), clipId))
                c->duration = 90 + (i % 7);
            project.endEdit();
            if (i % 20 == 0)
                settle(30);
        }
        settle(500);
        row(QStringLiteral("%1 edits").arg(edits), compositor->resourceStats());
        const int undoSteps = project.undoStack()->count();
        qInfo().noquote() << "[memprofile] undo steps held:" << undoSteps;

        for (const QString& page : {"cut", "color", "compositing", "audio", "deliver", "edit"}) {
            window.setCurrentPage(page);
            settle(300);
            engine.seek(int64_t(page.size() * 17) % 200);
            settle(200);
        }
        row(QStringLiteral("every page"), compositor->resourceStats());
        engine.play();
        settle(2000);
        engine.stop();
        row(QStringLiteral("played again"), compositor->resourceStats());
        settle(16000); // idle: decoders and targets are given back
        row(QStringLiteral("idle 16 s"), compositor->resourceStats());

        // What the idle process still holds: drop the caches one by one and trim the C heap
        // (freed memory that glibc keeps is "retained", not "held").
        qInfo().noquote() << "[memprofile] backend:" << compositor->backendDescription();
        media::MediaCache::instance().clearMemoryCache();
        settle(300);
        row(QStringLiteral("caches cleared"), compositor->resourceStats());
        ::malloc_trim(0);
        row(QStringLiteral("malloc_trim"), compositor->resourceStats());

        const QString text = table.join(QLatin1Char('\n'));
        if (const QString out = qEnvironmentVariable("KK_MEMPROFILE_OUT"); !out.isEmpty()) {
            QFile f(out);
            if (f.open(QIODevice::Append | QIODevice::Text))
                f.write(text.toUtf8() + "\n\n");
        }
        const Rss end = readRss();
        qInfo().noquote() << "[memprofile] start RSS" << baseRss.total << "MiB, end RSS" << end.total
                          << "MiB";
        if (end.total > 0)
            QVERIFY2(end.total < kRssCeilingMiB, "process memory above the ceiling of the profile");
    }
};

int main(int argc, char** argv)
{
    if (qEnvironmentVariable("KK_MEMPROFILE_ALLOC") != QLatin1String("default"))
        kk::membudget::tuneAllocator(); // as the application does; "default" = glibc as shipped
    return kk::test::glTestMain<TestMemoryProfile, QApplication>(argc, argv); // llvmpipe under xvfb: GL memory is process memory
}
#include "test_memoryprofile.moc"
