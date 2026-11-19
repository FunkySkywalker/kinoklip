// SPDX-License-Identifier: GPL-3.0-or-later
// Chorus, Flanger, Distortion, Stereo Width and the Reverb with Pre-delay, Decay and room presets
// (docs/behavior-spec.md section 138): each effect changes the sound as described, Mix 0 leaves it
// alone, a block size does not change a sample, the presets, saving and the tooltips.
#include "core/AudioEffectKeys.h"
#include "core/EffectRegistry.h"
#include "core/MixerJson.h"
#include "core/MixerOps.h"
#include "core/Serialization.h"
#include "render/AudioClipEffects.h"
#include "render/ModFx.h"
#include "ui/inspector/ParamAccess.h"

#include <QTest>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace kk;
using namespace kk::dsp;

namespace {

constexpr double kRate = 48000.0;
using Signal = std::vector<float>; ///< interleaved stereo

Signal silence(double seconds)
{
    return Signal(size_t(seconds * kRate) * 2, 0.0f);
}

Signal noise(double seconds, unsigned seed = 7, bool stereo = true)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-0.5f, 0.5f);
    Signal s(size_t(seconds * kRate) * 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        s[i] = d(rng);
        s[i + 1] = stereo ? d(rng) : s[i];
    }
    return s;
}

Signal sine(double seconds, double hz, double amp)
{
    Signal s = silence(seconds);
    for (size_t i = 0; i < s.size() / 2; ++i)
        s[2 * i] = s[2 * i + 1] = float(amp * std::sin(2.0 * std::numbers::pi * hz * double(i) / kRate));
    return s;
}

/// A click every @p periodSeconds.
Signal clicks(double seconds, double periodSeconds)
{
    Signal s = silence(seconds);
    const int period = int(periodSeconds * kRate);
    for (size_t i = 0; i * 2 < s.size(); i += size_t(period))
        s[2 * i] = s[2 * i + 1] = 1.0f;
    return s;
}

Signal impulse(double seconds)
{
    Signal s = silence(seconds);
    s[0] = s[1] = 1.0f;
    return s;
}

EffectInstance make(const char* id, std::initializer_list<std::pair<const char*, double>> values = {})
{
    EffectInstance e = EffectRegistry::instance().createEffect(QLatin1String(id), 1);
    for (const auto& [key, v] : values)
        e.params.setBase(QLatin1String(key), v);
    return e;
}

Signal run(const EffectInstance& e, const Signal& in, int block)
{
    ClipEffectChain chain;
    const QVector<EffectInstance> list = {e};
    Signal out = in;
    const int n = int(in.size() / 2);
    for (int at = 0; at < n; at += block)
        chain.process(list, out.data() + size_t(at) * 2, std::min(block, n - at));
    return out;
}

Signal run(const EffectInstance& e, const Signal& in)
{
    return run(e, in, int(in.size() / 2));
}

/// Level (dB) of the component at @p hz (a Goertzel sum, amplitude of a sine) of the left channel.
double componentDb(const Signal& s, double hz, int from = 0, int to = -1)
{
    const int n = to < 0 ? int(s.size() / 2) : to;
    double re = 0.0, im = 0.0;
    for (int i = from; i < n; ++i) {
        const double w = 2.0 * std::numbers::pi * hz * i / kRate;
        re += double(s[size_t(i) * 2]) * std::cos(w);
        im += double(s[size_t(i) * 2]) * std::sin(w);
    }
    const double amp = 2.0 * std::sqrt(re * re + im * im) / (n - from);
    return 20.0 * std::log10(std::max(amp, 1e-12));
}

/// The delay (samples) of each click of @p period samples in @p out (position of its peak).
std::vector<int> clickDelays(const Signal& out, int period, int skipClicks = 1)
{
    std::vector<int> delays;
    const int n = int(out.size() / 2);
    for (int c = skipClicks; (c + 1) * period <= n; ++c) {
        int best = 0;
        for (int i = 0; i < period; ++i) {
            if (std::fabs(out[size_t((c * period + i) * 2)]) > std::fabs(out[size_t((c * period + best) * 2)]))
                best = i;
        }
        delays.push_back(best);
    }
    return delays;
}

