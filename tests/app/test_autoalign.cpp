// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of Auto Align Clips (issue 16, spec section 153): the clip.align command and the code
// under it (ui/timeline/AutoAlign) on synthetic recordings of one "scene" with known offsets:
// noise, a different gain, partial overlap, negative offsets, linked clips that move together,
// the unreliable and the collision refusals, the muted reference, timecode, undo.
#include "app/commands/Command.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/timeline/AutoAlign.h"
#include "ui/timeline/TimelineEditUtil.h"

#include <QFile>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace kk;
using namespace kk::commands;

namespace {

constexpr int kRate = 48000;
constexpr int kFps = 24;

/// Speech-like bursts and claps, deterministic for a seed (same idea as test_audiosync).
std::vector<float> scene(double seconds, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::vector<float> out(size_t(seconds * kRate), 0.0f);
    size_t pos = 0;
    while (pos < out.size()) {
        pos += size_t((0.03 + 0.25 * uni(rng)) * kRate);
        const auto len = size_t((0.04 + 0.26 * uni(rng)) * kRate);
        const double level = 0.05 + 0.4 * uni(rng);
        const double alpha = 0.2 + 0.7 * uni(rng);
        double state = 0.0;
        for (size_t i = 0; i < len && pos + i < out.size(); ++i) {
            const double env = std::sin(M_PI * double(i) / double(len));
            state = alpha * state + (1.0 - alpha) * gauss(rng);
            out[pos + i] += float(level * env * state);
        }
        pos += len;
        if (uni(rng) < 0.05 && pos < out.size()) {
            for (size_t i = 0; i < size_t(0.005 * kRate) && pos + i < out.size(); ++i)
                out[pos + i] += float(0.8 * std::exp(-double(i) / 60.0) * gauss(rng));
        }
    }
    return out;
}

/// [from, from + seconds) of @p src with @p gain and white noise of @p noise RMS.
std::vector<float> record(const std::vector<float>& src, double from, double seconds, double gain,
    double noise, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::vector<float> out(size_t(seconds * kRate));
    const auto offset = int64_t(std::llround(from * kRate));
    for (size_t i = 0; i < out.size(); ++i) {
        const int64_t s = offset + int64_t(i);
        const double x = s >= 0 && s < int64_t(src.size()) ? src[size_t(s)] : 0.0;
        out[i] = float(gain * x + noise * gauss(rng));
    }
    return out;
}

/// A mono 16-bit 48 kHz WAV file.
bool writeWav(const QString& file, const std::vector<float>& samples)
{
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    const uint32_t bytes = uint32_t(samples.size() * 2);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(kRate);
    u32(kRate * 2);
    u16(2);
    u16(16);
    f.write("data", 4);
    u32(bytes);
    for (float s : samples) {
        const auto v = int16_t(std::lround(std::clamp(s, -1.0f, 1.0f) * 32000.0f));
        f.write(reinterpret_cast<const char*>(&v), 2);
    }
    return true;
}

CommandResult call(const QString& name, QJsonObject args)
{
    return CommandRegistry::builtin().execute(name, args);
}

} // namespace

