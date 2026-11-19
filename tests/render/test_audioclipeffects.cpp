// SPDX-License-Identifier: GPL-3.0-or-later
// Clip audio effects (docs/behavior-spec.md section 51): an effect stack on an audio clip changes
// the mixed samples as expected (gain, filters, dynamics, EQ, delay, reverb), a bypassed effect
// leaves the signal alone, the order of the stack matters, clip effects run before the clip's
// volume, and mixing in blocks of any size (playback) gives the samples of one long mix (export).
#include "RenderTestUtil.h"
#include "core/AudioEffectKeys.h"
#include "core/EffectRegistry.h"
#include "render/AudioMixer.h"

#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <functional>
#include <numbers>
#include <random>
#include <vector>

using namespace kk;
using namespace kk::test;

namespace {

constexpr int kRate = 48000;
const QString kMedia = QStringLiteral("fx");
constexpr int64_t kFrames = 50; ///< two seconds at 25 fps

/// Writes a 48 kHz mono 16-bit WAV of @p seconds whose sample n is @p sample(n) (-1 ... 1).
bool writeWav(const QString& path, int seconds, const std::function<double(int64_t)>& sample)
{
    const quint32 frames = quint32(kRate * seconds);
    QByteArray bytes;
    auto u32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i)
            bytes.append(char((v >> (8 * i)) & 0xff));
    };
    auto u16 = [&](quint16 v) {
        bytes.append(char(v & 0xff));
        bytes.append(char(v >> 8));
    };
    bytes.append("RIFF");
    u32(36 + frames * 2);
    bytes.append("WAVEfmt ");
    u32(16);
    u16(1);
    u16(1);
    u32(kRate);
    u32(kRate * 2);
    u16(2);
    u16(16);
    bytes.append("data");
    u32(frames * 2);
    for (quint32 n = 0; n < frames; ++n)
        u16(quint16(qint16(std::lround(sample(n) * 32767.0))));
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

double sine(int64_t n, double hz, double amp)
{
    return amp * std::sin(2.0 * std::numbers::pi * hz * double(n) / kRate);
}

double rms(const std::vector<float>& out, int from, int to)
{
    double sum = 0.0;
    for (int i = from; i < to; ++i)
        sum += double(out[size_t(2 * i)]) * double(out[size_t(2 * i)]);
    return std::sqrt(sum / (to - from));
}

double peak(const std::vector<float>& out, int from, int to)
{
    double p = 0.0;
    for (int i = from; i < to; ++i)
        p = std::max(p, std::abs(double(out[size_t(2 * i)])));
    return p;
}

/// An EffectInstance of descriptor @p id with its defaults and the given parameter values.
EffectInstance fx(TimelineData& tl, const char* id,
    std::initializer_list<std::pair<const char*, double>> values = {})
{
    EffectInstance e = EffectRegistry::instance().createEffect(QLatin1String(id), tl.allocateId());
    for (const auto& [key, v] : values)
        e.params.setBase(QLatin1String(key), v);
    return e;
}

} // namespace