int span(const std::vector<int>& v)
{
    return *std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end());
}

/// Reverberation time (s) from the Schroeder backward integral of the left channel: the slope
/// between -5 and -25 dB, extended to 60 dB.
double reverbTime(const Signal& out)
{
    const size_t n = out.size() / 2;
    std::vector<double> edc(n);
    double sum = 0.0;
    for (size_t i = n; i-- > 0;) {
        sum += double(out[2 * i]) * out[2 * i];
        edc[i] = sum;
    }
    int t5 = -1, t25 = -1;
    for (size_t i = 0; i < n; ++i) {
        const double db = 10.0 * std::log10(edc[i] / edc[0]);
        if (t5 < 0 && db <= -5.0)
            t5 = int(i);
        if (t25 < 0 && db <= -25.0) {
            t25 = int(i);
            break;
        }
    }
    if (t5 < 0 || t25 < 0)
        return 0.0;
    return 3.0 * (t25 - t5) / kRate;
}

/// Index of the first frame whose left sample is over @p threshold.
int onset(const Signal& s, float threshold)
{
    for (size_t i = 0; i < s.size() / 2; ++i) {
        if (std::fabs(s[2 * i]) > threshold)
            return int(i);
    }
    return -1;
}

const char* const kAllFx[] = {afxid::Chorus, afxid::Flanger, afxid::Distortion,
    afxid::StereoWidth, afxid::Reverb};

} // namespace

class TestModFx : public QObject
{
    Q_OBJECT

private slots:
    void registeredWithTooltips()
    {
        for (const char* id : kAllFx) {
            const EffectDescriptor* d = EffectRegistry::instance().find(QLatin1String(id));
            QVERIFY2(d, id);
            QCOMPARE(d->category, EffectCategory::AudioEffect);
            for (const ParamDescriptor& p : d->params)
                QVERIFY2(!p.tooltip.isEmpty(), qPrintable(QLatin1String(id) + "." + p.key));
        }
        const EffectDescriptor* rv = EffectRegistry::instance().find(QLatin1String(afxid::Reverb));
        auto has = [&](const char* key) {
            return std::any_of(rv->params.begin(), rv->params.end(),
                [&](const ParamDescriptor& p) { return p.key == QLatin1String(key); });
        };
        QVERIFY(has(afxkeys::RvPreDelayMs) && has(afxkeys::RvDecayS) && has(afxkeys::RoomSize)
            && has(afxkeys::Damping) && has(afxkeys::RvEarly) && has(afxkeys::Width)
            && has(afxkeys::Mix) && has(afxkeys::RvPreset));
        for (const ParamDescriptor& p : rv->params) {
            if (p.key == QLatin1String(afxkeys::RvPreset))
                QCOMPARE(p.choices.size(), 1 + kReverbPresetCount);
        }
    }

    void chorusMovesTheDelay()
    {
        const int period = 4800;
        const Signal in = clicks(5.0, 0.1);
        auto wet = [&](double depth, double voices) {
            return run(make(afxid::Chorus,
                           {{afxkeys::Mix, 100}, {afxkeys::Voices, voices}, {afxkeys::DepthPercent, depth},
                               {afxkeys::DelayMs, 15}, {afxkeys::RateHz, 0.8}}),
                in);
        };
        const int still = span(clickDelays(wet(0, 1), period));
        QVERIFY2(still <= 1, qPrintable(QString::number(still)));
        // Depth 40 %: +-20 % of 15 ms = +-144 samples.
        const int moved = span(clickDelays(wet(40, 1), period));
        QVERIFY2(std::abs(moved - 288) < 40, qPrintable(QString::number(moved)));
        // More voices: more copies of each click (4 voices at different points of their sweep).
        auto copies = [&](double voices) {
            const Signal o = wet(100, voices);
            int count = 0;
            for (int i = 0; i < period; ++i)
                count += std::fabs(o[size_t((10 * period + i) * 2)]) > 0.05f;
            return count;
        };
        QVERIFY(copies(1) <= 2);
        QVERIFY2(copies(4) >= 4, qPrintable(QString::number(copies(4))));
    }

