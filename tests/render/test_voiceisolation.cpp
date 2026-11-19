// SPDX-License-Identifier: GPL-3.0-or-later
// Voice Isolation (docs/behavior-spec.md section 141): the Isolator takes noise and a music bed out
// of a speech-like signal and keeps the signal, Amount 0 is the input untouched and without
// delay, the sound is the same at any block size, the effect is aligned in the mixer (clip and
// track), the toggle and Amount of a clip and of a track are saved, and the controls have tips.
#include "core/AudioEffectKeys.h"
#include "core/EffectRegistry.h"
#include "core/MixerJson.h"
#include "core/MixerOps.h"
#include "core/Serialization.h"
#include "core/VoiceIsolation.h"
#include "render/AudioClipEffects.h"
#include "render/NoiseReduction.h"

#include <QTest>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace kk;
using namespace kk::dsp::noise;

namespace {

constexpr int kRate = 48000;
using Signal = std::vector<float>;

/// Bursts of a voiced, syllable-shaped sound: 150 Hz and 12 harmonics, 0.3 s every 0.5 s.
double speechLike(int n)
{
    const double t = double(n) / kRate;
    const double u = std::fmod(t, 0.5);
    const double env = u < 0.3 ? std::min(1.0, std::min(u, 0.3 - u) / 0.02) : 0.0;
    double s = 0.0;
    for (int h = 1; h <= 12; ++h)
        s += std::sin(2.0 * std::numbers::pi * 150.0 * h * t) / h;
    return 0.2 * env * s;
}

/// A held chord, the kind of music bed under a voice.
double musicBed(int n)
{
    const double t = double(n) / kRate;
    double s = 0.0;
    for (const double hz : {262.0, 330.0, 392.0, 523.0})
        s += std::sin(2.0 * std::numbers::pi * hz * t);
    return 0.03 * s;
}

Signal make(int frames, double noiseRms, double bed, bool speech, unsigned seed = 5)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, std::max(noiseRms, 1e-12));
    Signal v(size_t(frames) * 2);
    for (int n = 0; n < frames; ++n) {
        const double s = (speech ? speechLike(n) : 0.0) + bed * musicBed(n) / 0.03 * 0.03;
        v[size_t(n) * 2] = float(s + (noiseRms > 0 ? gauss(rng) : 0.0));
        v[size_t(n) * 2 + 1] = float(s + (noiseRms > 0 ? gauss(rng) : 0.0));
    }
    return v;
}

double rms(const Signal& v, int from, int to)
{
    double sum = 0.0;
    for (int i = from; i < to; ++i)
        sum += double(v[size_t(i) * 2]) * v[size_t(i) * 2];
    return std::sqrt(sum / std::max(1, to - from));
}

double db(double ratio)
{
    return 20.0 * std::log10(std::max(ratio, 1e-12));
}

Signal run(double amount, const Signal& in, int block = 0)
{
    Isolator iso;
    iso.configure(amount);
    Signal out = in;
    const int n = int(in.size() / 2);
    if (block <= 0)
        block = n;
    for (int at = 0; at < n; at += block)
        iso.process(out.data() + size_t(at) * 2, std::min(block, n - at));
    return out;
}

/// The Isolator output aligned with its input (it is delayed by latencyFrames()).
double levelInBursts(const Signal& out, int latency, int from, int to)
{
    // RMS of the 0.3 s bursts (the speech) between from and to, taken from the aligned output.
    double sum = 0.0;
    int count = 0;
    for (int i = from; i < to; ++i) {
        const double u = std::fmod(double(i) / kRate, 0.5);
        if (u > 0.05 && u < 0.25) {
            const double v = out[size_t(i + latency) * 2];
            sum += v * v;
            ++count;
        }
    }
    return std::sqrt(sum / std::max(count, 1));
}

double levelInPauses(const Signal& out, int latency, int from, int to)
{
    double sum = 0.0;
    int count = 0;
    for (int i = from; i < to; ++i) {
        const double u = std::fmod(double(i) / kRate, 0.5);
        if (u > 0.35 && u < 0.48) {
            const double v = out[size_t(i + latency) * 2];
            sum += v * v;
            ++count;
        }
    }
    return std::sqrt(sum / std::max(count, 1));
}

} // namespace

class TestVoiceIsolation : public QObject
{
    Q_OBJECT
private slots:
    void amountZeroIsTheInputWithoutDelay()
    {
        const Signal in = make(2 * kRate, 0.02, 1.0, true);
        Isolator iso;
        iso.configure(0.0);
        QCOMPARE(iso.latencyFrames(), 0);
        QVERIFY(run(0.0, in) == in);
        QVERIFY(run(0.0, in, 333) == in);
        iso.configure(100.0);
        QVERIFY(iso.latencyFrames() > 0 && iso.latencyFrames() < 4800);
    }

    void takesNoiseOut()
    {
        const Signal in = make(4 * kRate, 0.03, 0.0, false);
        Isolator probe;
        probe.configure(100.0);
        const int latency = probe.latencyFrames();
        const Signal out = run(100.0, in);
        const double change = db(rms(out, kRate, 4 * kRate - latency) / rms(in, kRate, 4 * kRate - latency));
        qInfo("noise only: %.1f dB", change);
        QVERIFY2(change < -20.0, qPrintable(QString::number(change)));
    }

    void takesAMusicBedOut()
    {
        const Signal in = make(4 * kRate, 0.0, 1.0, false);
        const Signal out = run(100.0, in);
        const double change = db(rms(out, 2 * kRate, 4 * kRate - 4000) / rms(in, 2 * kRate, 4 * kRate - 4000));
        qInfo("music bed only: %.1f dB", change);
        QVERIFY2(change < -15.0, qPrintable(QString::number(change)));
    }