class TestAutoAlign : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    QString m_project;
    std::vector<float> m_scene; ///< 90 s of "the event"
    int m_files = 0;

    QString path(const QString& name) const { return m_dir.filePath(name); }

    CommandResult ok(const QString& name, QJsonObject args)
    {
        args.insert(QStringLiteral("project"), m_project);
        const CommandResult r = call(name, args);
        [&] { QVERIFY2(r.ok, qPrintable(name + ": " + r.errorCode + " " + r.error)); }();
        return r;
    }
    CommandResult run(const QString& name, QJsonObject args)
    {
        args.insert(QStringLiteral("project"), m_project);
        return call(name, args);
    }
    /// A fresh project with an extra audio track and video track (V1 V2, A1 A2).
    void makeProject()
    {
        m_project = path(QStringLiteral("p%1.kkproj").arg(++m_files));
        QVERIFY(call(QStringLiteral("project.new"),
            {{"project", m_project}, {"force", true}, {"width", 320}, {"height", 180},
                {"fps", kFps}})
                    .ok);
        ok(QStringLiteral("track.add"), {{"type", "audio"}});
        ok(QStringLiteral("track.add"), {{"type", "video"}});
    }
    /// A WAV file of [from, from + seconds) of the scene.
    QString wav(double from, double seconds, double gain = 1.0, double noise = 0.0)
    {
        const QString file = path(QStringLiteral("rec%1.wav").arg(++m_files));
        [&] {
            QVERIFY(writeWav(file, record(m_scene, from, seconds, gain, noise, 7u + m_files)));
        }();
        return file;
    }
    /// An mp4 (black picture + the WAV's sound), optionally with a start timecode.
    QString mp4(const QString& wavFile, const QString& timecode = {})
    {
        const QString file = path(QStringLiteral("vid%1.mp4").arg(++m_files));
        QStringList args {"-y", "-loglevel", "error", "-f", "lavfi", "-i",
            "color=c=black:s=160x90:r=24", "-i", wavFile, "-c:v", "mpeg4", "-c:a", "aac",
            "-shortest"};
        if (!timecode.isEmpty())
            args << "-timecode" << timecode;
        args << file;
        QProcess p;
        p.start(QStringLiteral("ffmpeg"), args);
        p.waitForFinished(60000);
        return p.exitCode() == 0 ? file : QString();
    }
    qint64 place(const QString& file, const QString& track, int at)
    {
        const CommandResult r
            = ok(QStringLiteral("clip.add"), {{"media", file}, {"track", track}, {"at", at}});
        for (const QJsonValue& v : r.data["clips"].toArray()) {
            if (v.toObject()["track"].toString() == track)
                return v.toObject()["id"].toInteger();
        }
        return 0;
    }
    QJsonObject clipOn(const QString& track)
    {
        const auto list = call(QStringLiteral("clip.list"),
            {{"project", m_project}, {"track", track}}).data["clips"].toArray();
        return list.isEmpty() ? QJsonObject() : list.first().toObject();
    }
    QJsonObject entryOf(const CommandResult& r, qint64 clip)
    {
        for (const QJsonValue& v : r.data["clips"].toArray()) {
            if (v.toObject()["clip"].toInteger() == clip)
                return v.toObject();
        }
        return {};
    }