    void chorusSpreadsVoicesAcrossTheStereoField()
    {
        // A sound on the left only: Stereo Spread 0 keeps it there, 100 sends a voice to the right.
        Signal in = noise(1.0, 3, false);
        for (size_t i = 1; i < in.size(); i += 2)
            in[i] = 0.0f;
        auto rightEnergy = [&](double spread) {
            const Signal o = run(make(afxid::Chorus, {{afxkeys::Mix, 100}, {afxkeys::Voices, 2},
                                                   {afxkeys::Spread, spread}}), in);
            double e = 0.0;
            for (size_t i = 1; i < o.size(); i += 2)
                e += double(o[i]) * o[i];
            return e;
        };
        QVERIFY(rightEnergy(0) < 1e-9);
        QVERIFY(rightEnergy(100) > 1.0);
    }

    void flangerEchoesWithSignedFeedback()
    {
        // Depth 0: a fixed delay of 2 ms (96 samples); the echoes are fb^n.
        for (double fb : {50.0, -50.0}) {
            const Signal out = run(make(afxid::Flanger, {{afxkeys::Mix, 100}, {afxkeys::DepthPercent, 0},
                                                      {afxkeys::DelayMs, 2}, {afxkeys::Feedback, fb}}),
                impulse(0.1));
            QVERIFY(std::fabs(out[2 * 96] - 1.0f) < 1e-4f);
            QVERIFY(std::fabs(out[2 * 192] - float(fb / 100.0)) < 1e-4f);
            QVERIFY(std::fabs(out[2 * 288] - float(fb * fb / 1e4)) < 1e-4f);
        }
    }

    void flangerSweepsTheDelay()
    {
        const int period = 4800;
        const Signal out = run(make(afxid::Flanger, {{afxkeys::Mix, 100}, {afxkeys::DepthPercent, 100},
                                                  {afxkeys::DelayMs, 2}, {afxkeys::Feedback, 0},
                                                  {afxkeys::RateHz, 0.5}}),
            clicks(6.0, 0.1));
        // 0.1 .. 1.9 times 96 samples.
        const int s = span(clickDelays(out, period));
        QVERIFY2(std::abs(s - 173) < 20, qPrintable(QString::number(s)));
    }

    void distortionAddsHarmonics()
    {
        const Signal in = sine(1.0, 480, 0.5);
        auto d = [&](const char* mode, double drive = 20) {
            EffectInstance e = make(afxid::Distortion, {{afxkeys::DriveDb, drive}, {afxkeys::Tone, 100},
                                                     {afxkeys::NrOutputDb, 0}, {afxkeys::Mix, 100}});
            e.params.setStatic(QLatin1String(afxkeys::DistMode), QLatin1String(mode));
            return run(e, in);
        };
        QVERIFY(componentDb(in, 1440) < -100);
        const Signal soft = d(afxchoice::DistSoft);
        QVERIFY(componentDb(soft, 1440) > -30); // odd harmonics
        QVERIFY(componentDb(soft, 960) < componentDb(soft, 1440) - 30); // and almost no even ones
        const Signal hard = d(afxchoice::DistHard);
        QVERIFY(componentDb(hard, 1440) > -30);
        QVERIFY(std::fabs(*std::max_element(hard.begin(), hard.end())) <= 1.15f); // the filters overshoot a little
        const Signal tube = d(afxchoice::DistTube, 12);
        QVERIFY(componentDb(tube, 960) > -40); // even harmonics
        // Bit crush at full drive: 3 bits, a handful of levels.
        EffectInstance crushFx = make(afxid::Distortion, {{afxkeys::DriveDb, 40}, {afxkeys::Tone, 100},
                                                       {afxkeys::NrOutputDb, 0}, {afxkeys::Mix, 100}});
        crushFx.params.setStatic(QLatin1String(afxkeys::DistMode), QLatin1String(afxchoice::DistCrush));
        crushFx.params.setStatic(QLatin1String(afxkeys::Oversampling), QLatin1String(afxchoice::OsOff));
        const Signal crush = run(crushFx, in); // oversampling off: the levels stay discrete
        std::vector<float> levels(crush.begin(), crush.end());
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
        QVERIFY2(levels.size() <= 8 && levels.size() >= 3, qPrintable(QString::number(levels.size())));
        QVERIFY(componentDb(crush, 1440) > -40);
        // Tone: the dark setting takes the harmonic down.
        EffectInstance dark = make(afxid::Distortion, {{afxkeys::DriveDb, 20}, {afxkeys::Tone, 0},
                                                    {afxkeys::NrOutputDb, 0}, {afxkeys::Mix, 100}});
        QVERIFY(componentDb(run(dark, in), 2400) < componentDb(soft, 2400) - 6);
        // Output: -6 dB is half.
        const Signal half = run(make(afxid::Distortion, {{afxkeys::DriveDb, 20}, {afxkeys::Tone, 100},
                                                       {afxkeys::NrOutputDb, -6.0206}, {afxkeys::Mix, 100}}),
            in);
        QVERIFY(std::fabs(componentDb(half, 480) - (componentDb(soft, 480) - 6.0206)) < 0.05);
    }

