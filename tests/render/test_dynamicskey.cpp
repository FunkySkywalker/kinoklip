// SPDX-License-Identifier: GPL-3.0-or-later
// Gate / Expander, key filters, side-chain ducking, De-Esser and Dialogue Leveler
// (docs/behavior-spec.md section 136).
#include "core/AudioEffectKeys.h"
#include "core/EffectRegistry.h"
#include "core/MixerJson.h"
#include "core/MixerOps.h"
#include "core/Serialization.h"
#include "export/ExportJob.h"
#include "render/AudioMixer.h"
#include "render/AudioNormalize.h"
#include "render/DeEsser.h"
#include "render/Dynamics.h"
#include "render/RenderTypes.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <functional>
#include <numbers>
#include <random>
#include <vector>

using namespace kk;
using namespace kk::dsp;
using namespace kk::serialization;

namespace {

constexpr double kRate = 48000.0;

using Signal = std::vector<float>; ///< interleaved stereo

Signal make(double seconds, const std::function<double(int)>& fn)
{
    const int n = int(seconds * kRate);
    Signal s(size_t(n) * 2);
    for (int i = 0; i < n; ++i)
        s[size_t(i) * 2] = s[size_t(i) * 2 + 1] = float(fn(i));
    return s;
}

double tone(int i, double hz, double amp)
{
    return amp * std::sin(2.0 * std::numbers::pi * hz * i / kRate);
}

double fromDb(double db)
{
    return std::pow(10.0, db / 20.0);
}

double rmsDb(const Signal& s, double fromSec, double toSec)
{
    const int a = int(fromSec * kRate), b = int(toSec * kRate);
    double sum = 0.0;
    for (int i = a; i < b; ++i)
        sum += double(s[size_t(i) * 2]) * s[size_t(i) * 2];
    return 10.0 * std::log10(std::max(sum / std::max(b - a, 1), 1e-20));
}

/// Level (dB) of the component at @p hz in [from, to) (a Goertzel sum, amplitude of a sine).
double componentDb(const Signal& s, double hz, double fromSec, double toSec)
{
    const int a = int(fromSec * kRate), b = int(toSec * kRate);
    double re = 0.0, im = 0.0;
    for (int i = a; i < b; ++i) {
        const double w = 2.0 * std::numbers::pi * hz * i / kRate;
        re += double(s[size_t(i) * 2]) * std::cos(w);
        im += double(s[size_t(i) * 2]) * std::sin(w);
    }
    const double amp = 2.0 * std::sqrt(re * re + im * im) / (b - a);
    return 20.0 * std::log10(std::max(amp, 1e-12));
}

void run(Dynamics& d, Signal& s, int block, const Signal* key = nullptr)
{
    const int n = int(s.size() / 2);
    for (int at = 0; at < n; at += block) {
        const int f = std::min(block, n - at);
        d.process(s.data() + size_t(at) * 2, f, key ? key->data() + size_t(at) * 2 : nullptr);
    }
}

} // namespace

