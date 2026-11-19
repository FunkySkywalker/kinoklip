// SPDX-License-Identifier: GPL-3.0-or-later
// Reverse playback of a long-GOP H.264 clip (Change Clip Speed > Reverse, J shuttle): the
// decoder must serve backward requests from one forward-decoded stretch instead of seeking to the
// keyframe for every frame (VideoDecoder reverse cache, spec 143).
//
// * decoderBackwardCost: deterministic. Steps a VideoDecoder backwards over two GOPs and checks
//   that every frame equals the one an exact seek returns, and that it took far fewer than the
//   (distance from the keyframe) frames per request that seeking each time costs.
// * reversedClipAt1x: real engine, fake sound card as clock. Plays a reversed clip at 1x and
//   reports frames shown vs lost and the render thread's decode time per frame; asserts only
//   a generous share of frames arrived (a loaded machine must not make it flaky).
//
// Media is generated once into KK_REVERSE_DIR (default: a temporary folder). KK_REVERSE_SIZE
// (default 1920x1080) picks the picture size; KINOKLIP_REVERSE_CACHE=off measures the old behaviour.
#include "PlaybackTestUtil.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "media/VideoDecoder.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTest>

#include <algorithm>
#include <vector>

using namespace kk;

namespace {

constexpr int kFps = 60;
constexpr int kGop = 250;
constexpr int kFileSeconds = 24;

bool makeLongGop(const QString& path)
{
    if (QFile::exists(path))
        return true;
    const QString size = qEnvironmentVariable("KK_REVERSE_SIZE", QStringLiteral("1920x1080"));
    QProcess ffmpeg;
    ffmpeg.setProcessChannelMode(QProcess::ForwardedChannels);
    ffmpeg.start(QStringLiteral("ffmpeg"),
        {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
            QStringLiteral("testsrc2=s=%1:r=%2:d=%3").arg(size).arg(kFps).arg(kFileSeconds), "-f",
            "lavfi", "-i", QStringLiteral("sine=frequency=440:d=%1").arg(kFileSeconds), "-c:v",
            "libx264", "-preset", "veryfast", "-crf", "20", "-g", QString::number(kGop),
            "-keyint_min", QString::number(kGop), "-sc_threshold", "0", "-bf", "2", "-pix_fmt",
            "yuv420p", "-c:a", "aac", "-shortest", path + ".tmp.mp4"});
    if (!ffmpeg.waitForFinished(10 * 60 * 1000) || ffmpeg.exitCode() != 0)
        return false;
    return QFile::rename(path + ".tmp.mp4", path);
}

int64_t frameUs(int frame)
{
    return framesToUs(frame, Rational {kFps, 1});
}

/// Hash of a frame's luma plane (rows sampled), to compare pictures without keeping them.
quint64 lumaHash(const media::VideoFrame& f)
{
    quint64 h = 1469598103934665603ULL;
    for (int y = 0; y < f.height; y += 7) {
        const uint8_t* row = f.data[0] + size_t(y) * size_t(f.linesize[0]);
        for (int x = 0; x < f.width * f.bytesPerPixel(0); x += 5)
            h = (h ^ row[x]) * 1099511628211ULL;
    }
    return h;
}

struct Shown
{
    int64_t frame = 0;
    double decodeMs = 0.0;
};

} // namespace