    // ---- Spec 140: Distortion oversampling, Reverb preset values, clip effect tails ----

    void oversamplingLowersTheAliasing()
    {
        // A 9.5 kHz sine hard driven: its 3rd harmonic (28.5 kHz) folds to 19.5 kHz and its 7th to
        // 18.5 kHz when the shaper runs at the plain rate, and is filtered out when it runs at 2x
        // or 4x.
        const Signal in = sine(0.6, 9500, 0.5);
        for (const char* mode : {afxchoice::DistHard, afxchoice::DistSoft, afxchoice::DistTube,
                 afxchoice::DistCrush}) {
            auto alias = [&](const char* os) {
                EffectInstance e = make(afxid::Distortion, {{afxkeys::DriveDb, QLatin1String(mode) == QLatin1String(afxchoice::DistCrush) ? 40 : 26}, {afxkeys::Tone, 100},
                                                         {afxkeys::NrOutputDb, 0}, {afxkeys::Mix, 100}});
                e.params.setStatic(QLatin1String(afxkeys::DistMode), QLatin1String(mode));
                e.params.setStatic(QLatin1String(afxkeys::Oversampling), QLatin1String(os));
                const Signal out = run(e, in);
                const double a = componentDb(out, 19500, 6000) - componentDb(out, 9500, 6000);
                const double b = componentDb(out, 18500, 6000) - componentDb(out, 9500, 6000);
                return std::max(a, b);
            };
            const double off = alias(afxchoice::OsOff);
            const double x2 = alias(afxchoice::Os2);
            const double x4 = alias(afxchoice::Os4);
            qInfo("%s alias vs fundamental: off %.1f dB, 2x %.1f dB, 4x %.1f dB", mode, off, x2, x4);
            const double need = QLatin1String(mode) == QLatin1String(afxchoice::DistCrush) ? 6.0 : 20.0;
            QVERIFY2(x2 < off - need, mode);
            QVERIFY2(x4 < off - need, mode);
        }
    }

    void oversamplingIsAlignedAndBlockIndependent()
    {
        // The wet sound is delayed by latencyFrames(); the dry part of Mix is delayed the same,
        // so a Mix of 50 % of a clean signal keeps its phase; and no block size changes a sample.
        EffectInstance e = make(afxid::Distortion, {{afxkeys::DriveDb, 0}, {afxkeys::Tone, 100},
                                                 {afxkeys::NrOutputDb, 0}, {afxkeys::Mix, 100}});
        e.params.setStatic(QLatin1String(afxkeys::Oversampling), QLatin1String(afxchoice::Os4));
        ClipEffectChain chain;
        const int latency = chain.latencyFrames({e});
        QVERIFY(latency > 0 && latency < 200);
        // Drive 0 dB soft clip of a low quiet sine is nearly linear: out(n) = in(n - latency).
        const Signal in = sine(0.5, 440, 0.05);
        const Signal out = run(e, in);
        for (int i = latency + 2000; i < 20000; ++i)
            QVERIFY(std::fabs(out[size_t(2 * i)] - in[size_t(2 * (i - latency))]) < 2e-3f);
        for (int block : {1, 37, 480, 4096})
            QVERIFY(run(e, in, block) == out);
        // Off: no delay at all; Mix 0: untouched.
        e.params.setStatic(QLatin1String(afxkeys::Oversampling), QLatin1String(afxchoice::OsOff));
        QCOMPARE(ClipEffectChain().latencyFrames({e}), 0);
        e.params.setStatic(QLatin1String(afxkeys::Oversampling), QLatin1String(afxchoice::Os2));
        e.params.setBase(QLatin1String(afxkeys::Mix), 0.0);
        QCOMPARE(ClipEffectChain().latencyFrames({e}), 0);
        QVERIFY(run(e, in) == in);
    }