class TestDynamicsKey : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    std::shared_ptr<MediaPathMap> m_paths = std::make_shared<MediaPathMap>();

    bool writeWav(const QString& name, double seconds, const std::function<double(int)>& fn)
    {
        const Signal s = make(seconds, fn);
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

    /// Music (A1, 440 Hz at -12 dBFS) and a voice (A2, 1 kHz at -12 dBFS from 1 s to 2 s).
    TimelineData duckingTimeline()
    {
        TimelineData tl = makeDefaultTimeline(QStringLiteral("duck"), Rational {25, 1}, 1920, 1080, 2, 2);
        int track = 0;
        for (const QString& id : {QStringLiteral("music"), QStringLiteral("voice")}) {
            ClipData c;
            c.id = tl.allocateId();
            c.kind = ClipKind::Media;
            c.mediaId = id;
            c.start = 0;
            c.duration = 100;
            c.sourceDurationUs = 4'000'000;
            tl.audioTracks[size_t(track++)].clips.push_back(c);
        }
        return tl;
    }

    static void keyMusicByVoice(TimelineData& tl)
    {
        DynamicsSettings& d = tl.audioTracks[0].mix.strip.dynamics;
        d.compressorEnabled = true;
        d.thresholdDb = -40.0;
        d.ratio = 10.0;
        d.attackMs = 5.0;
        d.releaseMs = 100.0;
        QVERIFY(mixerops::setKeySource(tl, MixTarget::track(tl.audioTracks[0].id), KeyKind::Track,
            tl.audioTracks[1].id));
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
        QVERIFY(writeWav(QStringLiteral("music.wav"), 4.0, [](int i) { return tone(i, 440, 0.25); }));
        QVERIFY(writeWav(QStringLiteral("voice.wav"), 4.0, [](int i) {
            return i >= 48000 && i < 96000 ? tone(i, 1000, 0.25) : 0.0;
        }));
        (*m_paths)[QStringLiteral("music")] = m_dir.filePath(QStringLiteral("music.wav"));
        (*m_paths)[QStringLiteral("voice")] = m_dir.filePath(QStringLiteral("voice.wav"));
    }

    // ---- 135.1 gate ----
    void gateLowersBelowTheThresholdByTheRange()
    {
        DynamicsSettings s;
        s.gateEnabled = true;
        s.gateThresholdDb = -40.0;
        s.gateRangeDb = 20.0;
        s.gateAttackMs = 1.0;
        s.gateReleaseMs = 20.0;
        s.gateHoldMs = 0.0;
        // Quiet tone (-55 dBFS peak, under the threshold): lowered by exactly the Range.
        Signal quiet = make(1.0, [](int i) { return tone(i, 1000, fromDb(-55)); });
        const double before = rmsDb(quiet, 0.5, 1.0);
        Dynamics d(kRate);
        d.setTarget(s);
        run(d, quiet, 480);
        QVERIFY2(std::abs((rmsDb(quiet, 0.5, 1.0) - before) - (-20.0)) < 0.5,
            qPrintable(QString::number(rmsDb(quiet, 0.5, 1.0) - before)));
        // A tone over the threshold passes untouched.
        Signal loud = make(1.0, [](int i) { return tone(i, 1000, fromDb(-20)); });
        const double loudBefore = rmsDb(loud, 0.5, 1.0);
        Dynamics d2(kRate);
        d2.setTarget(s);
        run(d2, loud, 480);
        QVERIFY(std::abs(rmsDb(loud, 0.5, 1.0) - loudBefore) < 0.1);
        // Switched off: nothing changes.
        s.gateEnabled = false;
        Signal off = make(0.5, [](int i) { return tone(i, 1000, fromDb(-55)); });
        const Signal copy = off;
        Dynamics d3(kRate);
        d3.setTarget(s);
        run(d3, off, 480);
        QCOMPARE(off, copy);
    }

    void gateHoldAndReleaseTiming()
    {
        DynamicsSettings s;
        s.gateEnabled = true;
        s.gateThresholdDb = -40.0;
        s.gateRangeDb = 40.0;
        s.gateAttackMs = 1.0;
        s.gateHoldMs = 100.0;
        s.gateReleaseMs = 50.0;
        s.gateHysteresisDb = 0.0;
        // Loud for 0.5 s, then a quiet tone (-60 dBFS): the key falls under the threshold.
        Signal x = make(1.5, [](int i) { return i < 24000 ? tone(i, 1000, 0.3) : tone(i, 1000, fromDb(-60)); });
        const Signal in = x;
        Dynamics d(kRate);
        d.setTarget(s);
        run(d, x, 256);
        auto gainDb = [&](double t) { // gain of a 5 ms window at time t
            return rmsDb(x, t, t + 0.005) - rmsDb(in, t, t + 0.005);
        };
        QVERIFY(std::abs(gainDb(0.45)) < 0.2);
        // During the hold (0.5 .. 0.6 s) the gate is still open.
        QVERIFY2(std::abs(gainDb(0.55)) < 1.0, qPrintable(QString::number(gainDb(0.55))));
        // One release time constant after the hold: 1 - 1/e of the range (25.3 dB), within 3 dB.
        const double oneTau = gainDb(0.6 + 0.035 + 0.050); // 35 ms: the key envelope falls first
        QVERIFY2(std::abs(oneTau - (-25.3)) < 3.0, qPrintable(QString::number(oneTau)));
        // Long after: the full range.
        QVERIFY(std::abs(gainDb(1.4) - (-40.0)) < 1.0);
    }

    void gateHysteresisKeepsItOpen()
    {
        DynamicsSettings s;
        s.gateEnabled = true;
        s.gateThresholdDb = -30.0;
        s.gateRangeDb = 40.0;
        s.gateHoldMs = 0.0;
        s.gateReleaseMs = 20.0;
        s.gateHysteresisDb = 10.0;
        // Opens with a loud start, then sits at -35 dB: between threshold and threshold - 10.
        Signal x = make(1.0, [](int i) { return i < 4800 ? tone(i, 1000, 0.3) : tone(i, 1000, fromDb(-35)); });
        const Signal in = x;
        Dynamics d(kRate);
        d.setTarget(s);
        run(d, x, 128);
        QVERIFY(std::abs(rmsDb(x, 0.5, 0.9) - rmsDb(in, 0.5, 0.9)) < 0.3); // stays open
        s.gateHysteresisDb = 0.0;
        Signal y = in;
        Dynamics d2(kRate);
        d2.setTarget(s);
        run(d2, y, 128);
        QVERIFY(rmsDb(y, 0.5, 0.9) - rmsDb(in, 0.5, 0.9) < -30.0); // closes
    }

    // ---- 135.2 expander ----
    void expanderFollowsTheRatio()
    {
        DynamicsSettings s;
        s.gateEnabled = true;
        s.gateMode = GateMode::Expander;
        s.gateThresholdDb = -30.0;
        s.gateRangeDb = 60.0;
        s.gateRatio = 3.0;
        s.gateHoldMs = 0.0;
        s.gateReleaseMs = 10.0;
        // Peak -40 dBFS = 10 dB under the threshold: loses 10 * (3 - 1) = 20 dB.
        Signal x = make(1.0, [](int i) { return tone(i, 1000, fromDb(-40)); });
        const double before = rmsDb(x, 0.5, 1.0);
        Dynamics d(kRate);
        d.setTarget(s);
        run(d, x, 480);
        const double lost = rmsDb(x, 0.5, 1.0) - before;
        QVERIFY2(std::abs(lost - (-20.0)) < 1.0, qPrintable(QString::number(lost)));
        // The static curve (the one the window draws).
        QVERIFY(std::abs(gateGainReductionDb(-40, -30, 60, 3, GateMode::Expander) - 20.0) < 1e-9);
        QVERIFY(std::abs(gateGainReductionDb(-80, -30, 30, 3, GateMode::Expander) - 30.0) < 1e-9);
        QCOMPARE(gateGainReductionDb(-20, -30, 60, 3, GateMode::Expander), 0.0);
        QCOMPARE(gateGainReductionDb(-80, -30, 25, 3, GateMode::Gate), 25.0);
    }

    // ---- 135.3 key filters ----
    void keyFiltersIgnoreRumble()
    {
        DynamicsSettings s;
        s.gateEnabled = true;
        s.gateThresholdDb = -30.0;
        s.gateRangeDb = 30.0;
        s.gateHoldMs = 0.0;
        s.gateReleaseMs = 20.0;
        // A loud 60 Hz rumble: with no key filter it holds the gate open.
        Signal rumble = make(1.0, [](int i) { return tone(i, 60, 0.3); });
        const Signal in = rumble;
        Dynamics d(kRate);
        d.setTarget(s);
        run(d, rumble, 256);
        QVERIFY(std::abs(rmsDb(rumble, 0.5, 1.0) - rmsDb(in, 0.5, 1.0)) < 0.3);
        // With the key low cut at 1 kHz the key does not hear it: the gate closes (the sound itself is not filtered).
        s.keyLowCutHz = 1000.0;
        Signal y = in;
        Dynamics d2(kRate);
        d2.setTarget(s);
        run(d2, y, 256);
        QVERIFY(rmsDb(y, 0.5, 1.0) - rmsDb(in, 0.5, 1.0) < -25.0);
        // The high cut does the same for a hiss.
        s.keyLowCutHz = kKeyMinHz;
        s.keyHighCutHz = 500.0;
        Signal hiss = make(1.0, [](int i) { return tone(i, 8000, 0.3); });
        const Signal hissIn = hiss;
        Dynamics d3(kRate);
        d3.setTarget(s);
        run(d3, hiss, 256);
        QVERIFY(rmsDb(hiss, 0.5, 1.0) - rmsDb(hissIn, 0.5, 1.0) < -25.0);
    }

    // ---- 135.4 side chain ----
    void compressorKeyedByAnotherSignal()
    {
        DynamicsSettings s;
        s.compressorEnabled = true;
        s.thresholdDb = -40.0;
        s.ratio = 10.0;
        s.attackMs = 2.0;
        s.releaseMs = 50.0;
        Signal music = make(1.0, [](int i) { return tone(i, 440, 0.25); }); // -12 dBFS
        Signal voice = make(1.0, [](int i) { return i >= 24000 ? tone(i, 1000, 0.25) : 0.0; });
        const Signal in = music;
        Dynamics d(kRate);
        d.setTarget(s);
        run(d, music, 300, &voice);
        // Voice absent (0.1 - 0.4 s): the music is not touched (its own level would have been compressed).
        QVERIFY(std::abs(rmsDb(music, 0.1, 0.4) - rmsDb(in, 0.1, 0.4)) < 0.2);
        // Voice present (0.7 - 1 s): over the threshold by 28 dB, 0.9 * 28 = 25 dB less.
        QVERIFY2(rmsDb(music, 0.7, 1.0) - rmsDb(in, 0.7, 1.0) < -20.0,
            qPrintable(QString::number(rmsDb(music, 0.7, 1.0) - rmsDb(in, 0.7, 1.0))));
        // Without the key the same music is compressed by its own level.
        Signal self = in;
        Dynamics d2(kRate);
        d2.setTarget(s);
        run(d2, self, 300);
        QVERIFY(rmsDb(self, 0.1, 0.4) - rmsDb(in, 0.1, 0.4) < -15.0);
    }

    void duckingIsTheSameInEveryBlockSize()
    {
        TimelineData tl = duckingTimeline();
        keyMusicByVoice(tl);
        const int frames = 4 * 48000;
        const Signal whole = mixChunks(tl, frames, frames);
        // The music (440 Hz) alone: before the voice, during it and after it.
        auto music = [&](const Signal& s, double a, double b) { return componentDb(s, 440, a, b); };
        const double before = music(whole, 0.2, 0.8);
        const double during = music(whole, 1.3, 1.9);
        const double after = music(whole, 3.0, 3.8);
        QVERIFY2(before - during > 20.0, qPrintable(QString::number(before - during)));
        QVERIFY2(std::abs(before - after) < 0.5, qPrintable(QString::number(before - after)));
        for (const int block : {64, 480, 1000, 4096}) {
            const Signal chunks = mixChunks(tl, frames, block);
            double worst = 0.0;
            for (size_t i = 0; i < whole.size(); ++i)
                worst = std::max(worst, double(std::abs(whole[i] - chunks[i])));
            QVERIFY2(worst < 1e-5, qPrintable(QString("block %1: %2").arg(block).arg(worst)));
        }
        // A seek into the ducked stretch gives the same samples as playing up to it.
        AudioMixer mixer;
        mixer.setMediaPaths(m_paths);
        Signal seek(size_t(24000) * 2);
        mixer.mix(tl, 72000, 24000, seek.data());
        double worst = 0.0;
        for (size_t i = 4800; i < seek.size(); ++i) // the envelopes restart: equal after 50 ms
            worst = std::max(worst, double(std::abs(seek[i] - whole[size_t(72000) * 2 + i])));
        QVERIFY2(worst < 0.01, qPrintable(QString::number(worst)));
    }

    void renderMatchesPlayback()
    {
        TimelineData tl = duckingTimeline();
        keyMusicByVoice(tl);
        ExportSettings s;
        s.container = QStringLiteral("wav");
        s.audio.codec = QStringLiteral("pcm_f32le");
        s.outputPath = m_dir.filePath(QStringLiteral("ducked.wav"));
        ExportJob job(s, std::make_shared<const TimelineData>(tl), m_paths);
        const ExportResult r = job.run();
        QVERIFY2(r.ok, qPrintable(r.error));
        audionorm::Source src;
        src.path = r.outputPath;
        src.endUs = 4'000'000;
        const audionorm::Measurement file = audionorm::measure(src);
        const audionorm::Measurement mix = audionorm::measureMix(tl, m_paths, std::nullopt, 0, 100);
        QVERIFY(file.valid && mix.valid);
        QVERIFY2(std::abs(file.loudnessLufs - mix.loudnessLufs) < 0.05,
            qPrintable(QString("%1 %2").arg(file.loudnessLufs).arg(mix.loudnessLufs)));
        // And the ducking is in the file: the same timeline without the key is louder.
        TimelineData plain = duckingTimeline();
        const audionorm::Measurement unducked = audionorm::measureMix(plain, m_paths, std::nullopt, 0, 100);
        QVERIFY2(mix.loudnessLufs < unducked.loudnessLufs - 0.4,
            qPrintable(QString("%1 %2").arg(mix.loudnessLufs).arg(unducked.loudnessLufs)));
    }

    void trackKeyedByABus()
    {
        // The voice (A2) feeds Bus 2 as well as the main bus; the music (A1) is keyed by Bus 2.
        TimelineData tl = duckingTimeline();
        const int bus2 = mixerops::addBus(tl, false);
        QVERIFY(mixerops::setTrackRoute(tl, tl.audioTracks[1].id, bus2, true));
        DynamicsSettings& m = tl.audioTracks[0].mix.strip.dynamics;
        m.compressorEnabled = true;
        m.thresholdDb = -40.0;
        m.ratio = 10.0;
        m.releaseMs = 100.0;
        QVERIFY(mixerops::setKeySource(tl, MixTarget::track(tl.audioTracks[0].id), KeyKind::Bus, bus2));
        // The music does not feed Bus 2, so there is no loop; feeding it would be one.
        QVERIFY(!mixerops::keySourceAllowed(tl, MixTarget::track(tl.audioTracks[1].id), KeyKind::Bus, bus2));
        const int frames = 4 * 48000;
        const Signal whole = mixChunks(tl, frames, frames);
        const double duck = componentDb(whole, 440, 0.2, 0.8) - componentDb(whole, 440, 1.3, 1.9);
        QVERIFY2(duck > 15.0, qPrintable(QString::number(duck)));
        QVERIFY(mixChunks(tl, frames, 333) == mixChunks(tl, frames, 333));
        // A bus keyed by a track (the main bus keyed by the voice): the mix is lowered while it speaks.
        TimelineData tl2 = duckingTimeline();
        DynamicsSettings& d = mixerops::bus(tl2, kMainBusId)->strip.dynamics;
        d.compressorEnabled = true;
        d.thresholdDb = -40.0;
        d.ratio = 10.0;
        QVERIFY(mixerops::setKeySource(tl2, MixTarget::bus(kMainBusId), KeyKind::Track, tl2.audioTracks[1].id));
        const Signal b = mixChunks(tl2, frames, 4096);
        QVERIFY(componentDb(b, 440, 0.2, 0.8) - componentDb(b, 440, 1.3, 1.9) > 15.0);
    }

    // ---- 135.5 no loops ----
    void loopsAreRefused()
    {
        TimelineData tl = duckingTimeline();
        const quint64 a1 = tl.audioTracks[0].id, a2 = tl.audioTracks[1].id;
        // Not itself.
        QVERIFY(!mixerops::keySourceAllowed(tl, MixTarget::track(a1), KeyKind::Track, a1));
        QVERIFY(!mixerops::keySourceAllowed(tl, MixTarget::bus(kMainBusId), KeyKind::Bus, kMainBusId));
        // A1 keyed by A2; then A2 cannot be keyed by A1.
        QVERIFY(mixerops::setKeySource(tl, MixTarget::track(a1), KeyKind::Track, a2));
        QVERIFY(!mixerops::keySourceAllowed(tl, MixTarget::track(a2), KeyKind::Track, a1));
        QVERIFY(!mixerops::setKeySource(tl, MixTarget::track(a2), KeyKind::Track, a1));
        QCOMPARE(tl.audioTracks[1].mix.strip.dynamics.keyKind, KeyKind::Self);
        // A track cannot key itself through a bus it feeds (A1 feeds the main bus).
        QVERIFY(!mixerops::keySourceAllowed(tl, MixTarget::track(a2), KeyKind::Bus, kMainBusId));
        // A bus keyed by a track that feeds it: allowed (no loop: the track does not depend on the bus).
        QVERIFY(mixerops::keySourceAllowed(tl, MixTarget::bus(kMainBusId), KeyKind::Track, a2));
        // Unknown sources.
        QVERIFY(!mixerops::keySourceAllowed(tl, MixTarget::track(a1), KeyKind::Track, 424242));
        QVERIFY(!mixerops::keySourceAllowed(tl, MixTarget::track(a1), KeyKind::Bus, 99));
        // A loop that came from a damaged file is ignored when mixing (it neither hangs nor keys).
        tl.audioTracks[1].mix.strip.dynamics.keyKind = KeyKind::Track;
        tl.audioTracks[1].mix.strip.dynamics.keyId = a1;
        QCOMPARE(mixerops::effectiveKey(tl, MixTarget::track(a2)).keyKind, KeyKind::Self);
        QCOMPARE(mixerops::effectiveKey(tl, MixTarget::track(a1)).keyKind, KeyKind::Self);
        const Signal out = mixChunks(tl, 48000, 4096);
        QVERIFY(!out.empty());
    }

    // ---- 135.6 save and load ----
    void settingsSurviveJson()
    {
        ChannelStrip s;
        s.dynamics.gateEnabled = true;
        s.dynamics.gateMode = GateMode::Expander;
        s.dynamics.gateThresholdDb = -33.5;
        s.dynamics.gateRangeDb = 22.0;
        s.dynamics.gateRatio = 3.5;
        s.dynamics.gateAttackMs = 2.5;
        s.dynamics.gateHoldMs = 120.0;
        s.dynamics.gateReleaseMs = 300.0;
        s.dynamics.gateHysteresisDb = 6.0;
        s.dynamics.keyKind = KeyKind::Track;
        s.dynamics.keyId = 7;
        s.dynamics.keyLowCutHz = 150.0;
        s.dynamics.keyHighCutHz = 9000.0;
        const ChannelStrip back = stripFromJson(stripToJson(s));
        QCOMPARE(back, s);
        s.dynamics.keyKind = KeyKind::Bus;
        s.dynamics.keyId = 3;
        QCOMPARE(stripFromJson(stripToJson(s)).dynamics.keyKind, KeyKind::Bus);
        // Defaults are not written; an old file loads with the defaults.
        QVERIFY(stripToJson(ChannelStrip {}).isEmpty());
        QCOMPARE(stripFromJson(QJsonObject {}), ChannelStrip {});
        // Out-of-range values are clamped.
        QJsonObject o = stripToJson(s);
        QJsonObject dyn = o.value(QStringLiteral("dynamics")).toObject();
        dyn[QStringLiteral("gateRange")] = 500.0;
        dyn[QStringLiteral("gateHold")] = -5.0;
        o[QStringLiteral("dynamics")] = dyn;
        const ChannelStrip clamped = stripFromJson(o);
        QCOMPARE(clamped.dynamics.gateRangeDb, kGateMaxRangeDb);
        QCOMPARE(clamped.dynamics.gateHoldMs, 0.0);
        // Through the whole timeline file.
        TimelineData tl = duckingTimeline();
        keyMusicByVoice(tl);
        tl.audioTracks[0].mix.strip.dynamics.gateEnabled = true;
        TimelineData loaded;
        QString error;
        QVERIFY2(timelineFromJson(timelineToJson(tl), loaded, &error), qPrintable(error));
        QCOMPARE(loaded.audioTracks[0].mix.strip.dynamics, tl.audioTracks[0].mix.strip.dynamics);
    }

    // ---- 135.7 de-esser ----
    void deEsserLowersSibilanceAndKeepsTheRest()
    {
        // A voiced tone at 200 Hz (-18 dBFS) all the time, bursts of "s" (7 kHz at -12 dBFS) at
        // 0.5 - 0.6 s and 1.0 - 1.1 s.
        auto burst = [](int i) { return (i >= 24000 && i < 28800) || (i >= 48000 && i < 52800); };
        auto sig = [&](int i) {
            return tone(i, 200, fromDb(-18)) + (burst(i) ? tone(i, 7000, fromDb(-12)) : 0.0);
        };
        Signal x = make(1.5, sig);
        const Signal in = x;
        DeEsser d(kRate);
        DeEsserSettings s;
        s.frequencyHz = 7000;
        s.rangeDb = 12;
        s.amountPercent = 100;
        s.reactionMs = 10;
        d.configure(s);
        for (int at = 0; at < int(x.size() / 2); at += 256)
            d.process(x.data() + size_t(at) * 2, std::min(256, int(x.size() / 2) - at));
        // The 7 kHz energy in the burst (after the attack) drops by at least 6 dB ...
        const double before = componentDb(in, 7000, 0.512, 0.595);
        const double after = componentDb(x, 7000, 0.512, 0.595);
        QVERIFY2(before - after > 6.0, qPrintable(QString("%1 -> %2").arg(before).arg(after)));
        // ... and never more than the Range.
        QVERIFY(before - after < 12.5);
        // The voiced part is not touched.
        QVERIFY(std::abs(componentDb(x, 200, 0.7, 0.95) - componentDb(in, 200, 0.7, 0.95)) < 0.3);
        QVERIFY(std::abs(componentDb(x, 200, 0.52, 0.595) - componentDb(in, 200, 0.52, 0.595)) < 0.6);
        // Amount 0: nothing happens.
        Signal off = in;
        DeEsser d0(kRate);
        s.amountPercent = 0;
        d0.configure(s);
        d0.process(off.data(), int(off.size() / 2));
        QCOMPARE(off, in);
        // Listen: only the band is heard (the 200 Hz tone is gone).
        Signal listen = in;
        DeEsser dl(kRate);
        s.amountPercent = 100;
        s.listen = true;
        dl.configure(s);
        dl.process(listen.data(), int(listen.size() / 2));
        QVERIFY(componentDb(listen, 200, 0.7, 0.95) < componentDb(in, 200, 0.7, 0.95) - 25.0);
        QVERIFY(std::abs(componentDb(listen, 7000, 0.512, 0.595) - componentDb(in, 7000, 0.512, 0.595)) < 1.0);
    }

    // ---- 135.8 dialogue leveler ----
    void dialogueLevelerRidesTowardTheTarget()
    {
        // Speech-like: a 300 Hz + 900 Hz tone with a 4 Hz amplitude wobble; 8 s quiet, 8 s loud.
        auto speech = [](int i, double rmsDbLevel) {
            const double wobble = 0.65 + 0.35 * std::sin(2.0 * std::numbers::pi * 4.0 * i / kRate);
            const double base = (tone(i, 300, 1.0) + 0.5 * tone(i, 900, 1.0)) * wobble;
            return base * fromDb(rmsDbLevel) * 1.7;
        };
        Signal x = make(16.0, [&](int i) { return speech(i, i < 8 * 48000 ? -38.0 : -12.0); });
        const Signal in = x;
        DialogueLeveler lv(kRate);
        LevelerSettings s;
        s.targetDb = -20.0;
        s.maxUpDb = 24.0;
        s.maxDownDb = 24.0;
        s.speed = LevelerSpeed::Fast;
        lv.configure(s);
        for (int at = 0; at < int(x.size() / 2); at += 480)
            lv.process(x.data() + size_t(at) * 2, std::min(480, int(x.size() / 2) - at));
        const double quiet = rmsDb(x, 6.0, 8.0), loud = rmsDb(x, 14.0, 16.0);
        const double quietIn = rmsDb(in, 6.0, 8.0), loudIn = rmsDb(in, 14.0, 16.0);
        QVERIFY2(loudIn - quietIn > 20.0, "the test signal has two levels");
        // Both end up near the target: closer to each other than 4 dB, and each within 3 dB of -20.
        QVERIFY2(std::abs(loud - quiet) < 4.0, qPrintable(QString("%1 %2").arg(quiet).arg(loud)));
        QVERIFY(std::abs(quiet - (-20.0)) < 3.5);
        QVERIFY(std::abs(loud - (-20.0)) < 3.5);
        // The limits hold: with Max Gain Up 6 dB the quiet speech is raised by 6 dB at most.
        Signal y = in;
        DialogueLeveler lv2(kRate);
        s.maxUpDb = 6.0;
        lv2.configure(s);
        for (int at = 0; at < int(y.size() / 2); at += 480)
            lv2.process(y.data() + size_t(at) * 2, std::min(480, int(y.size() / 2) - at));
        QVERIFY(rmsDb(y, 6.0, 8.0) - quietIn < 6.5);
        QVERIFY(rmsDb(y, 6.0, 8.0) - quietIn > 5.0);
    }

    void dialogueLevelerDoesNotLiftNoise()
    {
        std::mt19937 rng(7);
        std::normal_distribution<double> dist(0.0, 1.0);
        // Noise at -70 dBFS (under the floor) only: left as it is.
        Signal noise = make(4.0, [&](int) { return dist(rng) * fromDb(-70); });
        const Signal noiseIn = noise;
        DialogueLeveler lv(kRate);
        LevelerSettings s;
        s.targetDb = -20.0;
        s.maxUpDb = 30.0;
        s.speed = LevelerSpeed::Fast;
        s.noiseFloorDb = -55.0;
        lv.configure(s);
        lv.process(noise.data(), int(noise.size() / 2));
        QVERIFY(std::abs(rmsDb(noise, 2.0, 4.0) - rmsDb(noiseIn, 2.0, 4.0)) < 0.5);
        // Quiet speech (-40 dBFS) is raised; when it stops, the noise that follows is not lifted
        // along (the gain relaxes to 0 dB).
        Signal x = make(10.0, [&](int i) {
            const double wobble = 0.65 + 0.35 * std::sin(2.0 * std::numbers::pi * 4.0 * i / kRate);
            return i < 4 * 48000 ? tone(i, 300, fromDb(-37)) * wobble * 1.4
                                 : dist(rng) * fromDb(-70);
        });
        const Signal in = x;
        DialogueLeveler lv2(kRate);
        lv2.configure(s);
        for (int at = 0; at < int(x.size() / 2); at += 480)
            lv2.process(x.data() + size_t(at) * 2, std::min(480, int(x.size() / 2) - at));
        QVERIFY(rmsDb(x, 3.0, 4.0) - rmsDb(in, 3.0, 4.0) > 10.0); // speech was lifted
        QVERIFY2(rmsDb(x, 9.0, 10.0) - rmsDb(in, 9.0, 10.0) < 2.0,
            qPrintable(QString::number(rmsDb(x, 9.0, 10.0) - rmsDb(in, 9.0, 10.0))));
        // Background Reduction lowers that noise.
        s.backgroundDb = 12.0;
        Signal z = in;
        DialogueLeveler lv3(kRate);
        lv3.configure(s);
        for (int at = 0; at < int(z.size() / 2); at += 480)
            lv3.process(z.data() + size_t(at) * 2, std::min(480, int(z.size() / 2) - at));
        QVERIFY(rmsDb(z, 9.0, 10.0) - rmsDb(in, 9.0, 10.0) < -8.0);
    }

    // ---- 135.9 effects in the registry, on a clip and on a track ----
    void effectsAreInTheRegistryAndRunOnATrack()
    {
        for (const char* id : {afxid::DeEsserFx, afxid::LevelerFx}) {
            const EffectDescriptor* d = EffectRegistry::instance().find(QLatin1String(id));
            QVERIFY(d);
            QCOMPARE(d->category, EffectCategory::AudioEffect);
            for (const ParamDescriptor& p : d->params)
                QVERIFY2(!p.tooltip.isEmpty(), qPrintable(QLatin1String(id) + p.key));
        }
        // A Dialogue Leveler in a track slot raises the quiet music track toward the target.
        TimelineData tl = duckingTimeline();
        const quint64 slot = mixerops::addTrackEffect(tl, tl.audioTracks[0].id, afxid::LevelerFx);
        QVERIFY(slot != 0);
        EffectInstance* fx = mixerops::trackEffect(tl, tl.audioTracks[0].id, slot);
        fx->params.setBase(QLatin1String(afxkeys::LvTargetDb), -6.0);
        fx->params.setStatic(QLatin1String(afxkeys::LvSpeed), QLatin1String(afxchoice::LvFast));
        const Signal out = mixChunks(tl, 4 * 48000, 4096);
        QVERIFY2(componentDb(out, 440, 3.0, 3.8) > componentDb(out, 440, 0.0, 0.1) + 3.0,
            qPrintable(QString("%1 %2").arg(componentDb(out, 440, 3.0, 3.8)).arg(componentDb(out, 440, 0.0, 0.1)))); // rose
        const quint64 de = mixerops::addTrackEffect(tl, tl.audioTracks[1].id, afxid::DeEsserFx);
        QVERIFY(de != 0);
        QVERIFY(!mixChunks(tl, 48000, 777).empty());
    }
};

QTEST_GUILESS_MAIN(TestDynamicsKey)
#include "test_dynamicskey.moc"