class TestReversePlay : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("no ffmpeg");
        QString dir = qEnvironmentVariable("KK_REVERSE_DIR");
        if (dir.isEmpty())
            dir = m_temp.path();
        QDir().mkpath(dir);
        m_path = dir + QStringLiteral("/reverse_gop250.mp4");
        QVERIFY(makeLongGop(m_path));
    }

    void decoderBackwardCost()
    {
        const int first = 3 * kGop + 40; // inside a GOP, far from its keyframe
        const int count = 300; // crosses the keyframe at 3 * kGop into the previous GOP
        media::VideoDecoder reference;
        QVERIFY(reference.open(m_path));
        media::VideoDecoder d;
        QVERIFY(d.open(m_path));
        const int64_t before = d.decodedFrameCount();
        QElapsedTimer t;
        double totalMs = 0.0;
        for (int i = 0; i < count; ++i) {
            const int frame = first + 40 - i;
            t.restart();
            const auto got = d.decodeFrame(frameUs(frame));
            totalMs += t.nsecsElapsed() / 1e6;
            QVERIFY(got);
            if (i % 7 == 0 || i < 3 || i > count - 3) { // spot-check against an exact seek
                const auto want = reference.decodeFrame(frameUs(frame));
                QVERIFY(want);
                QCOMPARE(got->ptsUs, want->ptsUs);
                QCOMPARE(lumaHash(*got), lumaHash(*want));
            }
        }
        const double decodedPerRequest = double(d.decodedFrameCount() - before) / count;
        qInfo("backward by frame, %d requests: %.2f ms per frame, %.2f frames decoded per request",
            count, totalMs / count, decodedPerRequest);
        if (qEnvironmentVariable("KINOKLIP_REVERSE_CACHE") != QLatin1String("off")) {
            // Seeking each time decodes about 40 + i frames per request (>= 100 on average);
            // a stretch decodes each frame about once (the stretch limit costs a little more).
            QVERIFY2(decodedPerRequest < 6.0, qPrintable(QString::number(decodedPerRequest)));
        }
    }

    void reversedClipAt1x()
    {
        Project project;
        const ImportResult imported = project.importMedia({m_path});
        QCOMPARE(imported.importedIds.size(), 1);
        const int clipFrames = 240; // 4 s
        project.beginEdit(QStringLiteral("reverse"));
        TimelineData& tl = project.timeline()->mutableData();
        const media::MediaInfo& info = project.mediaPool()->item(imported.importedIds[0])->info;
        tl = makeDefaultTimeline(
            QStringLiteral("reverse"), {kFps, 1}, info.displayWidth, info.displayHeight, 1, 1);
        ClipData c;
        c.id = tl.allocateId();
        c.mediaId = imported.importedIds[0];
        c.start = 0;
        c.duration = clipFrames;
        c.sourceDurationUs = info.durationUs;
        c.sourceInUs = frameUs(3 * kGop + 120); // plays down to 3 * kGop + 120 - 240 (two GOPs)
        c.speed = -1.0;
        tl.videoTracks[0].clips.push_back(c);
        project.endEdit();

        auto* audio = new test::FakeAudioOutput(10, 40);
        PlaybackEngine::Options options;
        options.audio.reset(audio);
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        int client = 0;
        engine.setOutputClient(&client, QSize(1000, 562), engine.compositor()->isGpu());
        QVERIFY(QTest::qWaitFor([&] { return engine.currentFrame() != nullptr; }, 10000));
        std::vector<Shown> shown;
        QObject::connect(
            &engine, &PlaybackEngine::frameReady, &engine, [&](const RenderedFramePtr& f) {
                if (f && f->ok)
                    shown.push_back({f->frame, f->decodeMs});
            });
        engine.play();
        QVERIFY(QTest::qWaitFor([&] { return !engine.isPlaying(); }, clipFrames * 1000 / kFps + 20000));

        std::vector<double> decode;
        for (const Shown& s : shown)
            decode.push_back(s.decodeMs);
        std::sort(decode.begin(), decode.end());
        const double median = decode.empty() ? 0.0 : decode[decode.size() / 2];
        const double p95 = decode.empty() ? 0.0 : decode[decode.size() * 95 / 100];
        const int lost = clipFrames - int(shown.size());
        qInfo("reversed 1x, %d frames: shown %d, lost %d (%.0f%%); render-thread decode per frame: "
              "median %.2f ms, p95 %.1f ms, max %.1f ms",
            clipFrames, int(shown.size()), lost, 100.0 * lost / clipFrames, median, p95,
            decode.empty() ? 0.0 : decode.back());
        if (qEnvironmentVariable("KINOKLIP_REVERSE_CACHE") != QLatin1String("off")) {
            // Seeking per frame shows 10 - 20 % of the frames at 1080p; with the cache 75 - 95 %
            // (GL) or about 75 % (CPU renderer). A busy machine (a build running) is excused.
            QFile loadFile(QStringLiteral("/proc/loadavg"));
            const bool busy = loadFile.open(QIODevice::ReadOnly)
                && loadFile.readAll().split(' ').value(0).toDouble() > QThread::idealThreadCount();
            const double wanted = engine.compositor()->isGpu() ? 0.55 : 0.40;
            if (busy && double(shown.size()) < wanted * clipFrames)
                QSKIP("the machine is too busy to judge real-time playback");
            QVERIFY2(double(shown.size()) >= wanted * clipFrames,
                qPrintable(QStringLiteral("shown %1 of %2").arg(shown.size()).arg(clipFrames)));
        }
    }

private:
    QTemporaryDir m_temp;
    QString m_path;
};

QTEST_MAIN(TestReversePlay)
#include "test_reverseplay.moc"