    void reverbPresetMovesTheSliders()
    {
        ClipData clip;
        clip.id = 1;
        EffectInstance fx = make(afxid::Reverb);
        fx.id = 5;
        clip.effects.append(fx);
        const EffectDescriptor* d = EffectRegistry::instance().find(QLatin1String(afxid::Reverb));
        QVERIFY(d);
        auto ref = [&](const char* key) { return ParamRef {1, ParamScope::Effect, 5, QLatin1String(key)}; };
        auto number = [&](const char* key) {
            return paramaccess::scalar(clip, ref(key), *d->param(QLatin1String(key)), 0);
        };
        // Choosing Cathedral writes its values into the six sliders.
        paramaccess::setStatic(clip, ref(afxkeys::RvPreset), QLatin1String(afxchoice::kReverbPresetIds[5]));
        const afxchoice::ReverbPresetValues& v = afxchoice::kReverbPresetValues[5];
        QCOMPARE(number(afxkeys::RvPreDelayMs), v.preDelayMs);
        QCOMPARE(number(afxkeys::RvDecayS), v.decayS);
        QCOMPARE(number(afxkeys::RoomSize), v.roomSize);
        QCOMPARE(number(afxkeys::Damping), v.damping);
        QCOMPARE(number(afxkeys::RvEarly), v.early);
        QCOMPARE(number(afxkeys::Width), v.width);
        QCOMPARE(number(afxkeys::Mix), 25.0); // Mix stays the user's
        auto presetNow = [&] {
            return paramaccess::staticValue(clip, ref(afxkeys::RvPreset), *d->param(QLatin1String(afxkeys::RvPreset)))
                .toString();
        };
        QCOMPARE(presetNow(), QString::fromLatin1(afxchoice::kReverbPresetIds[5]));
        // Moving a slider makes the preset Custom, and keeps the other values.
        paramaccess::setScalar(clip, ref(afxkeys::RvDecayS), *d->param(QLatin1String(afxkeys::RvDecayS)), 0, 3.0);
        QCOMPARE(presetNow(), QString::fromLatin1(afxchoice::RvCustom));
        QCOMPARE(number(afxkeys::RvDecayS), 3.0);
        QCOMPARE(number(afxkeys::RoomSize), v.roomSize);
        // Mix is not part of a preset: moving it keeps the preset.
        paramaccess::setStatic(clip, ref(afxkeys::RvPreset), QLatin1String(afxchoice::kReverbPresetIds[1]));
        paramaccess::setScalar(clip, ref(afxkeys::Mix), *d->param(QLatin1String(afxkeys::Mix)), 0, 60.0);
        QCOMPARE(presetNow(), QString::fromLatin1(afxchoice::kReverbPresetIds[1]));
        // Choosing Custom changes no slider.
        const double decay = number(afxkeys::RvDecayS);
        paramaccess::setStatic(clip, ref(afxkeys::RvPreset), QLatin1String(afxchoice::RvCustom));
        QCOMPARE(number(afxkeys::RvDecayS), decay);
        // The sound of a preset is the sound of the sliders it set.
        EffectInstance* got = paramaccess::effect(clip, 5);
        QVERIFY(got);
    }

