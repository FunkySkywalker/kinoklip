// SPDX-License-Identifier: GPL-3.0-or-later
// Mixer track effect slots (docs/behavior-spec.md section 133): the audio effects of a track
// (TrackMix::effects) change the track's output the same way in playback blocks and in a render,
// a bypassed slot leaves the sound alone, the order of the slots matters, the slots run after the
// clip effects and before the fader, a delaying effect (Noise Reduction) stays in place, and the
// slots survive saving (JSON) and the id rules of a timeline.
#include "RenderTestUtil.h"
#include "core/AudioEffectKeys.h"
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/MixerJson.h"
#include "core/MixerOps.h"
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

double peak(const std::vector<float>& out, int from, int to)
{
    double p = 0.0;
    for (int i = from; i < to; ++i)
        p = std::max(p, std::abs(double(out[size_t(2 * i)])));
    return p;
}

double rms(const std::vector<float>& out, int from, int to)
{
    double sum = 0.0;
    for (int i = from; i < to; ++i)
        sum += double(out[size_t(2 * i)]) * double(out[size_t(2 * i)]);
    return std::sqrt(sum / (to - from));
}

double worstDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    double worst = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, std::abs(double(a[i]) - double(b[i])));
    return worst;
}

EffectInstance fx(TimelineData& tl, const char* id,
    std::initializer_list<std::pair<const char*, double>> values = {})
{
    EffectInstance e = EffectRegistry::instance().createEffect(QLatin1String(id), tl.allocateId());
    for (const auto& [key, v] : values)
        e.params.setBase(QLatin1String(key), v);
    return e;
}

} // namespace