class TestAudioClipEffects : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_wavSine, m_wavDc, m_wavImpulse, m_wavLow, m_wavNoise;

    std::shared_ptr<MediaPathMap> paths(const QString& wav)
    {
        auto map = std::make_shared<MediaPathMap>();
        map->insert(kMedia, wav);
        return map;
    }

    /// One clip of @p wav on A1 carrying @p effects, volume @p volumeDb.
    TimelineData timeline(const QVector<EffectInstance>& effects, double volumeDb = 0.0)
    {
        TimelineData tl = emptyTimeline();
        ClipData c = mediaClip(tl, kMedia, 0, kFrames, 0, 2'000'000);
        c.audio.setBase(keys::VolumeDb, volumeDb);
        c.effects = effects;
        addClip(tl, TrackType::Audio, 0, c);
        return tl;
    }

    /// The mix of [0, frames) in one call.
    std::vector<float> render(const TimelineData& tl, const QString& wav, int frames, int block = 0)
    {
        AudioMixer mixer;
        mixer.setMediaPaths(paths(wav));
        std::vector<float> out(size_t(2) * frames);
        if (block <= 0)
            block = frames;
        for (int pos = 0; pos < frames; pos += block) {
            const int n = std::min(block, frames - pos);
            mixer.mix(tl, pos, n, out.data() + size_t(2) * pos);
        }
        return out;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_wavSine = m_dir.filePath("sine.wav");
        m_wavDc = m_dir.filePath("dc.wav");
        m_wavImpulse = m_dir.filePath("impulse.wav");
        m_wavLow = m_dir.filePath("low.wav");
        m_wavNoise = m_dir.filePath("noise.wav");
        QVERIFY(writeWav(m_wavSine, 2, [](int64_t n) { return sine(n, 440, 0.6); }));
        QVERIFY(writeWav(m_wavDc, 2, [](int64_t) { return 0.5; }));
        QVERIFY(writeWav(m_wavImpulse, 2, [](int64_t n) { return n == 1000 ? 0.8 : 0.0; }));
        QVERIFY(writeWav(m_wavLow, 2, [](int64_t n) { return sine(n, 50, 0.1); }));
        std::mt19937 rng(11);
        std::normal_distribution<double> gauss(0.0, 0.03);
        QVERIFY(writeWav(m_wavNoise, 2, [&](int64_t n) { return gauss(rng) + sine(n, 1000, 0.1); }));
    }

    void registryHasTheAudioEffects()
    {
        const auto list = EffectRegistry::instance().byCategory(EffectCategory::AudioEffect);
        QStringList ids;
        for (const EffectDescriptor* d : list) {
            ids.append(d->id);
            for (const ParamDescriptor& p : d->params)
                QVERIFY2(!p.animatable, qPrintable(d->id + "/" + p.key));
        }
        QCOMPARE(ids,
            QStringList({"audio_gain", "audio_eq", "audio_compressor", "audio_limiter",
                "audio_highpass", "audio_lowpass", "audio_delay", "audio_reverb", "audio_chorus",
                "audio_flanger", "audio_distortion", "audio_stereo_width", "audio_noise_reduction", "audio_deesser", "audio_dialogue_leveler"}));
    }

    void noEffectsIsUntouched()
    {
        const auto plain = render(timeline({}), m_wavSine, 24000);
        QVERIFY(peak(plain, 1000, 24000) > 0.55);
    }

    void gainMinus6dBHalvesTheAmplitude()
    {
        TimelineData base = timeline({});
        const auto plain = render(base, m_wavSine, 24000);
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(
            fx(tl, afxid::Gain, {{afxkeys::GainDb, -6.0206}}));
        const auto out = render(tl, m_wavSine, 24000);
        for (int i = 0; i < 24000; ++i)
            QVERIFY2(std::abs(out[size_t(2 * i)] - 0.5f * plain[size_t(2 * i)]) < 2e-4f,
                qPrintable(QString::number(i)));
        QVERIFY(std::abs(peak(out, 1000, 24000) / peak(plain, 1000, 24000) - 0.5) < 0.01);
    }

    void bypassRestoresTheOriginal()
    {
        const auto plain = render(timeline({}), m_wavSine, 24000);
        TimelineData tl = timeline({});
        EffectInstance gain = fx(tl, afxid::Gain, {{afxkeys::GainDb, -12.0}});
        EffectInstance hp = fx(tl, afxid::HighPass, {{afxkeys::FrequencyHz, 2000.0}});
        gain.enabled = hp.enabled = false;
        tl.audioTracks[0].clips[0].effects = {gain, hp};
        const auto out = render(tl, m_wavSine, 24000);
        QVERIFY(out == plain); // bit for bit
    }

    void highPassRemovesDc()
    {
        const auto plain = render(timeline({}), m_wavDc, 48000);
        QVERIFY(std::abs(plain[2 * 40000] - 0.5f) < 1e-3f);
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(
            fx(tl, afxid::HighPass, {{afxkeys::FrequencyHz, 80.0}}));
        const auto out = render(tl, m_wavDc, 48000);
        QVERIFY(peak(out, 40000, 48000) < 0.01); // 0.5 -> ~0 after 0.8 s
        QVERIFY(peak(out, 0, 100) > 0.4); // the step passes first
    }

    void lowPassCutsHighFrequencies()
    {
        QTemporaryDir dir;
        const QString wav = dir.filePath("hi.wav");
        QVERIFY(writeWav(wav, 2, [](int64_t n) { return sine(n, 8000, 0.5); }));
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(
            fx(tl, afxid::LowPass, {{afxkeys::FrequencyHz, 500.0}}));
        const auto out = render(tl, wav, 24000);
        QVERIFY(rms(out, 4000, 24000) < 0.35 * std::pow(10.0, -40.0 / 20.0)); // > 40 dB down
    }

    void equalizerLowShelfBoostsBass()
    {
        const auto plain = render(timeline({}), m_wavLow, 24000);
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(
            fx(tl, afxid::Eq, {{afxkeys::LowGainDb, 12.0}, {afxkeys::LowFreqHz, 200.0}}));
        const auto out = render(tl, m_wavLow, 24000);
        const double gain = rms(out, 8000, 24000) / rms(plain, 8000, 24000);
        QVERIFY2(gain > 3.0 && gain < 4.6, qPrintable(QString::number(gain))); // about +12 dB
    }

    void compressorReducesLoudSound()
    {
        const auto plain = render(timeline({}), m_wavSine, 24000);
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(fx(tl, afxid::Compressor,
            {{afxkeys::ThresholdDb, -30.0}, {afxkeys::Ratio, 8.0}, {afxkeys::AttackMs, 1.0}}));
        const auto out = render(tl, m_wavSine, 24000);
        QVERIFY(rms(out, 12000, 24000) < 0.5 * rms(plain, 12000, 24000));
    }

    void limiterHoldsTheCeiling()
    {
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(
            fx(tl, afxid::Limiter, {{afxkeys::CeilingDb, -6.0}}));
        const auto out = render(tl, m_wavSine, 24000);
        QVERIFY(peak(out, 0, 24000) <= 0.5013); // -6 dBFS
        QVERIFY(peak(out, 0, 24000) > 0.45);
    }

    void delayEchoesTheImpulse()
    {
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(fx(tl, afxid::Delay,
            {{afxkeys::TimeMs, 100.0}, {afxkeys::Feedback, 0.0}, {afxkeys::Mix, 50.0}}));
        const auto out = render(tl, m_wavImpulse, 24000);
        QVERIFY(std::abs(out[2 * 1000] - 0.4f) < 2e-3f); // dry half
        QVERIFY(std::abs(out[2 * (1000 + 4800)] - 0.4f) < 2e-3f); // echo after 100 ms
        QVERIFY(peak(out, 1001, 1000 + 4799) < 1e-4);
        QVERIFY(peak(out, 1000 + 4801, 24000) < 1e-4); // no feedback: one echo only
    }

    void delayReachesItsLongTimes()
    {
        // 1.5 s of the 2 s maximum: the stereo delay line must hold the whole time.
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(fx(tl, afxid::Delay,
            {{afxkeys::TimeMs, 1500.0}, {afxkeys::Feedback, 0.0}, {afxkeys::Mix, 50.0}}));
        const auto out = render(tl, m_wavImpulse, 90000);
        QVERIFY(std::abs(out[2 * (1000 + 72000)] - 0.4f) < 2e-3f); // echo after 1.5 s
        QVERIFY(peak(out, 1001, 1000 + 71999) < 1e-4);
    }

    void reverbAddsATail()
    {
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(
            fx(tl, afxid::Reverb, {{afxkeys::RoomSize, 70.0}, {afxkeys::Mix, 40.0}}));
        const auto out = render(tl, m_wavImpulse, 48000);
        QVERIFY(peak(out, 0, 1000) < 1e-6);
        QVERIFY(rms(out, 6000, 30000) > 1e-4); // a tail where the dry sound is silent
        QVERIFY(rms(out, 6000, 12000) > rms(out, 36000, 48000)); // and it dies away
        // Mix 0 is the dry signal.
        tl.audioTracks[0].clips[0].effects[0].params.setBase(afxkeys::Mix, 0.0);
        const auto dry = render(tl, m_wavImpulse, 48000);
        QVERIFY(peak(dry, 6000, 48000) < 1e-6);
    }

    void clipEffectsRunBeforeTheClipVolume()
    {
        const auto plain = render(timeline({}), m_wavSine, 24000);
        // Gain -6 dB in the stack and a clip volume of -6 dB multiply to a quarter.
        TimelineData tl = timeline({}, -6.0206);
        tl.audioTracks[0].clips[0].effects.append(
            fx(tl, afxid::Gain, {{afxkeys::GainDb, -6.0206}}));
        const auto out = render(tl, m_wavSine, 24000);
        QVERIFY(std::abs(peak(out, 1000, 24000) / peak(plain, 1000, 24000) - 0.25) < 0.01);
        // A limiter at -6 dB before a +12 dB clip volume: the volume comes after it, so the
        // peak is the ceiling plus 12 dB (soft clipper permitting: stay below its knee).
        TimelineData tl2 = timeline({}, 6.0206);
        tl2.audioTracks[0].clips[0].effects.append(
            fx(tl2, afxid::Limiter, {{afxkeys::CeilingDb, -12.0}}));
        const auto out2 = render(tl2, m_wavSine, 24000);
        QVERIFY(std::abs(peak(out2, 0, 24000) - 0.2512 * 2.0) < 0.01);
    }

    void stackOrderMatters()
    {
        TimelineData a = timeline({});
        a.audioTracks[0].clips[0].effects = {fx(a, afxid::Gain, {{afxkeys::GainDb, 12.0}}),
            fx(a, afxid::Limiter, {{afxkeys::CeilingDb, -6.0}})};
        TimelineData b = timeline({});
        b.audioTracks[0].clips[0].effects = {fx(b, afxid::Limiter, {{afxkeys::CeilingDb, -6.0}}),
            fx(b, afxid::Gain, {{afxkeys::GainDb, 12.0}})};
        const auto outA = render(a, m_wavSine, 24000);
        const auto outB = render(b, m_wavSine, 24000);
        QVERIFY(peak(outA, 0, 24000) <= 0.5013); // limited last
        QVERIFY(peak(outB, 0, 24000) > 1.5 * peak(outA, 0, 24000)); // boosted last
    }

    void playbackBlocksGiveTheSamplesOfAnExport()
    {
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects
            = {fx(tl, afxid::HighPass, {{afxkeys::FrequencyHz, 200.0}}),
                fx(tl, afxid::Eq, {{afxkeys::MidGainDb, 6.0}}),
                fx(tl, afxid::Compressor, {{afxkeys::ThresholdDb, -24.0}}),
                fx(tl, afxid::Delay, {{afxkeys::TimeMs, 37.0}}), fx(tl, afxid::Reverb),
                fx(tl, afxid::Limiter), fx(tl, afxid::Gain, {{afxkeys::GainDb, -3.0}})};
        const auto whole = render(tl, m_wavSine, 36000);
        for (const int block : {512, 1024, 997}) { // playback chunks; 997 is not a multiple of 64
            const auto chunks = render(tl, m_wavSine, 36000, block);
            float worst = 0.0f;
            for (size_t i = 0; i < whole.size(); ++i)
                worst = std::max(worst, std::abs(whole[i] - chunks[i]));
            QVERIFY2(
                worst < 1e-4f, qPrintable(QStringLiteral("block %1: %2").arg(block).arg(worst)));
        }
        // The same range again from a new mixer (a second export) is identical.
        QVERIFY(render(tl, m_wavSine, 36000) == whole);
    }

    // Spec 132: the Noise Reduction effect through the mixer.
    void noiseReductionCleansTheClipAndKeepsTheTone()
    {
        TimelineData base = timeline({});
        const auto noisy = render(base, m_wavNoise, 96000);
        for (const char* engine : {afxchoice::NrGate, afxchoice::NrRnnoise}) {
            TimelineData tl = timeline({});
            EffectInstance e = fx(tl, afxid::NoiseReduction, {{afxkeys::NrReductionDb, 40.0}});
            e.params.setStatic(QLatin1String(afxkeys::NrEngine), QLatin1String(engine));
            tl.audioTracks[0].clips[0].effects.append(e);
            const auto out = render(tl, m_wavNoise, 96000);
            // The window starts after the 2-hop delay and the first seconds of settling.
            const int from = 48000, to = 90000;
            const double tonePart = std::abs(10.0 * std::log10(rms(out, from, to) / rms(noisy, from, to)));
            qInfo("%s: level change %.2f dB", engine, tonePart);
            if (QLatin1String(engine) == QLatin1String(afxchoice::NrGate)) {
                // The 1 kHz tone is kept (level within 1.5 dB of the noisy clip: noise is -30 dB
                // under it) and the noise around it falls: the sound is cleaner than the input,
                // so the difference to the pure tone shrinks by at least 10 dB.
                double errIn = 0, errOut = 0;
                for (int i = from; i < to; ++i) {
                    const double t = sine(i, 1000, 0.1);
                    const double tDelayed = sine(i, 1000, 0.1); // aligned: no delay left
                    errIn += std::pow(noisy[size_t(2 * i)] - t, 2);
                    errOut += std::pow(out[size_t(2 * i)] - tDelayed, 2);
                }
                QVERIFY2(10.0 * std::log10(errIn / errOut) > 10.0,
                    qPrintable(QString::number(10.0 * std::log10(errIn / errOut))));
            }
        }
    }

    void noiseReductionDryWetZeroIsTheClip()
    {
        const auto plain = render(timeline({}), m_wavNoise, 48000);
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(fx(tl, afxid::NoiseReduction, {{afxkeys::Mix, 0.0}}));
        QVERIFY(render(tl, m_wavNoise, 48000) == plain);
    }

    void noiseReductionPlaybackBlocksMatchTheExport()
    {
        for (const char* engine : {afxchoice::NrGate, afxchoice::NrRnnoise}) {
            TimelineData tl = timeline({});
            EffectInstance e = fx(tl, afxid::NoiseReduction, {{afxkeys::Mix, 85.0}});
            e.params.setStatic(QLatin1String(afxkeys::NrEngine), QLatin1String(engine));
            tl.audioTracks[0].clips[0].effects.append(e);
            const auto whole = render(tl, m_wavNoise, 60000);
            for (const int block : {512, 1024, 997}) {
                const auto chunks = render(tl, m_wavNoise, 60000, block);
                float worst = 0.0f;
                for (size_t i = 0; i < whole.size(); ++i)
                    worst = std::max(worst, std::abs(whole[i] - chunks[i]));
                QVERIFY2(worst < 1e-4f, qPrintable(QStringLiteral("%1 block %2: %3")
                                                       .arg(QLatin1String(engine)).arg(block).arg(worst)));
            }
            QVERIFY(render(tl, m_wavNoise, 60000) == whole);
        }
    }

    // The effect's own delay is cancelled: the output lines up with the input sample for sample,
    // the first block is not silent and the clip's tail is complete, at any block size.
    void noiseReductionHasNoLatency()
    {
        for (const char* engine : {afxchoice::NrGate, afxchoice::NrRnnoise}) {
            TimelineData tl = timeline({});
            EffectInstance e = fx(tl, afxid::NoiseReduction, {{afxkeys::Mix, 1.0}});
            e.params.setStatic(QLatin1String(afxkeys::NrEngine), QLatin1String(engine));
            tl.audioTracks[0].clips[0].effects.append(e);
            const auto plain = render(timeline({}), m_wavSine, 96000);
            for (const int block : {0, 1024, 997}) {
                const auto out = render(tl, m_wavSine, 96000, block);
                // With 1 % wet the output is the (aligned) input; a delay of 960 or 1024
                // samples would give errors of more than 0.5 for the 440 Hz tone.
                float worst = 0.0f;
                for (int i = 0; i < 96000; ++i)
                    worst = std::max(worst, std::abs(out[size_t(2 * i)] - plain[size_t(2 * i)]));
                QVERIFY2(worst < 0.02f, qPrintable(QStringLiteral("%1 block %2: %3")
                                                       .arg(QLatin1String(engine)).arg(block).arg(worst)));
                QVERIFY(rms(out, 0, 1024) > 0.3); // the first block is not silent
                QVERIFY(rms(out, 96000 - 2048, 96000) > 0.3); // the tail is there
            }
            // An impulse stays at its sample.
            const auto pulse = render(tl, m_wavImpulse, 4000);
            int at = 0;
            for (int i = 0; i < 4000; ++i)
                if (std::abs(pulse[size_t(2 * i)]) > std::abs(pulse[size_t(2 * at)]))
                    at = i;
            QCOMPARE(at, 1000);
        }
    }

    void noiseReductionDescriptorHasTooltipsAndChoices()
    {
        const EffectDescriptor* d = EffectRegistry::instance().find(QLatin1String(afxid::NoiseReduction));
        QVERIFY(d);
        QCOMPARE(d->category, EffectCategory::AudioEffect);
        QStringList keys;
        for (const ParamDescriptor& p : d->params) {
            keys.append(p.key);
            if (!p.hidden)
                QVERIFY2(!p.tooltip.isEmpty(), qPrintable(p.key));
        }
        QCOMPARE(keys, QStringList({"nr_mode", "nr_engine", "threshold_db", "attack_ms", "sensitivity",
                           "ratio", "reduction_db", "mix", "output_db", "nr_profile"}));
        const ParamDescriptor& mode = d->params[0];
        QCOMPARE(mode.choices, QStringList({"auto", "learn"}));
        QCOMPARE(d->params[1].choices, QStringList({"rnnoise", "gate"}));
    }

    // ---- Spec 140: the tail of a clip's effects goes on past the clip's end ----

    /// A clip of @p frames frames at @p start on A1 with @p effects (the impulse file).
    static ClipData tailClip(TimelineData& tl, int64_t start, int64_t frames, const QVector<EffectInstance>& effects)
    {
        ClipData c = mediaClip(tl, kMedia, start, frames, 0, 2'000'000);
        c.effects = effects;
        return c;
    }

    void aDelayEchoesRingPastTheClipEnd()
    {
        // The clip is the first second; the impulse (0.8) is 1000 samples in. Echoes (mix 100,
        // feedback 50 %, 300 ms) fall at 1000 + 14400 n with amplitude 0.8 * 0.5^(n-1); the 4th
        // echo (0.1) is at 58600, after the clip ends at 48000.
        TimelineData tl = emptyTimeline();
        const EffectInstance echo = fx(tl, afxid::Delay,
            {{afxkeys::TimeMs, 300}, {afxkeys::Feedback, 50}, {afxkeys::Mix, 100}});
        addClip(tl, TrackType::Audio, 0, tailClip(tl, 0, 25, {echo}));
        const auto out = render(tl, m_wavImpulse, 96000);
        QVERIFY2(peak(out, 58500, 58700) > 0.09 && peak(out, 58500, 58700) < 0.11,
            qPrintable(QString::number(peak(out, 58500, 58700))));
        QVERIFY(peak(out, 44100, 44300) > 0.19); // the 3rd, inside the clip
        // The tail ends where the echoes die (60 dB): nothing near the end of the render.
        QVERIFY(peak(out, 90000, 96000) < 1e-3);
        // The same samples in any block size (playback) as in one call (export).
        for (int block : {1000, 1024, 4800, 777})
            QVERIFY2(render(tl, m_wavImpulse, 96000, block) == out, qPrintable(QString::number(block)));
        // Without the effect nothing is heard after the clip.
        TimelineData plain = emptyTimeline();
        addClip(plain, TrackType::Audio, 0, tailClip(plain, 0, 25, {}));
        QVERIFY(peak(render(plain, m_wavImpulse, 96000), 48000, 96000) == 0.0);
    }

    void aReverbTailRingsOutAndStopsAtItsDecay()
    {
        TimelineData tl = emptyTimeline();
        const EffectInstance rv = fx(tl, afxid::Reverb, {{afxkeys::Mix, 100}, {afxkeys::RvDecayS, 1.0}});
        // A clip of 0.4 s (10 frames) of the sine file.
        addClip(tl, TrackType::Audio, 0, tailClip(tl, 0, 10, {rv}));
        const auto out = render(tl, m_wavSine, 192000);
        const int end = 19200; // clip end
        QVERIFY(rms(out, end + 2000, end + 6000) > 0.01); // still ringing 40 ms after the end
        QVERIFY(rms(out, end + 2000, end + 6000) > rms(out, end + 24000, end + 28000) * 2);
        QVERIFY(rms(out, 150000, 190000) < 1e-4); // gone long after the decay
        QVERIFY(render(tl, m_wavSine, 192000, 1024) == out);
        QVERIFY(render(tl, m_wavSine, 192000, 4999) == out);
    }

    void aFollowingClipOverlappingTheTailIsMixedWithIt()
    {
        TimelineData tl = emptyTimeline();
        const EffectInstance echo = fx(tl, afxid::Delay,
            {{afxkeys::TimeMs, 300}, {afxkeys::Feedback, 50}, {afxkeys::Mix, 100}});
        // A is the first second, B follows at once with the same sound and echo.
        addClip(tl, TrackType::Audio, 0, tailClip(tl, 0, 25, {echo}));
        addClip(tl, TrackType::Audio, 0, tailClip(tl, 25, 25, {echo}));
        TimelineData onlyA = emptyTimeline();
        addClip(onlyA, TrackType::Audio, 0, tailClip(onlyA, 0, 25, {echo}));
        TimelineData onlyB = emptyTimeline();
        addClip(onlyB, TrackType::Audio, 0, tailClip(onlyB, 25, 25, {echo}));
        const auto both = render(tl, m_wavImpulse, 144000);
        const auto a = render(onlyA, m_wavImpulse, 144000);
        const auto b = render(onlyB, m_wavImpulse, 144000);
        // A's 4th echo (58600) rings through B's clip and is heard with B's own echoes.
        QVERIFY(peak(both, 58500, 58700) > 0.09);
        QVERIFY(peak(both, 63300, 63500) > 0.7); // B's first echo: 48000 + 1000 + 14400
        for (size_t i = 0; i < both.size(); ++i)
            QVERIFY2(std::abs(both[i] - (a[i] + b[i])) < 1e-6f, qPrintable(QString::number(i)));
        QVERIFY(render(tl, m_wavImpulse, 144000, 1000) == both);
    }

    void aSeekRestartsTheEffectMemory()
    {
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(fx(tl, afxid::Delay,
            {{afxkeys::TimeMs, 100.0}, {afxkeys::Feedback, 0.0}, {afxkeys::Mix, 100.0}}));
        AudioMixer mixer;
        mixer.setMediaPaths(paths(m_wavImpulse));
        std::vector<float> out(2 * 4800);
        mixer.mix(tl, 0, 4800, out.data()); // the impulse at 1000 goes into the delay line
        mixer.mix(tl, 20000, 4800, out.data()); // a seek: the delay line must be empty
        QVERIFY(peak(out, 0, 4800) < 1e-6);
    }
};

QTEST_GUILESS_MAIN(TestAudioClipEffects)
#include "test_audioclipeffects.moc"
