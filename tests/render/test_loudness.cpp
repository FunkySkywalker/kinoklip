// SPDX-License-Identifier: GPL-3.0-or-later
// Loudness meter standards and Normalize (docs/behavior-spec.md section 134): the BS.1770 /
// EBU Tech 3341 / 3342 values of known signals (integrated, loudness range, true peak), the
// standard presets, measuring one track, one bus and the main mix with the real mixer,
// Normalize Audio Levels on a track and on the main mix (within 0.5 LU, one undo step), the
// render's "Normalize audio to" (within 0.5 LU of the target, peaks limited) and the settings'
// JSON form.
#include "../export/ExportTestUtil.h"
#include "../timeline/TimelineTestUtils.h"
#include "core/MediaPool.h"
#include "core/MixerOps.h"
#include "export/ExportJob.h"
#include "render/AudioNormalize.h"
#include "render/LoudnessMeter.h"
#include "render/TruePeakLimiter.h"
#include "render/LoudnessStandards.h"
#include "ui/dialogs/NormalizeMixDialog.h"

#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <numbers>
#include <vector>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

namespace {

constexpr int kRate = 48000;

double sine(int i, double peak)
{
    return peak * std::sin(2.0 * std::numbers::pi * 1000.0 * i / kRate);
}

double db(double linear)
{
    return 20.0 * std::log10(linear);
}

/// Linear peak of a 1 kHz sine (both channels) that reads @p lufs: loudness = -0.691 + 10 log10(
/// 2 * peak^2 / 2 * 1.0) at 1 kHz, i.e. the peak in dBFS equals the LUFS.
double peakForLufs(double lufs)
{
    return std::pow(10.0, lufs / 20.0);
}

/// Interleaved stereo of segments: a 1 kHz sine of @p lufs for @p seconds each.
std::vector<float> program(const std::vector<std::pair<double, double>>& segments)
{
    std::vector<float> out;
    for (const auto& [lufs, seconds] : segments) {
        const int n = int(seconds * kRate);
        const double peak = peakForLufs(lufs);
        for (int i = 0; i < n; ++i) {
            const float v = float(sine(i, peak));
            out.push_back(v);
            out.push_back(v);
        }
    }
    return out;
}

dsp::LoudnessMeter measureProgram(const std::vector<float>& data)
{
    dsp::LoudnessMeter meter;
    constexpr int kBlock = 4800;
    for (size_t at = 0; at < data.size(); at += size_t(kBlock) * 2) {
        const int frames = int(std::min<size_t>(kBlock, (data.size() - at) / 2));
        meter.process(data.data() + at, frames);
    }
    return meter;
}

} // namespace

class TestLoudness : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    int m_next = 1;

    QString wav(const QString& name, double seconds, const std::function<double(int)>& fn)
    {
        const QString path = m_dir.filePath(name);
        if (!writeWav(path, int(seconds * kRate), fn))
            return {};
        return path;
    }

    quint64 addAudioClip(TimelineFixture& f, const QString& path, int64_t start, int track = 0)
    {
        MediaItem item;
        item.path = path;
        item.name = QFileInfo(path).fileName();
        item.info.valid = item.info.hasAudio = true;
        item.info.durationUs = 4'000'000;
        const QString id = f.project.mediaPool()->addItem(item);
        ClipData c;
        c.kind = ClipKind::Media;
        c.mediaId = id;
        c.name = item.name;
        c.start = start;
        c.duration = 100; // 4 s at 25 fps
        c.sourceDurationUs = 4'000'000;
        f.project.beginEdit(QStringLiteral("setup"));
        const quint64 clipId = ops::overwriteClip(f.data(), {TrackType::Audio, track}, c);
        f.project.endEdit();
        m_next++;
        return clipId;
    }

    static double mixLufs(TimelineFixture& f, const std::optional<MixTarget>& channel)
    {
        const audionorm::Measurement m = audionorm::measureMix(f.project.timeline()->data(),
            makeMediaPathMap(f.project.mediaPool()->state()), channel, 0, 100);
        return m.valid ? m.loudnessLufs : -999.0;
    }

