// SPDX-License-Identifier: GPL-3.0-or-later
// Noise Reduction audio effect, engine part (docs/behavior-spec.md section 132): the spectral gate
// and RNNoise take white noise out of a tone / speech-like signal and keep the signal, Learn
// captures a profile that survives its text form, Dry / Wet 0 is the input, and any split of the
// input into blocks gives the samples of one long run.
#include "core/ClipAttributes.h"
#include "render/NoiseReduction.h"

#include <QTest>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace kk::dsp::noise;

namespace {

constexpr int kRate = 48000;

/// Interleaved stereo: white gaussian noise of @p noiseRms plus @p signal(n) on both channels.
template<typename F>
std::vector<float> make(int frames, double noiseRms, F signal, unsigned seed = 1)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, noiseRms);
    std::vector<float> v(size_t(frames) * 2);
    for (int n = 0; n < frames; ++n) {
        const double s = signal(n);
        v[size_t(n) * 2] = float(s + gauss(rng));
        v[size_t(n) * 2 + 1] = float(s + gauss(rng));
    }
    return v;
}

double tone(int n)
{
    return 0.1 * std::sin(2.0 * std::numbers::pi * 1000.0 * n / kRate);
}

/// The loudness of speechLike(): bursts of 0.3 s every 0.5 s with 20 ms ramps, pauses between.
double envelope(int n)
{
    const double t = std::fmod(double(n) / kRate, 0.5);
    const double ramp = 0.02;
    if (t < 0.3)
        return std::min(1.0, std::min(t, 0.3 - t) / ramp);
    return 0.0;
}

/// A voiced, syllable-shaped sound: 150 Hz with 12 harmonics in bursts.
double speechLike(int n)
{
    const double t = double(n) / kRate;
    const double env = envelope(n);
    double s = 0.0;
    for (int h = 1; h <= 12; ++h)
        s += std::sin(2.0 * std::numbers::pi * 150.0 * h * t) / h;
    return 0.2 * env * s;
}

/// Low-passed noise with a 50 Hz hum, @p rms in total (a room-like noise).
std::vector<float> colouredNoise(int frames, double rms, unsigned seed)
{
    auto v = make(frames, 1.0, [](int n) { return 0.5 * std::sin(2.0 * std::numbers::pi * 50.0 * n / kRate); }, seed);
    float state[2] = {0, 0};
    for (size_t i = 0; i < v.size(); ++i) {
        state[i & 1] += 0.1f * (v[i] - state[i & 1]);
        v[i] = state[i & 1];
    }
    double sum = 0;
    for (const float x : v)
        sum += double(x) * x;
    const float gain = float(rms / std::sqrt(sum / double(v.size())));
    for (float& x : v)
        x *= gain;
    return v;
}

double rmsDb(const std::vector<float>& v, int from, int to)
{
    double sum = 0.0;
    for (int i = from; i < to; ++i)
        sum += double(v[size_t(i) * 2]) * v[size_t(i) * 2];
    return 10.0 * std::log10(sum / (to - from) + 1e-20);
}

/// RMS (dB) of @p a - @p b (b delayed by @p delay frames) over [from, to) of @p a.
double errorDb(const std::vector<float>& a, const std::vector<float>& b, int delay, int from, int to)
{
    double sum = 0.0;
    for (int i = from; i < to; ++i) {
        const double d = double(a[size_t(i) * 2]) - double(b[size_t(i - delay) * 2]);
        sum += d * d;
    }
    return 10.0 * std::log10(sum / (to - from) + 1e-20);
}

std::vector<float> run(Reducer& r, std::vector<float> in, int block = 0)
{
    const int frames = int(in.size() / 2);
    if (block <= 0)
        block = frames;
    for (int pos = 0; pos < frames; pos += block)
        r.process(in.data() + size_t(pos) * 2, std::min(block, frames - pos));
    return in;
}

Settings gateSettings()
{
    Settings s;
    s.rnnoise = false;
    s.reductionDb = 30.0;
    return s;
}

} // namespace

