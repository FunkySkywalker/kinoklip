// SPDX-License-Identifier: GPL-3.0-or-later
// Automation lanes (docs/behavior-spec.md section 138): volume, pan, mute, send and effect
// parameter curves in the mixer (same samples in any block size), point edits, saving.
#include "core/AudioEffectKeys.h"
#include "core/MixerJson.h"
#include "core/MixerOps.h"
#include "render/AudioMixer.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <numbers>
#include <vector>

using namespace kk;
using namespace kk::serialization;

namespace {

constexpr double kRate = 48000.0;
using Signal = std::vector<float>; ///< interleaved stereo

double tone(int i, double hz, double amp)
{
    return amp * std::sin(2.0 * std::numbers::pi * hz * i / kRate);
}

/// Level (dB) of the 1 kHz component of channel @p side (0 = left) in [from, to) seconds.
double sideDb(const Signal& s, int side, double fromSec, double toSec, double hz = 1000.0)
{
    const int a = int(fromSec * kRate), b = int(toSec * kRate);
    double re = 0.0, im = 0.0;
    for (int i = a; i < b; ++i) {
        const double w = 2.0 * std::numbers::pi * hz * i / kRate;
        re += double(s[size_t(i) * 2 + size_t(side)]) * std::cos(w);
        im += double(s[size_t(i) * 2 + size_t(side)]) * std::sin(w);
    }
    const double amp = 2.0 * std::sqrt(re * re + im * im) / (b - a);
    return 20.0 * std::log10(std::max(amp, 1e-12));
}

float peakOf(const Signal& s, size_t from, size_t to)
{
    float p = 0.0f;
    for (size_t i = from; i < to; ++i)
        p = std::max(p, std::abs(s[i]));
    return p;
}

double maxDifference(const Signal& a, const Signal& b)
{
    double d = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        d = std::max(d, double(std::abs(a[i] - b[i])));
    return d;
}

} // namespace