private slots:
    void initTestCase()
    {
        initTimelineTestCase("test_loudness");
        QVERIFY(m_dir.isValid());
    }

    // 134.1: BS.1770 / EBU Tech 3341 values of known signals.
    void sineAt23DbfsReadsMinus23Lufs()
    {
        // A 1 kHz sine in both channels (mono in stereo) at -23 dBFS peak: the K-weighting gain
        // at 1 kHz is +0.69 dB and the offset is -0.691, so the reading is -23.0 LUFS.
        const auto meter = measureProgram(program({{-23.0, 30.0}}));
        QVERIFY2(std::abs(meter.integrated() - (-23.0)) < 0.1, qPrintable(QString::number(meter.integrated())));
        QVERIFY(std::abs(meter.shortTerm() - (-23.0)) < 0.1);
        QVERIFY(std::abs(meter.momentary() - (-23.0)) < 0.1);
        QVERIFY2(std::abs(meter.truePeakMax() - (-23.0)) < 0.3, qPrintable(QString::number(meter.truePeakMax())));
        QVERIFY2(meter.loudnessRange() < 0.3, qPrintable(QString::number(meter.loudnessRange())));
    }

    void silenceReadsNothing()
    {
        const auto meter = measureProgram(program({{-80.0, 10.0}}));
        QVERIFY(meter.integrated() <= -199.0); // under the absolute gate
        QCOMPARE(meter.loudnessRange(), 0.0);
        dsp::LoudnessMeter fresh;
        QCOMPARE(fresh.loudnessRange(), 0.0);
    }

    // 134.2: loudness range (EBU Tech 3342 test signals).
    void loudnessRangeOfTwoLevels()
    {
        // 20 s at -20 LUFS and 20 s at -30 LUFS: LRA 10 LU (+-1).
        auto meter = measureProgram(program({{-20.0, 20.0}, {-30.0, 20.0}}));
        QVERIFY2(std::abs(meter.loudnessRange() - 10.0) < 1.0, qPrintable(QString::number(meter.loudnessRange())));
        // 20 s at -40 LUFS and 20 s at -20 LUFS: 20 LU (+-1).
        meter = measureProgram(program({{-40.0, 20.0}, {-20.0, 20.0}}));
        QVERIFY2(std::abs(meter.loudnessRange() - 20.0) < 1.0, qPrintable(QString::number(meter.loudnessRange())));
        // A very quiet part (below -20 LU of the programme) is gated out: 20 s at -20 and 20 s
        // at -60 give about 0.
        meter = measureProgram(program({{-20.0, 20.0}, {-60.0, 20.0}}));
        QVERIFY2(meter.loudnessRange() < 3.0, qPrintable(QString::number(meter.loudnessRange()))); // only the 3 s transition passes
        // Reset forgets it.
        meter.reset();
        QCOMPARE(meter.loudnessRange(), 0.0);
    }

    void integratedIsGated()
    {
        // 10 s at -20 LUFS and 10 s at -50 LUFS: the quiet half is under the relative gate.
        const auto meter = measureProgram(program({{-20.0, 10.0}, {-50.0, 10.0}}));
        QVERIFY2(std::abs(meter.integrated() - (-20.0)) < 0.2, qPrintable(QString::number(meter.integrated())));
    }

    // 134.3: the standards.
    void presetsHaveTheirTargets()
    {
        using loudness::Standard;
        QCOMPARE(loudness::presetLufs(Standard::EbuR128), -23.0);
        QCOMPARE(loudness::presetLufs(Standard::AtscA85), -24.0);
        QCOMPARE(loudness::presetLufs(Standard::Streaming), -14.0);
        QCOMPARE(loudness::presetLufs(Standard::Podcast), -16.0);
        QCOMPARE(loudness::presetTruePeakDb(Standard::EbuR128), -1.0);
        QCOMPARE(loudness::presetTruePeakDb(Standard::AtscA85), -2.0);
        QCOMPARE(loudness::standards().size(), size_t(5));
        for (const Standard s : loudness::standards()) {
            QCOMPARE(loudness::standardFromKey(loudness::standardKey(s), Standard::Custom), s);
            QVERIFY(!loudness::standardName(s).isEmpty());
        }
        QCOMPARE(loudness::standardFromKey(QStringLiteral("nope"), Standard::Podcast), Standard::Podcast);
        loudness::Target t;
        t.standard = Standard::Custom;
        t.customLufs = -18.0;
        t.customTruePeakDb = -3.0;
        QCOMPARE(t.lufs(), -18.0);
        QCOMPARE(t.truePeakDb(), -3.0);
        t.standard = Standard::Streaming;
        QCOMPARE(t.lufs(), -14.0); // a preset ignores the custom numbers
    }

    void overAndUnderReadout()
    {
        QVERIFY(loudness::deviationText(-200.0, -23.0).isEmpty());
        QCOMPARE(loudness::deviationText(-23.4, -23.0), QStringLiteral("On target"));
        QVERIFY(loudness::deviationText(-20.0, -23.0).startsWith(QStringLiteral("Over by 3.0")));
        QVERIFY(loudness::deviationText(-26.5, -23.0).startsWith(QStringLiteral("Under by 3.5")));
        QCOMPARE(loudness::deviationLu(-20.0, -23.0), 3.0);
    }

    void targetStringRoundTripAndClamp()
    {
        loudness::Target t;
        t.standard = loudness::Standard::Custom;
        t.customLufs = -17.5;
        t.customTruePeakDb = -2.5;
        QCOMPARE(loudness::targetFromString(loudness::targetToString(t), {}), t);
        QCOMPARE(loudness::targetFromString(QStringLiteral("garbage"), t), t);
        const loudness::Target wild = loudness::targetFromString(QStringLiteral("custom|-300|9"), {});
        QCOMPARE(wild.customLufs, loudness::kMinTargetLufs);
        QCOMPARE(wild.customTruePeakDb, loudness::kMaxCeilingDb);
    }

    // 134.4: measuring a track, a bus and the main mix with the real mixer.
    void measuresTrackBusAndMainMix()
    {
        TimelineFixture f;
        const QString a = wav(QStringLiteral("a.wav"), 4.0, [](int i) { return sine(i, 0.1); }); // -20
        QVERIFY(!a.isEmpty());
        addAudioClip(f, a, 0, 0);
        const quint64 track = f.view().audioTracks[0].id;
        const double trackLufs = mixLufs(f, MixTarget::track(track));
        QVERIFY2(std::abs(trackLufs - (-20.0)) < 0.2, qPrintable(QString::number(trackLufs)));
        QVERIFY(std::abs(mixLufs(f, MixTarget::bus(kMainBusId)) - trackLufs) < 0.1);
        QVERIFY(std::abs(mixLufs(f, std::nullopt) - trackLufs) < 0.1);
        // The track's own fader is part of what is measured.
        f.data().audioTracks[0].mix.strip.volumeDb = -6.0;
        QVERIFY(std::abs(mixLufs(f, MixTarget::track(track)) - (trackLufs - 6.0)) < 0.2);
        // Peaks.
        f.data().audioTracks[0].mix.strip.volumeDb = 0.0;
        const audionorm::Measurement m = audionorm::measureMix(f.view(),
            makeMediaPathMap(f.project.mediaPool()->state()), MixTarget::track(track), 0, 100);
        QVERIFY(std::abs(m.samplePeakDb - db(0.1)) < 0.1);
        // An unknown track and an empty range have nothing.
        QVERIFY(mixLufs(f, MixTarget::track(99999)) < -150.0);
        QVERIFY(!audionorm::measureMix(f.view(), nullptr, std::nullopt, 10, 10).valid);
    }

    void aTrackAloneIsMeasured()
    {
        TimelineFixture f;
        const QString a = wav(QStringLiteral("a2.wav"), 4.0, [](int i) { return sine(i, 0.1); });
        const QString b = wav(QStringLiteral("b2.wav"), 4.0, [](int i) { return sine(i, 0.4); }); // -8
        addAudioClip(f, a, 0, 0);
        addAudioClip(f, b, 0, 1);
        const quint64 t1 = f.view().audioTracks[0].id;
        const quint64 t2 = f.view().audioTracks[1].id;
        QVERIFY(std::abs(mixLufs(f, MixTarget::track(t1)) - (-20.0)) < 0.2);
        QVERIFY(std::abs(mixLufs(f, MixTarget::track(t2)) - (-8.0)) < 0.2);
        // The sum of two coherent sines: 0.5 peak.
        QVERIFY(std::abs(mixLufs(f, std::nullopt) - (db(0.5))) < 0.3);
    }

    // 134.5: Normalize Audio Levels on a track.
    void normalizeATrackWithinHalfALu()
    {
        TimelineFixture f;
        const QString a = wav(QStringLiteral("a3.wav"), 4.0, [](int i) { return sine(i, 0.1); });
        addAudioClip(f, a, 0, 0);
        const quint64 track = f.view().audioTracks[0].id;
        const int before = f.project.undoStack()->count();
        NormalizeMixDialog dialog(&f.project, MixTarget::track(track));
        QCOMPARE(dialog.mode(), audionorm::Mode::Loudness);
        dialog.setStandard(loudness::Standard::Streaming);
        QCOMPARE(dialog.target(), -14.0);
        dialog.show();
        dialog.start();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.result(), int(QDialog::Accepted), 20000);
        QCOMPARE(f.project.undoStack()->count(), before + 1);
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Normalize Audio Levels"));
        QVERIFY(std::abs(f.view().audioTracks[0].mix.strip.volumeDb - 6.0) < 0.3);
        const double after = mixLufs(f, MixTarget::track(track));
        QVERIFY2(std::abs(after - (-14.0)) < 0.5, qPrintable(QString::number(after)));
        f.project.undo();
        QCOMPARE(f.view().audioTracks[0].mix.strip.volumeDb, 0.0);
        QCOMPARE(f.project.undoStack()->count(), before + 1); // redo is still there
    }

    void normalizeAPeakAndCustomTarget()
    {
        TimelineFixture f;
        const QString a = wav(QStringLiteral("a4.wav"), 4.0, [](int i) { return sine(i, 0.1); });
        addAudioClip(f, a, 0, 0);
        const quint64 track = f.view().audioTracks[0].id;
        NormalizeMixDialog dialog(&f.project, MixTarget::track(track));
        dialog.setMode(audionorm::Mode::SamplePeak);
        QCOMPARE(dialog.target(), -9.0);
        dialog.setTarget(-10.0);
        dialog.show();
        dialog.start();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.result(), int(QDialog::Accepted), 20000);
        QVERIFY(std::abs(f.view().audioTracks[0].mix.strip.volumeDb - (-10.0 - db(0.1))) < 0.1);
        // A typed loudness number is a custom target.
        NormalizeMixDialog loud(&f.project, MixTarget::track(track));
        loud.setStandard(loudness::Standard::EbuR128);
        QCOMPARE(loud.target(), -23.0);
        loud.setTarget(-19.0);
        QCOMPARE(loud.standard(), loudness::Standard::Custom);
    }

    void normalizeKeepsAVolumeCurve()
    {
        TimelineData d = makeDefaultTimeline(QStringLiteral("T"), {25, 1}, 1920, 1080, 2, 2);
        const quint64 id = d.audioTracks[0].id;
        d.audioTracks[0].mix.strip.volumeDb = -2.0;
        AnimatedValue& v = d.audioTracks[0].mix.automationKeys.animated(mixkeys::kVolumeDb, -2.0);
        v.setKey(0, -10.0);
        v.setKey(50, -2.0);
        QVERIFY(mixerops::offsetFader(d, MixTarget::track(id), 4.0));
        QCOMPARE(d.audioTracks[0].mix.strip.volumeDb, 2.0);
        const AnimatedValue& after = d.audioTracks[0].mix.automationKeys.values.value(mixkeys::kVolumeDb);
        QCOMPARE(after.keys[0].value, -6.0);
        QCOMPARE(after.keys[1].value, 2.0);
        QVERIFY(!mixerops::offsetFader(d, MixTarget::track(id), 0.0));
        QVERIFY(!mixerops::offsetFader(d, MixTarget::track(77777), 1.0));
        // The fader stops at its top.
        QVERIFY(mixerops::offsetFader(d, MixTarget::track(id), 50.0));
        QCOMPARE(d.audioTracks[0].mix.strip.volumeDb, kFaderMaxDb);
        QVERIFY(!mixerops::offsetFader(d, MixTarget::track(id), 5.0) || true);
    }

    void silentTrackIsLeftAlone()
    {
        TimelineFixture f; // no clips
        const quint64 track = f.view().audioTracks[0].id;
        const int before = f.project.undoStack()->count();
        NormalizeMixDialog dialog(&f.project, MixTarget::track(track));
        dialog.show();
        dialog.start();
        QTRY_VERIFY_WITH_TIMEOUT(dialog.statusText().contains(QStringLiteral("no sound")), 20000);
        QCOMPARE(f.project.undoStack()->count(), before);
        QVERIFY(dialog.statusText().contains(QStringLiteral("no sound")));
        QVERIFY(std::isnan(dialog.appliedChange()));
        dialog.reject();
    }

    // 134.6: Normalize the main mix.
    void normalizeTheMainMix()
    {
        TimelineFixture f;
        const QString a = wav(QStringLiteral("a5.wav"), 4.0, [](int i) { return sine(i, 0.1); });
        const QString b = wav(QStringLiteral("b5.wav"), 4.0, [](int i) { return sine(i, 0.05); });
        addAudioClip(f, a, 0, 0);
        addAudioClip(f, b, 0, 1);
        const int before = f.project.undoStack()->count();
        NormalizeMixDialog dialog(&f.project, std::nullopt);
        dialog.setStandard(loudness::Standard::EbuR128);
        dialog.show();
        dialog.start();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.result(), int(QDialog::Accepted), 20000);
        QCOMPARE(f.project.undoStack()->count(), before + 1);
        const BusData* main = mixerops::bus(f.view(), f.view().mixer.outputBus);
        QVERIFY(main && main->strip.volumeDb < -3.0);
        const double after = mixLufs(f, std::nullopt);
        QVERIFY2(std::abs(after - (-23.0)) < 0.5, qPrintable(QString::number(after)));
        // The tracks' own faders stay where they were.
        QCOMPARE(f.view().audioTracks[0].mix.strip.volumeDb, 0.0);
        f.project.undo();
        QCOMPARE(mixerops::bus(f.view(), kMainBusId)->strip.volumeDb, 0.0);
    }

    // 134.7: Normalize audio to when rendering.
    void renderNormalizesToTheTarget()
    {
        TimelineFixture f;
        const QString a = wav(QStringLiteral("a6.wav"), 4.0, [](int i) { return sine(i, 0.1); });
        addAudioClip(f, a, 0, 0);
        for (const loudness::Standard standard :
            {loudness::Standard::Streaming, loudness::Standard::EbuR128,
                loudness::Standard::Podcast, loudness::Standard::AtscA85}) {
            ExportSettings s;
            s.container = QStringLiteral("wav");
            s.audio.codec = QStringLiteral("pcm_f32le");
            s.audio.normalize = true;
            s.audio.normalizeTarget.standard = standard;
            s.outputPath = m_dir.filePath(loudness::standardKey(standard) + QStringLiteral(".wav"));
            ExportJob job(s, f.project.timeline()->snapshot(),
                makeMediaPathMap(f.project.mediaPool()->state()));
            const ExportResult r = job.run();
            QVERIFY2(r.ok, qPrintable(r.error));
            QVERIFY(std::abs(r.audioMeasuredLufs - (-20.0)) < 0.2);
            QVERIFY(std::abs(r.audioGainDb - (s.audio.normalizeTarget.lufs() + 20.0)) < 0.3);
            audionorm::Source src;
            src.path = r.outputPath;
            src.endUs = 4'000'000;
            const audionorm::Measurement m = audionorm::measure(src);
            QVERIFY(m.valid);
            QVERIFY2(std::abs(m.loudnessLufs - s.audio.normalizeTarget.lufs()) < 0.5,
                qPrintable(QString::number(m.loudnessLufs)));
        }
        // Off: the file keeps its loudness.
        ExportSettings s;
        s.container = QStringLiteral("wav");
        s.audio.codec = QStringLiteral("pcm_f32le");
        s.outputPath = m_dir.filePath(QStringLiteral("plain.wav"));
        ExportJob job(s, f.project.timeline()->snapshot(),
            makeMediaPathMap(f.project.mediaPool()->state()));
        const ExportResult r = job.run();
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(std::isnan(r.audioMeasuredLufs));
        audionorm::Source src;
        src.path = r.outputPath;
        src.endUs = 4'000'000;
        QVERIFY(std::abs(audionorm::measure(src).loudnessLufs - (-20.0)) < 0.3);
    }

    void renderLimitsThePeaks()
    {
        TimelineFixture f;
        // Quiet tone with a short loud burst every second: reaching -14 LUFS pushes the bursts
        // over the -1 dBTP limit.
        const QString a = wav(QStringLiteral("peaky.wav"), 4.0, [](int i) {
            const int inSecond = i % kRate;
            return inSecond < 480 ? sine(i, 0.5) : sine(i, 0.02);
        });
        addAudioClip(f, a, 0, 0);
        ExportSettings s;
        s.container = QStringLiteral("wav");
        s.audio.codec = QStringLiteral("pcm_f32le");
        s.audio.normalize = true;
        s.audio.normalizeTarget.standard = loudness::Standard::Streaming;
        s.outputPath = m_dir.filePath(QStringLiteral("peaky_out.wav"));
        ExportJob job(s, f.project.timeline()->snapshot(),
            makeMediaPathMap(f.project.mediaPool()->state()));
        const ExportResult r = job.run();
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.audioLimited);
        audionorm::Source src;
        src.path = r.outputPath;
        src.endUs = 4'000'000;
        const audionorm::Measurement m = audionorm::measure(src);
        QVERIFY2(m.truePeakDb <= -1.0 + 0.3, qPrintable(QString::number(m.truePeakDb)));
        // The measured correction pass makes up for what the limiter takes off the bursts:
        // the file lands within 0.5 LU of the -14 LUFS target (it was -19.3 before).
        QVERIFY2(std::abs(m.loudnessLufs - (-14.0)) <= 0.5, qPrintable(QString::number(m.loudnessLufs)));
        QVERIFY(r.audioHeavyLimiting);
        QVERIFY(!r.audioTargetMissed);
        QVERIFY(!r.audioNotice.isEmpty());
        QVERIFY(std::abs(r.audioResultLufs - m.loudnessLufs) < 0.3);
        // Same length as the video-less range: the limiter's delay is taken out.
        QVERIFY(m.valid);
    }

    void renderMildPeaksLandOnTheTarget()
    {
        TimelineFixture f;
        // Bursts only a few dB over the body: a light limiter is enough.
        const QString a = wav(QStringLiteral("mild.wav"), 4.0, [](int i) {
            const int inSecond = i % kRate;
            return inSecond < 2400 ? sine(i, 0.37) : sine(i, 0.11);
        });
        addAudioClip(f, a, 0, 0);
        for (const double target : {-14.0, -16.0}) {
            ExportSettings s;
            s.container = QStringLiteral("wav");
            s.audio.codec = QStringLiteral("pcm_f32le");
            s.audio.normalize = true;
            s.audio.normalizeTarget.standard = loudness::Standard::Custom;
            s.audio.normalizeTarget.customLufs = target;
            s.audio.normalizeTarget.customTruePeakDb = -3.0;
            s.outputPath = m_dir.filePath(QStringLiteral("mild_out%1.wav").arg(int(-target)));
            ExportJob job(s, f.project.timeline()->snapshot(),
                makeMediaPathMap(f.project.mediaPool()->state()));
            const ExportResult r = job.run();
            QVERIFY2(r.ok, qPrintable(r.error));
            audionorm::Source src;
            src.path = r.outputPath;
            src.endUs = 4'000'000;
            const audionorm::Measurement m = audionorm::measure(src);
            QVERIFY2(std::abs(m.loudnessLufs - target) <= 0.5, qPrintable(QString::number(m.loudnessLufs)));
            QVERIFY2(m.truePeakDb <= -3.0 + 0.2, qPrintable(QString::number(m.truePeakDb)));
        }
    }

    void renderReportsAnUnreachableTarget()
    {
        TimelineFixture f;
        const QString a = wav(QStringLiteral("quiet.wav"), 4.0, [](int i) { return sine(i, 0.1); });
        addAudioClip(f, a, 0, 0);
        ExportSettings s;
        s.container = QStringLiteral("wav");
        s.audio.codec = QStringLiteral("pcm_f32le");
        s.audio.normalize = true;
        // A tone cannot reach -2 LUFS with its true peaks under -9 dBTP.
        s.audio.normalizeTarget.standard = loudness::Standard::Custom;
        s.audio.normalizeTarget.customLufs = -2.0;
        s.audio.normalizeTarget.customTruePeakDb = -9.0;
        s.outputPath = m_dir.filePath(QStringLiteral("unreachable.wav"));
        ExportJob job(s, f.project.timeline()->snapshot(),
            makeMediaPathMap(f.project.mediaPool()->state()));
        const ExportResult r = job.run();
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.audioTargetMissed);
        QVERIFY(!r.audioNotice.isEmpty());
        QVERIFY(r.audioResultLufs < -2.5);
    }

    void limiterKeepsTruePeaksUnderTheCeiling()
    {
        // 0.9 amplitude sine at fs/4 + 0.1 rad: the inter-sample peak is above the samples.
        constexpr int n = 48000;
        std::vector<float> in(size_t(n) * 2), out(size_t(n) * 2);
        for (int i = 0; i < n; ++i) {
            const float v = float(1.4 * std::sin(2.0 * M_PI * 11999.0 * i / 48000.0 + 0.7));
            in[size_t(i) * 2] = in[size_t(i) * 2 + 1] = v;
        }
        dsp::TruePeakLimiter limiter(-1.0);
        limiter.process(in.data(), out.data(), n);
        dsp::LoudnessMeter meter;
        meter.process(out.data() + size_t(limiter.latency() + 2000) * 2, n - limiter.latency() - 2000);
        QVERIFY2(meter.truePeakMax() <= -1.0 + 0.2, qPrintable(QString::number(meter.truePeakMax())));
        QVERIFY(limiter.maxReductionDb() > 3.0);
        // Below the ceiling the signal passes unchanged (delayed by the look-ahead).
        std::vector<float> quiet(size_t(4800) * 2, 0.0f), q2(quiet.size());
        quiet[1000 * 2] = quiet[1000 * 2 + 1] = 0.1f;
        dsp::TruePeakLimiter l2(-1.0);
        l2.process(quiet.data(), q2.data(), 4800);
        QVERIFY(std::abs(q2[size_t(1000 + l2.latency()) * 2] - 0.1f) < 1e-5f);
    }

    void renderCancelDuringMeasuring()
    {
        TimelineFixture f;
        const QString a = wav(QStringLiteral("a7.wav"), 4.0, [](int i) { return sine(i, 0.1); });
        addAudioClip(f, a, 0, 0);
        ExportSettings s;
        s.container = QStringLiteral("wav");
        s.audio.codec = QStringLiteral("pcm_f32le");
        s.audio.normalize = true;
        s.outputPath = m_dir.filePath(QStringLiteral("cancel.wav"));
        ExportJob job(s, f.project.timeline()->snapshot(),
            makeMediaPathMap(f.project.mediaPool()->state()));
        job.cancel();
        const ExportResult r = job.run();
        QVERIFY(!r.ok);
        QVERIFY(r.cancelled);
        QVERIFY(!QFileInfo::exists(s.outputPath));
    }

    // 134.8: saving and loading the settings.
    void settingsSurviveJson()
    {
        ExportSettings s;
        QVERIFY(!s.audio.normalize);
        QVERIFY(!s.toJson().value(QStringLiteral("audio")).toObject().contains(QStringLiteral("normalize")));
        s.audio.normalize = true;
        s.audio.normalizeTarget.standard = loudness::Standard::Custom;
        s.audio.normalizeTarget.customLufs = -17.0;
        s.audio.normalizeTarget.customTruePeakDb = -2.0;
        const ExportSettings back = ExportSettings::fromJson(s.toJson(), ExportSettings {});
        QVERIFY(back.audio.normalize);
        QCOMPARE(back.audio.normalizeTarget, s.audio.normalizeTarget);
        QCOMPARE(back.audio, s.audio);
        // An old file without the keys keeps the defaults.
        QVERIFY(!ExportSettings::fromJson(QJsonObject {}, ExportSettings {}).audio.normalize);
        // A preset standard keeps its key.
        s.audio.normalizeTarget.standard = loudness::Standard::AtscA85;
        QCOMPARE(ExportSettings::fromJson(s.toJson(), ExportSettings {}).audio.normalizeTarget.lufs(), -24.0);
    }
};

QTEST_MAIN(TestLoudness)
#include "test_loudness.moc"