class TestNoiseReduction : public QObject
{
    Q_OBJECT

private slots:
    void gateRemovesWhiteNoise()
    {
        const auto noise = make(3 * kRate, 0.02, [](int) { return 0.0; });
        Reducer r;
        r.configure(gateSettings());
        const auto out = run(r, noise);
        const double cut = rmsDb(noise, kRate, 3 * kRate) - rmsDb(out, kRate, 3 * kRate);
        qInfo("gate auto, white noise: cut %.1f dB", cut);
        QVERIFY2(cut > 12.0, qPrintable(QString::number(cut)));
    }

    void gateKeepsTheToneAndRemovesTheNoiseAroundIt()
    {
        const int frames = 5 * kRate;
        const auto noisy = make(frames, 0.02, tone);
        const auto clean = make(frames, 0.0, tone);
        Reducer r;
        r.configure(gateSettings());
        const auto out = run(r, noisy);
        const double before = errorDb(noisy, clean, 0, kRate, frames);
        const double after = errorDb(out, clean, r.latencyFrames(), kRate, frames);
        qInfo("gate auto, tone: error %.1f dB -> %.1f dB", before, after);
        QVERIFY2(before - after > 12.0, qPrintable(QString::number(before - after)));
        QVERIFY(std::abs(rmsDb(out, kRate, frames) - rmsDb(clean, kRate, frames)) < 1.5);
    }

    void learnedGateRemovesNoiseAndKeepsSpeechLikeSound()
    {
        const int frames = 5 * kRate;
        const auto noiseOnly = make(kRate, 0.02, [](int) { return 0.0; }, 7);
        const Profile p = learnProfile(noiseOnly.data(), kRate);
        QVERIFY(p.valid());
        Settings s = gateSettings();
        s.learnMode = true;
        // Noise alone: cut.
        {
            const auto noise = make(3 * kRate, 0.02, [](int) { return 0.0; }, 8);
            Reducer r;
            r.setProfile(p);
            r.configure(s);
            const auto out = run(r, noise);
            const double cut = rmsDb(noise, kRate, 3 * kRate) - rmsDb(out, kRate, 3 * kRate);
            qInfo("gate learn, white noise: cut %.1f dB", cut);
            QVERIFY2(cut > 20.0, qPrintable(QString::number(cut)));
        }
        // Speech-like sound alone: kept (level within 1 dB).
        const auto clean = make(frames, 0.0, speechLike);
        {
            Reducer r;
            r.setProfile(p);
            r.configure(s);
            const auto out = run(r, clean);
            const double change = rmsDb(out, kRate, frames) - rmsDb(clean, kRate, frames);
            qInfo("gate learn, speech-like only: level change %.2f dB", change);
            QVERIFY(std::abs(change) < 1.0);
        }
        // Both: the sound keeps its level and the pauses between the bursts are cut.
        const auto noisy = make(frames, 0.02, speechLike, 3);
        Reducer r;
        r.setProfile(p);
        r.configure(s);
        const auto out = run(r, noisy);
        const int lat = r.latencyFrames();
        double loud = 0, loudClean = 0, quiet = 0, quietIn = 0;
        int nl = 0, nq = 0;
        for (int i = kRate; i < frames - lat; ++i) {
            const double e = envelope(i);
            if (e > 0.95) {
                loud += std::pow(double(out[size_t(i + lat) * 2]), 2);
                loudClean += std::pow(double(clean[size_t(i) * 2]), 2);
                ++nl;
            } else if (e < 0.001) {
                quiet += std::pow(double(out[size_t(i + lat) * 2]), 2);
                quietIn += std::pow(double(noisy[size_t(i) * 2]), 2);
                ++nq;
            }
        }
        const double keep = 10 * std::log10((loud / nl) / (loudClean / nl));
        const double pause = 10 * std::log10((quietIn / nq) / (quiet / nq));
        qInfo("gate learn, bursts + noise: loud part %.2f dB (vs clean), pauses cut %.1f dB", keep, pause);
        QVERIFY(std::abs(keep) < 1.0);
        QVERIFY2(pause > 12.0, qPrintable(QString::number(pause)));
    }