class TestAutomation : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    std::shared_ptr<MediaPathMap> m_paths = std::make_shared<MediaPathMap>();

    bool writeTone(const QString& name, double seconds, double hz, double amp)
    {
        const int n = int(seconds * kRate);
        Signal s(size_t(n) * 2);
        for (int i = 0; i < n; ++i)
            s[size_t(i) * 2] = s[size_t(i) * 2 + 1] = float(tone(i, hz, amp));
        QByteArray h;
        auto u32 = [&h](quint32 v) { h.append(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&h](quint16 v) { h.append(reinterpret_cast<const char*>(&v), 2); };
        const quint32 bytes = quint32(s.size() * 4);
        h.append("RIFF");
        u32(36 + bytes);
        h.append("WAVEfmt ");
        u32(16);
        u16(3);
        u16(2);
        u32(48000);
        u32(48000 * 8);
        u16(8);
        u16(32);
        h.append("data");
        u32(bytes);
        QFile f(m_dir.filePath(name));
        return f.open(QIODevice::WriteOnly) && f.write(h) == h.size()
            && f.write(reinterpret_cast<const char*>(s.data()), bytes) == qint64(bytes);
    }

    /// Two tracks, each playing the 1 kHz tone (0.25 = -12 dBFS) for 4 s.
    TimelineData timeline()
    {
        TimelineData tl
            = makeDefaultTimeline(QStringLiteral("automation"), Rational {25, 1}, 1920, 1080, 2, 2);
        for (int t = 0; t < 2; ++t) {
            ClipData c;
            c.id = tl.allocateId();
            c.kind = ClipKind::Media;
            c.mediaId = QStringLiteral("tone");
            c.start = 0;
            c.duration = 100;
            c.sourceDurationUs = 4'000'000;
            tl.audioTracks[size_t(t)].clips.push_back(c);
        }
        // Only the first track plays unless a test says otherwise.
        tl.audioTracks[1].muted = true;
        return tl;
    }

    Signal mixChunks(const TimelineData& tl, int frames, int block)
    {
        AudioMixer mixer;
        mixer.setMediaPaths(m_paths);
        Signal out(size_t(frames) * 2);
        for (int at = 0; at < frames; at += block) {
            const int f = std::min(block, frames - at);
            mixer.mix(tl, at, f, out.data() + size_t(at) * 2);
        }
        return out;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(writeTone(QStringLiteral("tone.wav"), 4.0, 1000.0, 0.25));
        (*m_paths)[QStringLiteral("tone")] = m_dir.filePath(QStringLiteral("tone.wav"));
    }

    // ---- 138.4 volume curve ----
    void volumeCurveChangesTheGainAtTheRightSamples()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        // 0 dB at frame 0, -20 dB at frame 50 (2 s), held after it.
        QVERIFY(mixerops::addAutomationPoint(tl, a1, mixkeys::kVolumeDb, 0, 0.0));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, mixkeys::kVolumeDb, 50, -20.0));
        const Signal out = mixChunks(tl, 192000, 512);
        // The tone is -12 dBFS; at 0.5 s the curve is -5 dB, at 1.0 s -10 dB, after 2 s -20 dB.
        QVERIFY2(std::abs(sideDb(out, 0, 0.45, 0.55) - (-17.0)) < 0.5,
            qPrintable(QString::number(sideDb(out, 0, 0.45, 0.55))));
        QVERIFY(std::abs(sideDb(out, 0, 0.95, 1.05) - (-22.0)) < 0.5);
        QVERIFY(std::abs(sideDb(out, 0, 2.5, 3.5) - (-32.0)) < 0.2);
        // Off ignores the curve: the static fader (0 dB) plays.
        QVERIFY(mixerops::setAutomationMode(tl, a1, AutomationMode::Off));
        QVERIFY(std::abs(sideDb(mixChunks(tl, 192000, 512), 0, 2.5, 3.5) - (-12.0)) < 0.2);
    }

    void curveGivesTheSameSamplesInAnyBlockSize()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        QVERIFY(mixerops::addAutomationPoint(tl, a1, mixkeys::kVolumeDb, 0, -30.0));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, mixkeys::kVolumeDb, 40, 0.0));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, mixkeys::kPan, 0, -1.0));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, mixkeys::kPan, 60, 1.0));
        const quint64 fx = mixerops::addTrackEffect(tl, a1, afxid::Gain);
        QVERIFY(fx != 0);
        QVERIFY(mixerops::addAutomationPoint(
            tl, a1, mixerops::effectKey(fx, QString::fromLatin1(afxkeys::GainDb)), 0, -6.0));
        QVERIFY(mixerops::addAutomationPoint(
            tl, a1, mixerops::effectKey(fx, QString::fromLatin1(afxkeys::GainDb)), 80, 6.0));
        const Signal a = mixChunks(tl, 192000, 512);
        const Signal b = mixChunks(tl, 192000, 1024);
        const Signal c = mixChunks(tl, 192000, 4096);
        QVERIFY2(maxDifference(a, b) < 1e-6, qPrintable(QString::number(maxDifference(a, b))));
        QVERIFY2(maxDifference(a, c) < 1e-6, qPrintable(QString::number(maxDifference(a, c))));
    }

    // ---- 138.4 pan curve ----
    void panCurveMovesTheSoundAcross()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        QVERIFY(mixerops::addAutomationPoint(tl, a1, mixkeys::kPan, 0, -1.0));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, mixkeys::kPan, 100, 1.0));
        const Signal out = mixChunks(tl, 192000, 512);
        // At the start the sound is left, at the end right, in the middle both are equal.
        QVERIFY(sideDb(out, 0, 0.02, 0.1) > sideDb(out, 1, 0.02, 0.1) + 20.0);
        QVERIFY(sideDb(out, 1, 3.9, 3.99) > sideDb(out, 0, 3.9, 3.99) + 20.0);
        QVERIFY(std::abs(sideDb(out, 0, 1.95, 2.05) - sideDb(out, 1, 1.95, 2.05)) < 0.5);
    }

    // ---- 138.4 effect parameter curve ----
    void effectParameterCurveChangesTheEffect()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        const quint64 fx = mixerops::addTrackEffect(tl, a1, afxid::Gain);
        QVERIFY(fx != 0);
        const QString key = mixerops::effectKey(fx, QString::fromLatin1(afxkeys::GainDb));
        // The lane offers the effect's parameters.
        const auto params = mixerops::automationParams(tl, tl.audioTracks[0]);
        bool found = false;
        for (const auto& p : params)
            found |= p.key == key;
        QVERIFY(found);
        QVERIFY(mixerops::addAutomationPoint(tl, a1, key, 0, 0.0));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, key, 50, -12.0));
        const Signal out = mixChunks(tl, 192000, 512);
        QVERIFY2(std::abs(sideDb(out, 0, 0.0, 0.1) - (-12.0)) < 0.5,
            qPrintable(QString::number(sideDb(out, 0, 0.0, 0.1))));
        QVERIFY2(std::abs(sideDb(out, 0, 3.0, 3.8) - (-24.0)) < 0.3,
            qPrintable(QString::number(sideDb(out, 0, 3.0, 3.8))));
        QVERIFY(std::abs(sideDb(out, 0, 0.95, 1.05) - (-18.0)) < 0.7);
    }

    // ---- 138.4 mute and send curves ----
    void muteCurveSilencesThePartAfterAStep()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        QVERIFY(mixerops::addAutomationPoint(tl, a1, mixkeys::kMute, 0, 0.0));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, mixkeys::kMute, 50, 1.0));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, mixkeys::kMute, 75, 0.0));
        const Signal out = mixChunks(tl, 192000, 512);
        QVERIFY(std::abs(sideDb(out, 0, 0.5, 1.5) - (-12.0)) < 0.2);
        QVERIFY(peakOf(out, size_t(2.2 * kRate) * 2, size_t(2.9 * kRate) * 2) < 1e-4f);
        QVERIFY(std::abs(sideDb(out, 0, 3.3, 3.9) - (-12.0)) < 0.2);
        QVERIFY(mixerops::mutedAt(tl.audioTracks[0], 60.0));
        QVERIFY(!mixerops::mutedAt(tl.audioTracks[0], 20.0));
    }

    void sendLevelCurveFollowsTheCurve()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        const int aux = mixerops::addBus(tl, false, {}, true);
        QVERIFY(mixerops::addSend(tl, MixTarget::track(a1), aux, 0.0, false));
        QVERIFY(mixerops::setOutputBus(tl, aux));
        const QString key = mixerops::sendKey(aux);
        QVERIFY(mixerops::addAutomationPoint(tl, a1, key, 0, 0.0));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, key, 50, -24.0));
        const Signal a = mixChunks(tl, 192000, 512);
        QVERIFY(std::abs(sideDb(a, 0, 3.0, 3.8) - (-36.0)) < 0.3);
        QVERIFY(std::abs(sideDb(a, 0, 0.0, 0.05) - (-12.0)) < 0.5);
        QVERIFY(maxDifference(a, mixChunks(tl, 192000, 1024)) < 1e-6);
    }

    // ---- 138.3 point edits ----
    void pointsAreAddedMovedAndDeleted()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        const QString k = mixkeys::kVolumeDb;
        QVERIFY(mixerops::addAutomationPoint(tl, a1, k, 10, -6.0));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, k, 30, -12.0));
        QVERIFY(!mixerops::addAutomationPoint(tl, a1, k, 30, -12.0)); // nothing changed
        QVERIFY(!mixerops::addAutomationPoint(tl, a1, QStringLiteral("nonsense"), 30, 0.0));
        const TrackData& t = tl.audioTracks[0];
        QVERIFY(mixerops::readsAutomation(t.mix, k));
        QCOMPARE(mixerops::faderDbAt(t, 20.0), -9.0); // linear between the points
        QVERIFY(mixerops::moveAutomationPoint(tl, a1, k, 30, 40, -3.0));
        QCOMPARE(mixerops::faderDbAt(t, 25.0), -4.5);
        QVERIFY(mixerops::moveAutomationPoint(tl, a1, k, 10, 40, 0.0)); // replaces the point at 40
        QCOMPARE(t.mix.automationKeys.values.value(k).keys.size(), qsizetype(1));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, k, 10, -20.0));
        // A segment moves both its points; values stay in range.
        QVERIFY(mixerops::moveAutomationSegment(tl, a1, k, 10, 40, 5.0));
        QCOMPARE(mixerops::faderDbAt(t, 10.0), -15.0);
        QCOMPARE(mixerops::faderDbAt(t, 40.0), 5.0);
        QVERIFY(mixerops::moveAutomationSegment(tl, a1, k, 10, 40, 100.0));
        QCOMPARE(mixerops::faderDbAt(t, 40.0), kFaderMaxDb);
        // Shape of a segment: Hold steps at the next point.
        QVERIFY(mixerops::setAutomationPointInterp(tl, a1, k, 10, Interp::Hold));
        QCOMPARE(mixerops::faderDbAt(t, 39.0), mixerops::faderDbAt(t, 10.0));
        QVERIFY(mixerops::removeAutomationPoint(tl, a1, k, 10));
        QVERIFY(mixerops::removeAutomationPoint(tl, a1, k, 40));
        // The last point takes the curve with it: the static fader plays again.
        QVERIFY(!mixerops::readsAutomation(t.mix, k));
        QCOMPARE(mixerops::faderDbAt(t, 25.0), 0.0);
        QVERIFY(!mixerops::removeAutomationPoint(tl, a1, k, 40));
    }

    // ---- 138.5 recording modes (the written results) ----
    void trimAddsAnOffsetToTheExistingCurve()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        const QString k = mixkeys::kVolumeDb;
        QVERIFY(mixerops::addAutomationPoint(tl, a1, k, 0, -10.0));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, k, 20, -10.0));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, k, 40, -20.0));
        const AnimatedValue original = tl.audioTracks[0].mix.automationKeys.values.value(k);
        // Trim from frame 10 to 30 by +6 dB, stepping like a playhead.
        QVERIFY(mixerops::trimAutomation(tl, a1, k, original, 10, 10, 0.0));
        for (int64_t f = 11; f <= 30; ++f)
            mixerops::trimAutomation(tl, a1, k, original, f - 1, f, 6.0);
        const TrackData& t = tl.audioTracks[0];
        QCOMPARE(mixerops::faderDbAt(t, 5.0), -10.0); // before the trim: untouched
        QCOMPARE(mixerops::faderDbAt(t, 15.0), -4.0); // original -10 plus 6
        QCOMPARE(mixerops::faderDbAt(t, 20.0), -4.0); // an original key inside, offset too
        QCOMPARE(mixerops::faderDbAt(t, 30.0), -9.0); // original -15 plus 6
        QCOMPARE(mixerops::faderDbAt(t, 40.0), -20.0); // after: the original again
    }

    // ---- 138.6 saving ----
    void automationSurvivesSaving()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        const int aux = mixerops::addBus(tl, false, {}, true);
        QVERIFY(mixerops::addSend(tl, MixTarget::track(a1), aux));
        const quint64 fx = mixerops::addTrackEffect(tl, a1, afxid::Gain);
        const QString fxKey = mixerops::effectKey(fx, QString::fromLatin1(afxkeys::GainDb));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, mixkeys::kVolumeDb, 5, -3.0));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, mixkeys::kPan, 5, 0.5));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, mixkeys::kMute, 9, 1.0));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, mixerops::sendKey(aux), 5, -9.0));
        QVERIFY(mixerops::addAutomationPoint(tl, a1, fxKey, 5, 3.0));
        QVERIFY(mixerops::setAutomationPointInterp(
            tl, a1, mixkeys::kVolumeDb, 5, Interp::Smooth));
        for (AutomationMode m : {AutomationMode::Latch, AutomationMode::Trim}) {
            QVERIFY(mixerops::setAutomationMode(tl, a1, m));
            const TrackMix back = trackMixFromJson(trackMixToJson(tl.audioTracks[0].mix));
            QCOMPARE(back.automation, m);
            QCOMPARE(back.automationKeys, tl.audioTracks[0].mix.automationKeys);
            QCOMPARE(back, tl.audioTracks[0].mix);
        }
        QCOMPARE(automationModeFromString(QStringLiteral("latch"), AutomationMode::Off),
            AutomationMode::Latch);
        QCOMPARE(automationModeFromString(QStringLiteral("trim"), AutomationMode::Off),
            AutomationMode::Trim);
    }

    void parameterListHasTheTrackParameters()
    {
        TimelineData tl = timeline();
        const quint64 a1 = tl.audioTracks[0].id;
        const int aux = mixerops::addBus(tl, false, {}, true);
        QVERIFY(mixerops::addSend(tl, MixTarget::track(a1), aux));
        QVERIFY(mixerops::addTrackEffect(tl, a1, afxid::Gain) != 0);
        const auto params = mixerops::automationParams(tl, tl.audioTracks[0]);
        QVERIFY(params.size() >= 5);
        QCOMPARE(params[0].key, mixkeys::kVolumeDb);
        QCOMPARE(params[1].key, mixkeys::kPan);
        QCOMPARE(params[2].key, mixkeys::kMute);
        QVERIFY(params[2].stepped);
        QCOMPARE(params[3].key, mixerops::sendKey(aux));
        int busId = 0;
        QVERIFY(mixerops::parseSendKey(params[3].key, busId));
        QCOMPARE(busId, aux);
        quint64 effectId = 0;
        QString param;
        QVERIFY(mixerops::parseEffectKey(params[4].key, effectId, param));
        QVERIFY(effectId != 0);
        QVERIFY(!mixerops::parseEffectKey(QStringLiteral("fx.1"), effectId, param));
        QVERIFY(!mixerops::parseSendKey(QStringLiteral("pan"), busId));
    }
};

QTEST_MAIN(TestAutomation)
#include "test_automation.moc"