    void stereoWidth()
    {
        const Signal in = noise(1.0);
        // 100 % is the identity.
        QVERIFY(run(make(afxid::StereoWidth), in) == in);
        // 0 % is mono: the mid.
        const Signal mono = run(make(afxid::StereoWidth, {{afxkeys::Width, 0}}), in);
        for (size_t i = 0; i < in.size(); i += 2) {
            QVERIFY(std::fabs(mono[i] - mono[i + 1]) < 1e-6f);
            QVERIFY(std::fabs(mono[i] - 0.5f * (in[i] + in[i + 1])) < 1e-6f);
        }
        // 200 % doubles the difference and keeps the sum.
        const Signal wide = run(make(afxid::StereoWidth, {{afxkeys::Width, 200}}), in);
        for (size_t i = 0; i < in.size(); i += 2) {
            QVERIFY(std::fabs((wide[i] - wide[i + 1]) - 2.0f * (in[i] - in[i + 1])) < 1e-5f);
            QVERIFY(std::fabs((wide[i] + wide[i + 1]) - (in[i] + in[i + 1])) < 1e-5f);
        }
        // Output gain.
        const Signal loud = run(make(afxid::StereoWidth, {{afxkeys::NrOutputDb, 6.0206}}), in);
        QVERIFY(std::fabs(loud[1001] / in[1001] - 2.0f) < 1e-3f);
        // Mono Below 200 Hz: an opposite-phase 50 Hz is cut, one at 2 kHz stays.
        auto antiphase = [&](double hz) {
            Signal s = sine(1.0, hz, 0.5);
            for (size_t i = 1; i < s.size(); i += 2)
                s[i] = -s[i];
            return s;
        };
        auto keep = [&](double hz) {
            const Signal o = run(make(afxid::StereoWidth, {{afxkeys::MonoBelowHz, 200}}), antiphase(hz));
            return componentDb(o, hz, 24000);
        };
        QVERIFY(keep(50) < -6.0 - 18.0);
        QVERIFY(keep(2000) > -6.0 - 0.5);
    }

    void reverbTimeFollowsDecay()
    {
        for (double decay : {0.5, 1.0, 3.0}) {
            const Signal out = run(make(afxid::Reverb, {{afxkeys::Mix, 100}, {afxkeys::RvDecayS, decay},
                                                     {afxkeys::Damping, 0}, {afxkeys::RvEarly, 0},
                                                     {afxkeys::RvPreDelayMs, 0}}),
                impulse(decay * 2.5 + 2.0));
            const double t = reverbTime(out);
            QVERIFY2(std::fabs(t / decay - 1.0) < 0.2, qPrintable(QString("decay %1 measured %2").arg(decay).arg(t)));
        }
        // Room size changes the sound but not the time.
        const Signal big = run(make(afxid::Reverb, {{afxkeys::Mix, 100}, {afxkeys::RvDecayS, 1.0},
                                                 {afxkeys::Damping, 0}, {afxkeys::RvEarly, 0},
                                                 {afxkeys::RoomSize, 100}}),
            impulse(4.0));
        QVERIFY(std::fabs(reverbTime(big) - 1.0) < 0.2);
    }

    void reverbPreDelayShiftsTheOnset()
    {
        auto at = [&](double pre, double early) {
            return onset(run(make(afxid::Reverb, {{afxkeys::Mix, 100}, {afxkeys::RvPreDelayMs, pre},
                                               {afxkeys::RvEarly, early}}),
                             impulse(1.0)),
                1e-6f);
        };
        for (double early : {0.0, 60.0}) {
            const int base = at(0, early);
            QVERIFY(base >= 0);
            QCOMPARE(at(50, early) - base, 2400);
            QCOMPARE(at(250, early) - base, 12000);
        }
        // The early reflections come first; the tail is later.
        QVERIFY(at(0, 60) < at(0, 0));
    }

    void reverbDampingTakesTheHighs()
    {
        auto highShare = [&](double damping) {
            const Signal out = run(make(afxid::Reverb, {{afxkeys::Mix, 100}, {afxkeys::Damping, damping},
                                                     {afxkeys::RvEarly, 0}}),
                                   noise(0.5, 5));
            // Energy of the difference of neighbouring samples against the energy.
            double hi = 0.0, all = 1e-12;
            for (size_t i = 2; i + 1 < out.size() / 2; ++i) {
                const double d = double(out[2 * i]) - out[2 * (i - 1)];
                hi += d * d;
                all += double(out[2 * i]) * out[2 * i];
            }
            return hi / all;
        };
        QVERIFY(highShare(100) < highShare(0) * 0.5);
    }