private slots:
    void initTestCase() { m_scene = scene(90.0, 11u); }

    void knownOffsets_data()
    {
        QTest::addColumn<double>("refFrom");
        QTest::addColumn<double>("refLength");
        QTest::addColumn<double>("otherFrom");
        QTest::addColumn<double>("otherLength");
        QTest::addColumn<double>("gain");
        QTest::addColumn<double>("noise");
        // Offset = otherFrom - refFrom: the other recording starts that long after the reference.
        QTest::newRow("later start") << 0.0 << 40.0 << 7.345 << 25.0 << 1.0 << 0.0;
        QTest::newRow("earlier start") << 10.0 << 30.0 << 6.5 << 40.0 << 1.0 << 0.0;
        QTest::newRow("quiet and noisy") << 5.0 << 50.0 << 12.0011 << 30.0 << 0.1 << 0.006;
        QTest::newRow("loud") << 0.0 << 30.0 << 3.21 << 20.0 << 4.0 << 0.01;
        QTest::newRow("partial overlap") << 0.0 << 30.0 << 22.0 << 30.0 << 0.5 << 0.01;
    }
    void knownOffsets()
    {
        QFETCH(double, refFrom);
        QFETCH(double, refLength);
        QFETCH(double, otherFrom);
        QFETCH(double, otherLength);
        QFETCH(double, gain);
        QFETCH(double, noise);
        makeProject();
        const qint64 ref = place(wav(refFrom, refLength), QStringLiteral("A1"), 300);
        const qint64 other = place(wav(otherFrom, otherLength, gain, noise), QStringLiteral("A2"), 5);
        const CommandResult r = ok(QStringLiteral("clip.align"),
            {{"reference", ref}, {"clip", QJsonArray {QString::number(other)}}});
        const QJsonObject e = entryOf(r, other);
        QCOMPARE(e["status"].toString(), QStringLiteral("aligned"));
        const int64_t expected = 300 + int64_t(std::llround((otherFrom - refFrom) * kFps));
        QCOMPARE(clipOn(QStringLiteral("A2"))["start"].toInteger(), qint64(expected));
        QCOMPARE(e["newStart"].toInteger(), qint64(expected));
        QVERIFY(e["confidence"].toDouble() > 0.2);
        QCOMPARE(clipOn(QStringLiteral("A1"))["start"].toInteger(), qint64(300)); // reference stays
        QCOMPARE(r.data["method"].toString(), QStringLiteral("waveform"));
    }

    void unrelatedSoundIsNotMoved()
    {
        makeProject();
        const qint64 ref = place(wav(0, 30), QStringLiteral("A1"), 100);
        // Another event altogether.
        const QString file = path(QStringLiteral("other-event.wav"));
        QVERIFY(writeWav(file, scene(20.0, 99u)));
        const qint64 other = place(file, QStringLiteral("A2"), 30);
        const CommandResult r = run(QStringLiteral("clip.align"),
            {{"reference", ref}, {"clip", QJsonArray {QString::number(other)}}});
        QVERIFY(!r.ok);
        QCOMPARE(r.errorCode, QStringLiteral("unreliable"));
        QVERIFY(!r.error.isEmpty());
        QCOMPARE(clipOn(QStringLiteral("A2"))["start"].toInteger(), qint64(30));
    }

    void oneGoodOneUnreliable()
    {
        makeProject();
        ok(QStringLiteral("track.add"), {{"type", "audio"}});
        const qint64 ref = place(wav(0, 40), QStringLiteral("A1"), 100);
        const qint64 good = place(wav(5, 20), QStringLiteral("A2"), 0);
        const QString file = path(QStringLiteral("noise-only.wav"));
        QVERIFY(writeWav(file, scene(15.0, 5u)));
        const qint64 bad = place(file, QStringLiteral("A3"), 0);
        const CommandResult r = ok(QStringLiteral("clip.align"),
            {{"reference", ref},
                {"clip", QJsonArray {QString::number(good), QString::number(bad)}}});
        QCOMPARE(entryOf(r, good)["status"].toString(), QStringLiteral("aligned"));
        QCOMPARE(entryOf(r, bad)["status"].toString(), QStringLiteral("unreliable"));
        QVERIFY(!entryOf(r, bad)["reason"].toString().isEmpty());
        QCOMPARE(r.data["aligned"].toInt(), 1);
        QCOMPARE(r.data["unreliable"].toInt(), 1);
        QCOMPARE(clipOn(QStringLiteral("A2"))["start"].toInteger(), qint64(100 + 5 * kFps));
        QCOMPARE(clipOn(QStringLiteral("A3"))["start"].toInteger(), qint64(0));
    }

    void cameraClipIsTheReferenceAndLinkedClipsMoveTogether()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg is not installed");
        makeProject();
        const QString camera = mp4(wav(0, 30, 0.2, 0.005));
        const QString second = mp4(wav(4, 20));
        QVERIFY(!camera.isEmpty() && !second.isEmpty());
        const qint64 camId = place(camera, QStringLiteral("V1"), 240); // V1 + A1, linked
        const qint64 otherVideo = place(second, QStringLiteral("V2"), 3); // V2 + A2, linked
        const qint64 external = place(wav(2, 25, 1.0, 0.003), QStringLiteral("A3"), 2);
        (void)external;
        // No reference given: the clip on the lowest video track (V1) is it.
        const CommandResult r = ok(QStringLiteral("clip.align"),
            {{"clip", QJsonArray {QString::number(otherVideo), QString::number(camId)}}});
        QCOMPARE(entryOf(r, otherVideo)["status"].toString(), QStringLiteral("aligned"));
        QCOMPARE(r.data["reference"].toInteger(), camId);
        const qint64 expected = 240 + 4 * kFps;
        QCOMPARE(clipOn(QStringLiteral("V2"))["start"].toInteger(), expected);
        QCOMPARE(clipOn(QStringLiteral("A2"))["start"].toInteger(), expected); // its sound followed
        QCOMPARE(clipOn(QStringLiteral("V1"))["start"].toInteger(), qint64(240));
        QCOMPARE(clipOn(QStringLiteral("A1"))["start"].toInteger(), qint64(240));
        QVERIFY(!r.data["muted"].toBool());
        QVERIFY(clipOn(QStringLiteral("A1"))["volumeDb"].toDouble() > -90.0);
    }

    void externalRecordingAndMutedReference()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg is not installed");
        makeProject();
        const QString camera = mp4(wav(0, 30, 0.2, 0.005));
        QVERIFY(!camera.isEmpty());
        const qint64 camId = place(camera, QStringLiteral("V1"), 100);
        const qint64 external = place(wav(2, 25, 1.0, 0.003), QStringLiteral("A2"), 0);
        const CommandResult r = ok(QStringLiteral("clip.align"),
            {{"reference", camId}, {"clip", QJsonArray {QString::number(external)}},
                {"mute-reference", true}});
        QCOMPARE(clipOn(QStringLiteral("A2"))["start"].toInteger(), qint64(100 + 2 * kFps));
        QVERIFY(r.data["muted"].toBool());
        QVERIFY(clipOn(QStringLiteral("A1"))["volumeDb"].toDouble() <= -90.0);
        QCOMPARE(clipOn(QStringLiteral("V1"))["enabled"].toBool(), true);
        // The camera sound at the same position as before.
        QCOMPARE(clipOn(QStringLiteral("A1"))["start"].toInteger(), qint64(100));
    }

    void overwritingIsRefused()
    {
        makeProject();
        const qint64 ref = place(wav(0, 40), QStringLiteral("A1"), 100);
        const qint64 other = place(wav(5, 20), QStringLiteral("A2"), 0);
        // A third clip sits where the aligned clip would land (100 + 120 = frame 220).
        place(wav(60, 10), QStringLiteral("A2"), 230);
        const CommandResult r = run(QStringLiteral("clip.align"),
            {{"reference", ref}, {"clip", QJsonArray {QString::number(other)}}});
        QVERIFY(!r.ok);
        QCOMPARE(r.errorCode, QStringLiteral("cannot_align"));
        QVERIFY(r.error.contains(QLatin1String("overwrite")));
        QCOMPARE(clipOn(QStringLiteral("A2"))["start"].toInteger(), qint64(0));
    }

    void startingBeforeTheTimelineIsRefused()
    {
        makeProject();
        const qint64 ref = place(wav(10, 30), QStringLiteral("A1"), 10);
        // Sounds 8 s before the reference starts: frame 10 - 192 < 0.
        const qint64 other = place(wav(2, 20), QStringLiteral("A2"), 50);
        const CommandResult r = run(QStringLiteral("clip.align"),
            {{"reference", ref}, {"clip", QJsonArray {QString::number(other)}}});
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains(QLatin1String("before the beginning")));
        QCOMPARE(clipOn(QStringLiteral("A2"))["start"].toInteger(), qint64(50));
    }

    void alreadyAlignedIsNotAnEdit()
    {
        makeProject();
        const qint64 ref = place(wav(0, 40), QStringLiteral("A1"), 100);
        const qint64 other = place(wav(5, 20), QStringLiteral("A2"), 100 + 5 * kFps);
        const CommandResult r = ok(QStringLiteral("clip.align"),
            {{"reference", ref}, {"clip", QJsonArray {QString::number(other)}}});
        QCOMPARE(entryOf(r, other)["status"].toString(), QStringLiteral("unchanged"));
        QCOMPARE(r.data["aligned"].toInt(), 0);
    }

    void badArguments()
    {
        makeProject();
        const qint64 ref = place(wav(0, 20), QStringLiteral("A1"), 0);
        QCOMPARE(run(QStringLiteral("clip.align"), {{"clip", QJsonArray {"99999"}}}).errorCode,
            QStringLiteral("unknown_clip"));
        QCOMPARE(run(QStringLiteral("clip.align"),
                     {{"reference", 99999}, {"clip", QJsonArray {QString::number(ref)}}})
                     .errorCode,
            QStringLiteral("unknown_clip"));
        // Only the reference itself: nothing to line up.
        QCOMPARE(run(QStringLiteral("clip.align"), {{"clip", QJsonArray {QString::number(ref)}}})
                     .errorCode,
            QStringLiteral("bad_value"));
        QVERIFY(!run(QStringLiteral("clip.align"),
            {{"clip", QJsonArray {QString::number(ref)}}, {"method", "fuzzy"}})
                     .ok);
        // The command is in the registry with typed parameters (CLI and MCP are built from it).
        const Command* c = CommandRegistry::builtin().find(QStringLiteral("clip.align"));
        QVERIFY(c);
        QVERIFY(!c->spec.params.isEmpty());
    }

    void timecodeMethod()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg is not installed");
        makeProject();
        const QString a = mp4(wav(0, 10), QStringLiteral("01:00:00:00"));
        const QString b = mp4(wav(40, 10), QStringLiteral("01:00:02:12"));
        QVERIFY(!a.isEmpty() && !b.isEmpty());
        const qint64 ref = place(a, QStringLiteral("V1"), 100);
        const qint64 other = place(b, QStringLiteral("V2"), 7);
        const CommandResult r = run(QStringLiteral("clip.align"),
            {{"reference", ref}, {"clip", QJsonArray {QString::number(other)}},
                {"method", "timecode"}});
        if (!r.ok && r.error.contains(QLatin1String("timecode")))
            QSKIP("this ffmpeg did not write a start timecode");
        QVERIFY2(r.ok, qPrintable(r.errorCode + " " + r.error));
        QCOMPARE(clipOn(QStringLiteral("V2"))["start"].toInteger(), qint64(100 + 2 * kFps + 12));
        QCOMPARE(clipOn(QStringLiteral("A2"))["start"].toInteger(), qint64(100 + 2 * kFps + 12));
        QCOMPARE(r.data["method"].toString(), QStringLiteral("timecode"));
        // No timecode in plain WAV files: skipped, not guessed.
        const qint64 w = place(wav(0, 5), QStringLiteral("A1"), 600);
        const CommandResult none = run(QStringLiteral("clip.align"),
            {{"reference", ref}, {"clip", QJsonArray {QString::number(w)}}, {"method", "timecode"}});
        QVERIFY(!none.ok);
    }

    void oneUndoStep()
    {
        makeProject();
        const qint64 ref = place(wav(0, 40), QStringLiteral("A1"), 100);
        const qint64 other = place(wav(5, 20), QStringLiteral("A2"), 0);
        Project project;
        QString error;
        QVERIFY2(project.load(m_project, &error), qPrintable(error));
        autoalign::Request request;
        request.reference = quint64(ref);
        request.clips = {quint64(other)};
        QString code;
        const auto job = autoalign::prepare(
            project.timeline()->data(), *project.mediaPool(), request, &error, &code);
        QVERIFY2(job.has_value(), qPrintable(error));
        autoalign::Result result = autoalign::analyse(*job);
        QCOMPARE(result.count(autoalign::Status::Aligned), 1);
        // analyse() changes nothing.
        QCOMPARE(editops::clipById(project.timeline()->data(), quint64(other))->start, int64_t(0));
        QVERIFY(timelineedit::runEdit(&project, QStringLiteral("Auto Align Clips"),
            [&](TimelineData& data) { return autoalign::apply(data, *job, result); }));
        QCOMPARE(editops::clipById(project.timeline()->data(), quint64(other))->start,
            int64_t(100 + 5 * kFps));
        project.undo();
        QCOMPARE(editops::clipById(project.timeline()->data(), quint64(other))->start, int64_t(0));
        QCOMPARE(editops::clipById(project.timeline()->data(), quint64(ref))->start, int64_t(100));
    }

    void lockedTrackIsRefused()
    {
        makeProject();
        const qint64 ref = place(wav(0, 40), QStringLiteral("A1"), 100);
        const qint64 other = place(wav(5, 20), QStringLiteral("A2"), 0);
        ok(QStringLiteral("track.set"), {{"track", "A2"}, {"locked", true}});
        const CommandResult r = run(QStringLiteral("clip.align"),
            {{"reference", ref}, {"clip", QJsonArray {QString::number(other)}}});
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains(QLatin1String("locked")));
    }
};

QTEST_MAIN(TestAutoAlign)
#include "test_autoalign.moc"