    void learnCapturesTheNoiseLevel()
    {
        const auto a = make(kRate, 0.02, [](int) { return 0.0; }, 1);
        const auto b = make(kRate, 0.2, [](int) { return 0.0; }, 2);
        const Profile pa = learnProfile(a.data(), kRate);
        const Profile pb = learnProfile(b.data(), kRate);
        QVERIFY(pa.valid() && pb.valid());
        // Ten times the amplitude is 20 dB more power in every band (the mean over bins).
        double ma = 0, mb = 0;
        for (int k = 10; k < kBins - 10; ++k) {
            ma += pa.powerDb[size_t(k)];
            mb += pb.powerDb[size_t(k)];
        }
        QVERIFY(std::abs((mb - ma) / (kBins - 20) - 20.0) < 1.0);
        QVERIFY(!learnProfile(a.data(), 100).valid()); // too short
    }

    void profileTextRoundTrip()
    {
        const auto a = make(kRate, 0.05, tone);
        const Profile p = learnProfile(a.data(), kRate);
        const QString text = encodeProfile(p);
        QVERIFY(text.startsWith(QLatin1String("v1:")));
        const Profile back = decodeProfile(text);
        QVERIFY(back.valid());
        for (int k = 0; k < kBins; ++k)
            QVERIFY(std::abs(back.powerDb[size_t(k)] - p.powerDb[size_t(k)]) < 0.4f);
        QVERIFY(encodeProfile(Profile {}).isEmpty());
        QVERIFY(!decodeProfile(QStringLiteral("garbage")).valid());
        QVERIFY(!decodeProfile(QStringLiteral("v1:AAAA")).valid());
    }

    void dryWetZeroIsTheInput()
    {
        const auto in = make(kRate, 0.02, tone);
        for (const bool rnn : {false, true}) {
            Reducer r;
            Settings s = gateSettings();
            s.rnnoise = rnn;
            s.mix = 0.0;
            r.configure(s);
            QVERIFY(run(r, in, 777) == in);
        }
    }

    void blockSizeDoesNotChangeTheResult()
    {
        const auto in = make(2 * kRate, 0.02, speechLike);
        for (const bool rnn : {false, true}) {
            Settings s = gateSettings();
            s.rnnoise = rnn;
            s.thresholdDb = -40.0;
            s.mix = 80.0;
            s.outputDb = 3.0;
            Reducer whole;
            whole.configure(s);
            const auto ref = run(whole, in);
            for (const int block : {64, 480, 1000, 1024, 4801}) {
                Reducer r;
                r.configure(s);
                const auto out = run(r, in, block);
                float worst = 0;
                for (size_t i = 0; i < out.size(); ++i)
                    worst = std::max(worst, std::abs(out[i] - ref[i]));
                QVERIFY2(worst < 1e-5f, qPrintable(QStringLiteral("rnn %1 block %2: %3")
                                                       .arg(rnn).arg(block).arg(worst)));
            }
        }
    }

    void rnnoiseRemovesNoise()
    {
        if (!rnnoiseAvailable())
            QSKIP("RNNoise is not part of this build");
        // RNNoise is trained on room noise, hum and hiss of recordings; white noise at a high
        // level is not what it removes best, so the test noise is low-passed with a hum.
        const auto noise = colouredNoise(3 * kRate, 0.02, 4);
        Reducer r;
        Settings s;
        s.reductionDb = 40.0;
        r.configure(s);
        const auto out = run(r, noise);
        const double cut = rmsDb(noise, kRate, 3 * kRate) - rmsDb(out, kRate, 3 * kRate);
        qInfo("rnnoise, room-like noise: cut %.1f dB", cut);
        QVERIFY2(cut > 20.0, qPrintable(QString::number(cut)));
    }

    void reductionLimitsTheCut()
    {
        for (const bool rnn : {false, true}) {
            const auto noiseOnly = rnn ? colouredNoise(2 * kRate, 0.02, 9)
                                       : make(2 * kRate, 0.02, [](int) { return 0.0; }, 9);
            Reducer r;
            Settings s = gateSettings();
            s.rnnoise = rnn;
            s.reductionDb = 6.0;
            r.configure(s);
            const auto out = run(r, noiseOnly);
            const double cut = rmsDb(noiseOnly, kRate, 2 * kRate) - rmsDb(out, kRate, 2 * kRate);
            qInfo("rnn %d reduction 6 dB: cut %.1f dB", rnn, cut);
            QVERIFY(cut < 8.0);
            QVERIFY(cut > 3.0);
        }
    }
};

QTEST_GUILESS_MAIN(TestNoiseReduction)
#include "test_noisereduction.moc"