    void reverbWidth()
    {
        auto sideShare = [&](double width) {
            const Signal out = run(make(afxid::Reverb, {{afxkeys::Mix, 100}, {afxkeys::Width, width}}),
                impulse(1.0));
            double side = 0.0, mid = 1e-12;
            for (size_t i = 0; i < out.size(); i += 2) {
                side += double(out[i] - out[i + 1]) * (out[i] - out[i + 1]);
                mid += double(out[i] + out[i + 1]) * (out[i] + out[i + 1]);
            }
            return side / mid;
        };
        QVERIFY(sideShare(0) < 1e-9);
        QVERIFY(sideShare(200) > sideShare(100) * 2);
    }

    void reverbPresets()
    {
        QStringList ids;
        std::vector<double> times;
        Signal first;
        for (int i = 0; i < kReverbPresetCount; ++i) {
            const char* id = afxchoice::kReverbPresetIds[i];
            EffectInstance e = make(afxid::Reverb, {{afxkeys::Mix, 100}});
            e.params.setStatic(QLatin1String(afxkeys::RvPreset), QLatin1String(id));
            const Signal out = run(e, impulse(10.0));
            QVERIFY2(onset(out, 1e-6f) >= 0, id);
            if (i == 0)
                first = out;
            else
                QVERIFY2(out != first, id);
            times.push_back(reverbTime(out));
            // The preset is the Custom settings of the same values.
            const ReverbSettings s = reverbPreset(i);
            EffectInstance c = make(afxid::Reverb,
                {{afxkeys::Mix, 100}, {afxkeys::RvPreDelayMs, s.preDelayMs}, {afxkeys::RvDecayS, s.decaySeconds},
                    {afxkeys::RoomSize, s.roomSizePercent}, {afxkeys::Damping, s.dampingPercent},
                    {afxkeys::RvEarly, s.earlyPercent}, {afxkeys::Width, s.widthPercent}});
            QVERIFY2(run(c, impulse(10.0)) == out, id);
        }
        // Small Room < Chamber < Large Room < Plate < Hall < Cathedral by the set decay times.
        QVERIFY(times[0] < times[4] && times[4] < times[1] && times[1] < times[2] && times[2] < times[5]);
        QVERIFY2(times[5] > 3.0 && times[5] < 7.5, qPrintable(QString::number(times[5])));
        QVERIFY2(times[0] > 0.2 && times[0] < 0.7, qPrintable(QString::number(times[0])));
        // Mix still works with a preset.
        EffectInstance e = make(afxid::Reverb, {{afxkeys::Mix, 0}});
        e.params.setStatic(QLatin1String(afxkeys::RvPreset), QLatin1String(afxchoice::kReverbPresetIds[2]));
        const Signal in = noise(0.3);
        QVERIFY(run(e, in) == in);
    }

    void mixZeroIsIdentity()
    {
        const Signal in = noise(1.0, 11);
        for (const char* id : {afxid::Chorus, afxid::Flanger, afxid::Distortion, afxid::Reverb}) {
            const Signal out = run(make(id, {{afxkeys::Mix, 0}}), in);
            QVERIFY2(out == in, id);
        }
        QVERIFY(run(make(afxid::StereoWidth), in) == in);
    }

    void blockSizeDoesNotChangeASample()
    {
        const Signal in = noise(2.0, 13);
        for (const char* id : kAllFx) {
            EffectInstance e = make(id);
            if (QLatin1String(id) == QLatin1String(afxid::StereoWidth)) {
                e.params.setBase(QLatin1String(afxkeys::Width), 150.0);
                e.params.setBase(QLatin1String(afxkeys::MonoBelowHz), 120.0);
            }
            const Signal whole = run(e, in);
            QVERIFY2(whole != in, id);
            for (int block : {1, 64, 480, 777, 4096})
                QVERIFY2(run(e, in, block) == whole, qPrintable(QLatin1String(id) + QString(" block %1").arg(block)));
        }
        for (const char* mode : {afxchoice::DistHard, afxchoice::DistCrush, afxchoice::DistTube}) {
            EffectInstance e = make(afxid::Distortion);
            e.params.setStatic(QLatin1String(afxkeys::DistMode), QLatin1String(mode));
            QVERIFY2(run(e, in, 333) == run(e, in), mode);
        }
        EffectInstance fl = make(afxid::Flanger, {{afxkeys::Feedback, -80}});
        QVERIFY(run(fl, in, 100) == run(fl, in));
        EffectInstance ch = make(afxid::Chorus, {{afxkeys::Voices, 4}, {afxkeys::Feedback, 60}});
        QVERIFY(run(ch, in, 101) == run(ch, in));
    }