class TestTrackEffects : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_wavSine, m_wavImpulse, m_wavNoise;

    std::shared_ptr<MediaPathMap> paths(const QString& wav)
    {
        auto map = std::make_shared<MediaPathMap>();
        map->insert(kMedia, wav);
        return map;
    }

    /// One clip on A1 (with @p clipEffects), the track carrying @p trackFx.
    TimelineData timeline(const QVector<EffectInstance>& trackFx = {},
        const QVector<EffectInstance>& clipEffects = {}, double faderDb = 0.0)
    {
        TimelineData tl = emptyTimeline();
        ClipData c = mediaClip(tl, kMedia, 0, kFrames, 0, 2'000'000);
        c.effects = clipEffects;
        addClip(tl, TrackType::Audio, 0, c);
        tl.audioTracks[0].mix.effects = trackFx;
        tl.audioTracks[0].mix.strip.volumeDb = faderDb;
        return tl;
    }

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
        m_wavImpulse = m_dir.filePath("impulse.wav");
        m_wavNoise = m_dir.filePath("noise.wav");
        QVERIFY(writeWav(m_wavSine, 2, [](int64_t n) { return sine(n, 440, 0.6); }));
        QVERIFY(writeWav(m_wavImpulse, 2, [](int64_t n) { return n == 1000 ? 0.8 : 0.0; }));
        std::mt19937 rng(11);
        std::normal_distribution<double> gauss(0.0, 0.03);
        QVERIFY(writeWav(m_wavNoise, 2, [&](int64_t n) { return gauss(rng) + sine(n, 1000, 0.1); }));
    }

    // 133.3: a track effect changes the track's output.
    void aTrackEffectChangesTheOutput()
    {
        TimelineData tl = timeline();
        const auto plain = render(tl, m_wavSine, 24000);
        tl.audioTracks[0].mix.effects = {fx(tl, afxid::Gain, {{afxkeys::GainDb, -6.0206}})};
        const auto out = render(tl, m_wavSine, 24000);
        for (int i = 0; i < 24000; ++i)
            QVERIFY2(std::abs(out[size_t(2 * i)] - 0.5f * plain[size_t(2 * i)]) < 2e-4f,
                qPrintable(QString::number(i)));
        // The same range again from a new mixer (a render) is identical.
        QVERIFY(render(tl, m_wavSine, 24000) == out);
    }

    // 133.3: playback blocks give the samples of one long render.
    void playbackBlocksMatchTheRender()
    {
        for (const char* type : {afxid::Delay, afxid::Reverb, afxid::HighPass, afxid::Eq,
                 afxid::Compressor, afxid::NoiseReduction}) {
            TimelineData tl = timeline();
            tl.audioTracks[0].mix.effects = {fx(tl, type)};
            const auto whole = render(tl, m_wavNoise, 60000);
            for (const int block : {512, 997, 4801}) {
                const auto chunks = render(tl, m_wavNoise, 60000, block);
                QVERIFY2(worstDifference(whole, chunks) < 1e-4,
                    qPrintable(QStringLiteral("%1 block %2").arg(QLatin1String(type)).arg(block)));
            }
        }
    }

    // 133.4: a bypassed slot is the signal as it was, bit for bit.
    void bypassRestoresTheOriginal()
    {
        const auto plain = render(timeline(), m_wavSine, 24000);
        TimelineData tl = timeline();
        EffectInstance gain = fx(tl, afxid::Gain, {{afxkeys::GainDb, -12.0}});
        EffectInstance hp = fx(tl, afxid::HighPass, {{afxkeys::FrequencyHz, 2000.0}});
        gain.enabled = hp.enabled = false;
        tl.audioTracks[0].mix.effects = {gain, hp};
        QVERIFY(render(tl, m_wavSine, 24000) == plain);
        // Switched on, the first one changes it.
        tl.audioTracks[0].mix.effects[0].enabled = true;
        QVERIFY(render(tl, m_wavSine, 24000) != plain);
    }

    // 133.5: the slots run in list order.
    void theOrderOfTheSlotsMatters()
    {
        TimelineData a = timeline();
        a.audioTracks[0].mix.effects
            = {fx(a, afxid::Gain, {{afxkeys::GainDb, 12.0}}),
                fx(a, afxid::Limiter, {{afxkeys::CeilingDb, -6.0}})};
        TimelineData b = timeline();
        b.audioTracks[0].mix.effects
            = {fx(b, afxid::Limiter, {{afxkeys::CeilingDb, -6.0}}),
                fx(b, afxid::Gain, {{afxkeys::GainDb, 12.0}})};
        const auto outA = render(a, m_wavSine, 24000);
        const auto outB = render(b, m_wavSine, 24000);
        QVERIFY(worstDifference(outA, outB) > 0.2);
        // Gain then Limiter stays under the ceiling (0.5); the other order is loud again.
        QVERIFY(peak(outA, 2000, 24000) < 0.55);
        QVERIFY(peak(outB, 2000, 24000) > 0.7);
    }

    // 133.6: slots come after the clip effects and before the fader.
    void slotsRunAfterClipEffectsAndBeforeTheFader()
    {
        // The clip is boosted 12 dB (peak 2.4), the track's limiter holds it at -6 dB, the fader
        // takes 12 dB off: 0.5 * 0.25 = 0.126. With the limiter before the clip effect or
        // after the fader the peak would be 0.5 or 0.6.
        TimelineData tl = timeline({}, {}, -12.0);
        tl.audioTracks[0].clips[0].effects = {fx(tl, afxid::Gain, {{afxkeys::GainDb, 12.0}})};
        tl.audioTracks[0].mix.effects = {fx(tl, afxid::Limiter, {{afxkeys::CeilingDb, -6.0}})};
        const auto out = render(tl, m_wavSine, 24000);
        const double p = peak(out, 4000, 24000);
        QVERIFY2(p > 0.09 && p < 0.16, qPrintable(QString::number(p)));
    }

    // 133.7: Noise Reduction as a track effect lines up with the track's sound.
    void aDelayingEffectStaysInPlace()
    {
        for (const char* engine : {afxchoice::NrGate, afxchoice::NrRnnoise}) {
            TimelineData tl = timeline();
            EffectInstance e = fx(tl, afxid::NoiseReduction, {{afxkeys::Mix, 1.0}});
            e.params.setStatic(QLatin1String(afxkeys::NrEngine), QLatin1String(engine));
            tl.audioTracks[0].mix.effects = {e};
            const auto plain = render(timeline(), m_wavSine, 96000);
            for (const int block : {0, 1024, 997}) {
                const auto out = render(tl, m_wavSine, 96000, block);
                const double worst = worstDifference(out, plain);
                QVERIFY2(worst < 0.02, qPrintable(QStringLiteral("%1 block %2: %3")
                                                       .arg(QLatin1String(engine)).arg(block).arg(worst)));
                QVERIFY(rms(out, 0, 1024) > 0.3); // the first block is there
                QVERIFY(rms(out, 96000 - 2048, 96000) > 0.3); // and the tail
            }
            const auto pulse = render(tl, m_wavImpulse, 4000);
            int at = 0;
            for (int i = 0; i < 4000; ++i)
                if (std::abs(pulse[size_t(2 * i)]) > std::abs(pulse[size_t(2 * at)]))
                    at = i;
            QCOMPARE(at, 1000);
        }
    }

    // 133.8: the slots are saved and loaded with the track.
    void slotsSurviveSaveAndLoad()
    {
        TimelineData tl = timeline();
        EffectInstance a = fx(tl, afxid::Gain, {{afxkeys::GainDb, -3.5}});
        EffectInstance b = fx(tl, afxid::NoiseReduction, {{afxkeys::Mix, 64.0}});
        b.params.setStatic(QLatin1String(afxkeys::NrEngine), QStringLiteral("gate"));
        b.enabled = false;
        TrackMix mix;
        mix.effects = {a, b};
        const QJsonObject json = serialization::trackMixToJson(mix);
        const TrackMix back = serialization::trackMixFromJson(json);
        QVERIFY(back.effects == mix.effects);
        QVERIFY(!back.effects[1].enabled);
        QCOMPARE(back.effects[0].params.value(QLatin1String(afxkeys::GainDb), 0, 0.0), -3.5);
        // A track without slots writes nothing for them.
        QVERIFY(!serialization::trackMixToJson(TrackMix {}).contains(QStringLiteral("effects")));
    }

    // 133.8: slot ids are unique in the timeline and repaired when they are not.
    void slotIdsAreUniqueAndRepaired()
    {
        TimelineData tl = timeline();
        QVERIFY(mixerops::addTrackEffect(tl, tl.audioTracks[0].id, afxid::Gain) != 0);
        QVERIFY(mixerops::addTrackEffect(tl, tl.audioTracks[0].id, afxid::Eq) != 0);
        QString why;
        QVERIFY2(editops::checkInvariants(tl, &why), qPrintable(why));
        tl.audioTracks[0].mix.effects[1].id = tl.audioTracks[0].mix.effects[0].id;
        QVERIFY(!editops::checkInvariants(tl, &why));
        editops::normalizeIds(tl);
        QVERIFY2(editops::checkInvariants(tl, &why), qPrintable(why));
    }

    // 133.2: the edits of the slot list.
    void slotEditsRefuseWhatDoesNotFit()
    {
        TimelineData tl = timeline();
        const quint64 track = tl.audioTracks[0].id;
        QCOMPARE(mixerops::addTrackEffect(tl, track, QStringLiteral("gaussian_blur")), quint64(0));
        QCOMPARE(mixerops::addTrackEffect(tl, track, QStringLiteral("nothing")), quint64(0));
        QCOMPARE(mixerops::addTrackEffect(tl, 999999, afxid::Gain), quint64(0));
        const quint64 a = mixerops::addTrackEffect(tl, track, afxid::Gain);
        const quint64 b = mixerops::addTrackEffect(tl, track, afxid::Eq);
        const quint64 c = mixerops::addTrackEffect(tl, track, afxid::Delay, 0);
        QCOMPARE(tl.audioTracks[0].mix.effects.size(), 3);
        QCOMPARE(tl.audioTracks[0].mix.effects[0].id, c); // inserted at the front
        QVERIFY(mixerops::moveTrackEffect(tl, track, a, 2));
        QCOMPARE(tl.audioTracks[0].mix.effects[2].id, a);
        QVERIFY(!mixerops::moveTrackEffect(tl, track, a, 2)); // already there
        QVERIFY(mixerops::setTrackEffectEnabled(tl, track, b, false));
        QVERIFY(!mixerops::setTrackEffectEnabled(tl, track, b, false));
        QVERIFY(mixerops::removeTrackEffect(tl, track, b));
        QVERIFY(!mixerops::removeTrackEffect(tl, track, b));
        QCOMPARE(tl.audioTracks[0].mix.effects.size(), 2);
    }
};

QTEST_GUILESS_MAIN(TestTrackEffects)
#include "test_trackeffects.moc"