    void keepsSpeechLikeSoundAndRemovesTheBackground()
    {
        const Signal clean = make(6 * kRate, 0.0, 0.0, true);
        const Signal noisy = make(6 * kRate, 0.02, 1.0, true, 9);
        Isolator probe;
        probe.configure(100.0);
        const int latency = probe.latencyFrames();
        const Signal out = run(100.0, noisy);
        const int from = 2 * kRate, to = 6 * kRate - latency - 100;
        const double kept = db(levelInBursts(out, latency, from, to) / levelInBursts(clean, 0, from, to));
        const double pausesIn = levelInPauses(noisy, 0, from, to);
        const double pausesOut = levelInPauses(out, latency, from, to);
        qInfo("speech-like: kept %.1f dB; pauses %.1f dB", kept, db(pausesOut / pausesIn));
        QVERIFY2(std::fabs(kept) < 4.0, qPrintable(QString::number(kept)));
        QVERIFY2(db(pausesOut / pausesIn) < -15.0, qPrintable(QString::number(db(pausesOut / pausesIn))));
    }

    void amountScalesTheReduction()
    {
        const Signal in = make(4 * kRate, 0.03, 0.0, false);
        double previous = 1e9;
        for (const double amount : {0.0, 30.0, 60.0, 100.0}) {
            Isolator probe;
            probe.configure(amount);
            const Signal out = run(amount, in);
            const double level = rms(out, 2 * kRate, 4 * kRate - 3000);
            qInfo("amount %.0f: %.1f dB", amount, db(level));
            QVERIFY(level <= previous * 1.001);
            previous = level;
        }
    }

    void theSameAtAnyBlockSize()
    {
        const Signal in = make(2 * kRate, 0.02, 1.0, true, 3);
        const Signal whole = run(80.0, in);
        QVERIFY(whole != in);
        for (const int block : {1, 480, 513, 1024, 4801})
            QVERIFY2(run(80.0, in, block) == whole, qPrintable(QString::number(block)));
        // and the same on a second run (deterministic)
        QVERIFY(run(80.0, in) == whole);
    }

    void aSeekRestartsCleanly()
    {
        const Signal in = make(kRate, 0.02, 0.0, true, 4);
        Isolator iso;
        iso.configure(100.0);
        Signal a = in;
        iso.process(a.data(), kRate);
        iso.reset();
        Signal b = in;
        iso.process(b.data(), kRate);
        QVERIFY(a == b);
    }

    void theEffectIsInTheCatalogueWithTipsAndTheAiLabel()
    {
        const EffectDescriptor* d = EffectRegistry::instance().find(QLatin1String(afxid::VoiceIsolation));
        QVERIFY(d);
        QCOMPARE(d->category, EffectCategory::AudioEffect);
        QVERIFY(d->description.contains(QLatin1String("Optional AI")));
        QCOMPARE(d->params.size(), 1);
        QCOMPARE(d->params[0].key, QString::fromLatin1(afxkeys::DeAmount));
        QVERIFY(!d->params[0].tooltip.isEmpty());
        QVERIFY(!d->params[0].animatable);
        QCOMPARE(d->params[0].def, 100.0);
    }

    void theToggleAndAmountOfAClipAreSaved()
    {
        QVector<EffectInstance> fx;
        QVERIFY(!voiceisolation::isOn(fx));
        QCOMPARE(voiceisolation::amount(fx), 100.0);
        QVERIFY(voiceisolation::setOn(fx, true, 41));
        QVERIFY(!voiceisolation::setOn(fx, true, 42)); // already on
        QVERIFY(voiceisolation::setAmount(fx, 35.0, 43));
        QVERIFY(voiceisolation::isOn(fx));
        QCOMPARE(voiceisolation::amount(fx), 35.0);
        QCOMPARE(fx.size(), 1);
        QVERIFY(voiceisolation::setOn(fx, false, 0));
        QVERIFY(!voiceisolation::isOn(fx));
        QCOMPARE(voiceisolation::amount(fx), 35.0); // kept while off
        // Round trip through the project file.
        TimelineData tl;
        TrackData track;
        track.id = tl.allocateId();
        track.type = TrackType::Audio;
        tl.audioTracks.push_back(track);
        ClipData clip;
        clip.id = tl.allocateId();
        clip.kind = ClipKind::Media;
        clip.duration = 10;
        clip.effects = fx;
        voiceisolation::setOn(clip.effects, true, 0);
        tl.audioTracks[0].clips.push_back(clip);
        QVERIFY(mixerops::setTrackVoiceIsolation(tl, track.id, true));
        QVERIFY(mixerops::setTrackVoiceIsolationAmount(tl, track.id, 60.0));
        const QJsonObject json = serialization::timelineToJson(tl);
        TimelineData back;
        QString error;
        QVERIFY2(serialization::timelineFromJson(json, back, &error), qPrintable(error));
        QVERIFY(voiceisolation::isOn(back.audioTracks[0].clips[0].effects));
        QCOMPARE(voiceisolation::amount(back.audioTracks[0].clips[0].effects), 35.0);
        QVERIFY(voiceisolation::isOn(back.audioTracks[0].mix.effects));
        QCOMPARE(voiceisolation::amount(back.audioTracks[0].mix.effects), 60.0);
        // Off for the track: bypassed, Amount kept, undo-able as a plain data change.
        QVERIFY(mixerops::setTrackVoiceIsolation(tl, track.id, false));
        QVERIFY(!voiceisolation::isOn(tl.audioTracks[0].mix.effects));
        QCOMPARE(voiceisolation::amount(tl.audioTracks[0].mix.effects), 60.0);
    }
};

QTEST_GUILESS_MAIN(TestVoiceIsolation)
#include "test_voiceisolation.moc"