    void reverbRingsOutAfterTheInput()
    {
        // The input stops after 0.5 s; a reverb in a track slot goes on sounding (the track is
        // processed past the last clip), and falls away.
        Signal in = noise(0.5);
        in.resize(size_t(6 * kRate) * 2, 0.0f);
        const Signal out = run(make(afxid::Reverb, {{afxkeys::Mix, 100}, {afxkeys::RvDecayS, 2.0}}), in, 1024);
        auto rms = [&](double a, double b) {
            double s = 0.0;
            for (int i = int(a * kRate); i < int(b * kRate); ++i)
                s += double(out[size_t(2 * i)]) * out[size_t(2 * i)];
            return std::sqrt(s / ((b - a) * kRate));
        };
        QVERIFY(rms(0.6, 0.8) > 1e-3);
        QVERIFY(rms(0.6, 0.8) > rms(1.6, 1.8) * 2);
        QVERIFY(rms(1.6, 1.8) > rms(3.6, 3.8) * 2);
        QVERIFY(rms(5.8, 6.0) < 1e-3);
    }

    void noDenormalsInTheTails()
    {
        Signal in = impulse(20.0);
        for (const char* id : kAllFx) {
            EffectInstance e = make(id);
            if (QLatin1String(id) == QLatin1String(afxid::Reverb))
                e.params.setBase(QLatin1String(afxkeys::RvDecayS), 0.3);
            if (QLatin1String(id) == QLatin1String(afxid::Chorus))
                e.params.setBase(QLatin1String(afxkeys::Feedback), 80.0);
            if (QLatin1String(id) == QLatin1String(afxid::Flanger))
                e.params.setBase(QLatin1String(afxkeys::Feedback), 90.0);
            for (float v : run(e, in, 512)) {
                QVERIFY2(std::isfinite(v), id);
                QVERIFY2(v == 0.0f || std::fabs(v) >= 1.2e-38f, id);
            }
        }
    }

    void settingsAreSavedAndLoaded()
    {
        TrackMix mix;
        for (const char* id : kAllFx)
            mix.effects.append(make(id));
        mix.effects[4].params.setStatic(QLatin1String(afxkeys::RvPreset), QStringLiteral("cathedral"));
        mix.effects[4].params.setBase(QLatin1String(afxkeys::RvPreDelayMs), 77.0);
        mix.effects[2].params.setStatic(QLatin1String(afxkeys::DistMode), QLatin1String(afxchoice::DistTube));
        mix.effects[2].params.setBase(QLatin1String(afxkeys::DriveDb), 27.5);
        mix.effects[1].params.setBase(QLatin1String(afxkeys::Feedback), -64.0);
        mix.effects[0].params.setBase(QLatin1String(afxkeys::Voices), 4.0);
        for (size_t i = 0; i < 5; ++i)
            mix.effects[int(i)].id = quint64(i + 1);
        const TrackMix back = serialization::trackMixFromJson(serialization::trackMixToJson(mix));
        QVERIFY(back.effects == mix.effects);
        QCOMPARE(back.effects[4].params.staticValue(QLatin1String(afxkeys::RvPreset)).toString(), QStringLiteral("cathedral"));
        QCOMPARE(back.effects[1].params.value(QLatin1String(afxkeys::Feedback), 0, 0.0), -64.0);
        // The same settings sound the same.
        const Signal in = noise(0.5);
        for (int i = 0; i < 5; ++i)
            QVERIFY(run(back.effects[i], in) == run(mix.effects[i], in));
    }

    void effectsCanBeTrackSlots()
    {
        TimelineData tl = makeDefaultTimeline(QStringLiteral("t"), Rational {25, 1}, 1920, 1080, 1, 1);
        for (const char* id : kAllFx)
            QVERIFY2(mixerops::addTrackEffect(tl, tl.audioTracks[0].id, id) != 0, id);
        QCOMPARE(tl.audioTracks[0].mix.effects.size(), 5);
    }
};

QTEST_GUILESS_MAIN(TestModFx)
#include "test_modfx.moc"
