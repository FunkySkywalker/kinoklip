// SPDX-License-Identifier: GPL-3.0-or-later
// AudioClipEffects — gain, equalizer, compressor, limiter, high / low-pass, delay, reverb, chorus,
// flanger, distortion, stereo width, noise reduction, de-esser and dialogue leveler
// processors of a clip's audio effect stack. The EQ and dynamics reuse the mixer's
// dsp::Equalizer and dsp::Dynamics; filters use the RBJ biquads of render/Biquad.h.
#include "render/AudioClipEffects.h"

#include "core/AudioEffectKeys.h"
#include "core/EffectRegistry.h"
#include "render/AudioMixer.h"
#include "render/Biquad.h"
#include "render/DeEsser.h"
#include "render/Dynamics.h"
#include "render/Equalizer.h"
#include "render/ModFx.h"
#include "render/NoiseReduction.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace kk::dsp {

namespace {

constexpr double kRate = AudioMixer::kSampleRate;
constexpr double kButterworthQ = 0.7071; // maximally flat 12 dB / octave filters

/// Parameter @p key (a plain value), @p def when the set lacks it, clamped to [lo, hi]
/// (damaged or hand-edited projects must not break the processors).
double param(const ParamSet& p, const char* key, double def, double lo, double hi)
{
    return std::clamp(p.value(QLatin1String(key), 0, def), lo, hi);
}

// ---- Gain ----

class GainEffect final : public ClipEffect
{
public:
    void configure(const ParamSet& p) override
    {
        const float target = AudioMixer::dbToGain(param(p, afxkeys::GainDb, 0, -60, 24));
        if (m_fresh)
            m_gain = target;
        m_target = target;
        m_fresh = false;
    }
    void reset() override { m_fresh = true; }
    void process(float* io, int frames) override
    {
        // A changed gain ramps linearly over the block, so editing it never clicks.
        const float step = (m_target - m_gain) / float(std::max(frames, 1));
        float g = m_gain;
        for (int i = 0; i < frames; ++i, g += step) {
            io[size_t(i) * 2] *= g;
            io[size_t(i) * 2 + 1] *= g;
        }
        m_gain = m_target;
    }

private:
    float m_gain = 1.0f, m_target = 1.0f;
    bool m_fresh = true;
};

// ---- Equalizer: low shelf, bell, high shelf on the mixer's 6-band EQ ----

class EqEffect final : public ClipEffect
{
public:
    EqEffect(double rate = kRate)
        : m_eq(rate)
    { }
    void configure(const ParamSet& p) override
    {
        namespace k = afxkeys;
        EqSettings s;
        s.enabled = true;
        for (EqBand& b : s.bands)
            b.enabled = false;
        auto setBand = [&](int index, EqBandType type, double hz, double gainDb, double q) {
            s.bands[size_t(index)] = {type, true, hz, gainDb, q};
        };
        setBand(1, EqBandType::LowShelf, param(p, k::LowFreqHz, 100, kEqMinFrequencyHz, 1000),
            param(p, k::LowGainDb, 0, -kEqMaxGainDb, kEqMaxGainDb), kButterworthQ);
        setBand(2, EqBandType::Bell, param(p, k::MidFreqHz, 1000, 100, kEqMaxFrequencyHz),
            param(p, k::MidGainDb, 0, -kEqMaxGainDb, kEqMaxGainDb),
            param(p, k::MidQ, 1, kEqMinQ, kEqMaxQ));
        setBand(4, EqBandType::HighShelf, param(p, k::HighFreqHz, 8000, 1000, kEqMaxFrequencyHz),
            param(p, k::HighGainDb, 0, -kEqMaxGainDb, kEqMaxGainDb), kButterworthQ);
        m_eq.setTarget(s);
    }
    void reset() override { m_eq.reset(); }
    void process(float* io, int frames) override { m_eq.process(io, frames); }

private:
    Equalizer m_eq;
};

// ---- Compressor and limiter on the mixer's dynamics ----

class DynamicsEffect final : public ClipEffect
{
public:
    explicit DynamicsEffect(bool limiter)
        : m_limiter(limiter)
        , m_dyn(kRate)
    { }
    void configure(const ParamSet& p) override
    {
        namespace k = afxkeys;
        DynamicsSettings s;
        if (m_limiter) {
            s.limiterEnabled = true;
            s.limiterCeilingDb = param(p, k::CeilingDb, -1, kLimiterMinCeilingDb, 0);
            s.limiterReleaseMs = param(p, k::ReleaseMs, 50, 1, kDynMaxReleaseMs);
        } else {
            s.compressorEnabled = true;
            s.thresholdDb = param(p, k::ThresholdDb, -20, kDynMinThresholdDb, 0);
            s.ratio = param(p, k::Ratio, 4, 1, kDynMaxRatio);
            s.attackMs = param(p, k::AttackMs, 5, kDynMinTimeMs, kDynMaxAttackMs);
            s.releaseMs = param(p, k::ReleaseMs, 120, kDynMinTimeMs, kDynMaxReleaseMs);
            s.makeupDb = param(p, k::MakeupDb, 0, 0, kDynMaxMakeupDb);
        }
        m_dyn.setTarget(s);
    }
    void reset() override { m_dyn.reset(); }
    void process(float* io, int frames) override { m_dyn.process(io, frames); }

private:
    bool m_limiter;
    Dynamics m_dyn;
};

// ---- High-pass / low-pass: one 12 dB / octave biquad per channel ----

class FilterEffect final : public ClipEffect
{
public:
    explicit FilterEffect(bool highPass)
        : m_highPass(highPass)
    { }
    void configure(const ParamSet& p) override
    {
        const double hz = m_highPass
            ? param(p, afxkeys::FrequencyHz, 80, kEqMinFrequencyHz, 2000)
            : param(p, afxkeys::FrequencyHz, 12000, 200, kEqMaxFrequencyHz);
        if (hz != m_hz) {
            m_hz = hz;
            m_coeffs = designBand(m_highPass ? EqBandType::HighPass : EqBandType::LowPass, hz, 0.0,
                kButterworthQ, kRate);
        }
    }
    void reset() override
    {
        for (BiquadState& s : m_state)
            s.reset();
    }
    void process(float* io, int frames) override
    {
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < 2; ++c) {
                float& x = io[size_t(i) * 2 + size_t(c)];
                x = float(m_state[size_t(c)].process(m_coeffs, double(x)));
            }
        }
    }

private:
    bool m_highPass;
    double m_hz = 0.0;
    BiquadCoeffs m_coeffs;
    std::array<BiquadState, 2> m_state;
};

// ---- Delay: feedback echo, wet / dry crossfade ----

class DelayEffect final : public ClipEffect
{
public:
    DelayEffect()
        : m_line((size_t(std::ceil(kAudioDelayMaxMs * 1e-3 * kRate)) + 1) * 2, 0.0f) // stereo
        , m_size(int(m_line.size() / 2))
    { }
    void configure(const ParamSet& p) override
    {
        namespace k = afxkeys;
        m_delay = std::clamp(
            int(std::lround(param(p, k::TimeMs, 250, 1, kAudioDelayMaxMs) * 1e-3 * kRate)), 1,
            m_size - 1);
        m_feedback = float(param(p, k::Feedback, 35, 0, 95) / 100.0);
        m_mix = float(param(p, k::Mix, 30, 0, 100) / 100.0);
    }
    void reset() override
    {
        std::fill(m_line.begin(), m_line.end(), 0.0f);
        m_write = 0;
    }
    void process(float* io, int frames) override
    {
        for (int i = 0; i < frames; ++i) {
            int read = m_write - m_delay;
            if (read < 0)
                read += m_size;
            for (int c = 0; c < 2; ++c) {
                const float dry = io[size_t(i) * 2 + size_t(c)];
                const float echo = m_line[size_t(read) * 2 + size_t(c)];
                m_line[size_t(m_write) * 2 + size_t(c)] = dry + echo * m_feedback;
                io[size_t(i) * 2 + size_t(c)] = dry * (1.0f - m_mix) + echo * m_mix;
            }
            if (++m_write == m_size)
                m_write = 0;
        }
    }

private:
    std::vector<float> m_line; ///< interleaved stereo ring, m_size frames
    int m_size;
    int m_write = 0;
    int m_delay = 1; ///< frames
    float m_feedback = 0.0f, m_mix = 0.0f;
};

/// Index of the Reverb preset @p id in dsp::reverbPreset(), -1 for Custom or an unknown id.
int reverbPresetIndex(const QString& id)
{
    for (int i = 0; i < kReverbPresetCount; ++i) {
        if (id == QLatin1String(afxchoice::kReverbPresetIds[i]))
            return i;
    }
    return -1;
}

// ---- Reverb, Chorus, Flanger, Distortion, Stereo Width (render/ModFx.h, spec 138) ----

class ReverbEffect final : public ClipEffect
{
public:
    ReverbEffect()
        : m_reverb(kRate)
    { }
    void configure(const ParamSet& p) override
    {
        namespace k = afxkeys;
        const QString preset
            = p.staticValue(QLatin1String(k::RvPreset), QLatin1String(afxchoice::RvCustom)).toString();
        const int index = reverbPresetIndex(preset);
        ReverbSettings s;
        if (index >= 0) {
            s = reverbPreset(index);
        } else {
            s.preDelayMs = param(p, k::RvPreDelayMs, 10, 0, kReverbMaxPreDelayMs);
            s.decaySeconds = param(p, k::RvDecayS, 1.8, 0.1, 20);
            s.roomSizePercent = param(p, k::RoomSize, 50, 0, 100);
            s.dampingPercent = param(p, k::Damping, 30, 0, 100);
            s.earlyPercent = param(p, k::RvEarly, 40, 0, 100);
            s.widthPercent = param(p, k::Width, 100, 0, 200);
        }
        s.mixPercent = param(p, k::Mix, 25, 0, 100);
        m_reverb.configure(s);
    }
    void reset() override { m_reverb.reset(); }
    void process(float* io, int frames) override { m_reverb.process(io, frames); }

private:
    Reverb m_reverb;
};

class ChorusEffect final : public ClipEffect
{
public:
    ChorusEffect()
        : m_chorus(kRate)
    { }
    void configure(const ParamSet& p) override
    {
        namespace k = afxkeys;
        ChorusSettings s;
        s.rateHz = param(p, k::RateHz, 0.8, 0.05, 10);
        s.depthPercent = param(p, k::DepthPercent, 40, 0, 100);
        s.delayMs = param(p, k::DelayMs, 15, 1, 50);
        s.feedbackPercent = param(p, k::Feedback, 0, 0, 90);
        s.voices = int(std::lround(param(p, k::Voices, 2, 1, 4)));
        s.spreadPercent = param(p, k::Spread, 60, 0, 100);
        s.mixPercent = param(p, k::Mix, 50, 0, 100);
        m_chorus.configure(s);
    }
    void reset() override { m_chorus.reset(); }
    void process(float* io, int frames) override { m_chorus.process(io, frames); }

private:
    Chorus m_chorus;
};

class FlangerEffect final : public ClipEffect
{
public:
    FlangerEffect()
        : m_flanger(kRate)
    { }
    void configure(const ParamSet& p) override
    {
        namespace k = afxkeys;
        FlangerSettings s;
        s.rateHz = param(p, k::RateHz, 0.3, 0.05, 10);
        s.depthPercent = param(p, k::DepthPercent, 70, 0, 100);
        s.delayMs = param(p, k::DelayMs, 2, 0.1, 10);
        s.feedbackPercent = param(p, k::Feedback, 50, -95, 95);
        s.mixPercent = param(p, k::Mix, 50, 0, 100);
        m_flanger.configure(s);
    }
    void reset() override { m_flanger.reset(); }
    void process(float* io, int frames) override { m_flanger.process(io, frames); }

private:
    Flanger m_flanger;
};

class DistortionEffect final : public ClipEffect
{
public:
    DistortionEffect()
        : m_dist(kRate)
    { }
    void configure(const ParamSet& p) override
    {
        namespace k = afxkeys;
        DistortionSettings s;
        s.driveDb = param(p, k::DriveDb, 12, 0, 40);
        s.tonePercent = param(p, k::Tone, 70, 0, 100);
        const QString mode
            = p.staticValue(QLatin1String(k::DistMode), QLatin1String(afxchoice::DistSoft)).toString();
        s.mode = mode == QLatin1String(afxchoice::DistHard) ? DistortionMode::HardClip
            : mode == QLatin1String(afxchoice::DistCrush)   ? DistortionMode::BitCrush
            : mode == QLatin1String(afxchoice::DistTube)    ? DistortionMode::Tube
                                                            : DistortionMode::SoftClip;
        s.outputDb = param(p, k::NrOutputDb, -6, -24, 24);
        s.mixPercent = param(p, k::Mix, 100, 0, 100);
        const QString os
            = p.staticValue(QLatin1String(k::Oversampling), QLatin1String(afxchoice::Os2)).toString();
        s.oversampling = os == QLatin1String(afxchoice::OsOff) ? 1
            : os == QLatin1String(afxchoice::Os4)               ? 4
                                                                : 2;
        m_dist.configure(s);
    }
    void reset() override { m_dist.reset(); }
    void process(float* io, int frames) override { m_dist.process(io, frames); }
    int latencyFrames() const override { return m_dist.latencyFrames(); }

private:
    Distortion m_dist;
};

class StereoWidthEffect final : public ClipEffect
{
public:
    StereoWidthEffect()
        : m_width(kRate)
    { }
    void configure(const ParamSet& p) override
    {
        namespace k = afxkeys;
        StereoWidthSettings s;
        s.widthPercent = param(p, k::Width, 100, 0, 200);
        s.monoBelowHz = param(p, k::MonoBelowHz, 0, 0, 500);
        s.outputDb = param(p, k::NrOutputDb, 0, -24, 24);
        m_width.configure(s);
    }
    void reset() override { m_width.reset(); }
    void process(float* io, int frames) override { m_width.process(io, frames); }

private:
    StereoWidth m_width;
};

// ---- Noise Reduction: RNNoise or the spectral gate (render/NoiseReduction.h) ----

class NoiseReductionEffect final : public ClipEffect
{
public:
    void configure(const ParamSet& p) override
    {
        namespace k = afxkeys;
        const QString profileText = p.staticValue(QLatin1String(k::NrProfile)).toString();
        if (profileText != m_profileText) {
            m_profileText = profileText;
            m_reducer.setProfile(noise::decodeProfile(profileText));
        }
        noise::Settings s;
        s.learnMode = p.staticValue(QLatin1String(k::NrMode), QLatin1String(afxchoice::NrAuto))
                          .toString()
            == QLatin1String(afxchoice::NrLearn);
        s.rnnoise = p.staticValue(QLatin1String(k::NrEngine), QLatin1String(afxchoice::NrRnnoise))
                        .toString()
            != QLatin1String(afxchoice::NrGate);
        s.thresholdDb = param(p, k::ThresholdDb, -70, -80, 0);
        s.attackMs = param(p, k::AttackMs, 20, 1, 500);
        s.sensitivity = param(p, k::NrSensitivity, 50, 0, 100);
        s.ratio = param(p, k::Ratio, 2, 1, 10);
        s.reductionDb = param(p, k::NrReductionDb, 20, 0, 60);
        s.mix = param(p, k::Mix, 100, 0, 100);
        s.outputDb = param(p, k::NrOutputDb, 0, -24, 24);
        m_reducer.configure(s);
    }
    void reset() override { m_reducer.reset(); }
    void process(float* io, int frames) override { m_reducer.process(io, frames); }
    int latencyFrames() const override { return m_reducer.latencyFrames(); }

private:
    noise::Reducer m_reducer;
    QString m_profileText;
};

// ---- De-Esser and Dialogue Leveler (render/DeEsser.h) ----

class DeEsserEffect final : public ClipEffect
{
public:
    DeEsserEffect()
        : m_deesser(kRate)
    { }
    void configure(const ParamSet& p) override
    {
        namespace k = afxkeys;
        DeEsserSettings s;
        s.frequencyHz = param(p, k::FrequencyHz, 6500, 2000, 12000);
        s.rangeDb = param(p, k::DeRangeDb, 9, 0, 24);
        s.amountPercent = param(p, k::DeAmount, 50, 0, 100);
        s.reactionMs = param(p, k::DeReactionMs, 10, 1, 100);
        s.listen = p.staticValue(QLatin1String(k::DeListen), false).toBool();
        m_deesser.configure(s);
    }
    void reset() override { m_deesser.reset(); }
    void process(float* io, int frames) override { m_deesser.process(io, frames); }

private:
    DeEsser m_deesser;
};

class DialogueLevelerEffect final : public ClipEffect
{
public:
    DialogueLevelerEffect()
        : m_leveler(kRate)
    { }
    void configure(const ParamSet& p) override
    {
        namespace k = afxkeys;
        LevelerSettings s;
        s.targetDb = param(p, k::LvTargetDb, -20, -50, -6);
        s.maxUpDb = param(p, k::LvMaxUpDb, 12, 0, 30);
        s.maxDownDb = param(p, k::LvMaxDownDb, 12, 0, 30);
        const QString speed
            = p.staticValue(QLatin1String(k::LvSpeed), QLatin1String(afxchoice::LvMedium)).toString();
        s.speed = speed == QLatin1String(afxchoice::LvSlow) ? LevelerSpeed::Slow
            : speed == QLatin1String(afxchoice::LvFast)     ? LevelerSpeed::Fast
                                                            : LevelerSpeed::Medium;
        s.noiseFloorDb = param(p, k::LvNoiseFloorDb, -55, -80, -20);
        s.backgroundDb = param(p, k::LvBackgroundDb, 0, 0, 24);
        m_leveler.configure(s);
    }
    void reset() override { m_leveler.reset(); }
    void process(float* io, int frames) override { m_leveler.process(io, frames); }

private:
    DialogueLeveler m_leveler;
};

std::unique_ptr<ClipEffect> create(const QString& type)
{
    using namespace afxid;
    if (type == QLatin1String(Gain))
        return std::make_unique<GainEffect>();
    if (type == QLatin1String(Eq))
        return std::make_unique<EqEffect>();
    if (type == QLatin1String(Compressor))
        return std::make_unique<DynamicsEffect>(false);
    if (type == QLatin1String(Limiter))
        return std::make_unique<DynamicsEffect>(true);
    if (type == QLatin1String(HighPass))
        return std::make_unique<FilterEffect>(true);
    if (type == QLatin1String(LowPass))
        return std::make_unique<FilterEffect>(false);
    if (type == QLatin1String(Delay))
        return std::make_unique<DelayEffect>();
    if (type == QLatin1String(afxid::Reverb))
        return std::make_unique<ReverbEffect>();
    if (type == QLatin1String(NoiseReduction))
        return std::make_unique<NoiseReductionEffect>();
    if (type == QLatin1String(DeEsserFx))
        return std::make_unique<DeEsserEffect>();
    if (type == QLatin1String(LevelerFx))
        return std::make_unique<DialogueLevelerEffect>();
    if (type == QLatin1String(afxid::Chorus))
        return std::make_unique<ChorusEffect>();
    if (type == QLatin1String(afxid::Flanger))
        return std::make_unique<FlangerEffect>();
    if (type == QLatin1String(afxid::Distortion))
        return std::make_unique<DistortionEffect>();
    if (type == QLatin1String(afxid::StereoWidth))
        return std::make_unique<StereoWidthEffect>();
    return nullptr;
}

} // namespace

ClipEffectChain::ClipEffectChain() = default;
ClipEffectChain::~ClipEffectChain() = default;

bool ClipEffectChain::hasActive(const QVector<EffectInstance>& effects)
{
    return std::any_of(effects.begin(), effects.end(), [](const EffectInstance& fx) {
        return fx.enabled && fx.type.startsWith(QLatin1String("audio_"));
    });
}

void ClipEffectChain::reset()
{
    for (Slot& s : m_slots) {
        if (s.effect)
            s.effect->reset();
    }
}

void ClipEffectChain::prepare(const QVector<EffectInstance>& effects)
namespace {

constexpr int kMaxTailFrames = 30 * int(kRate); ///< no effect keeps a clip sounding longer

/// Frames until feedback loop of @p periodFrames with loop gain @p gain has fallen by 60 dB.
double feedbackTail(double periodFrames, double gain)
{
    const double g = std::clamp(std::fabs(gain), 0.0, 0.999);
    if (g < 1e-3)
        return periodFrames;
    return periodFrames * std::ceil(std::log(1e-3) / std::log(g)) + periodFrames;
}

/// The tail (frames) of one effect from its parameters; mirrors the processors above.
int effectTail(const EffectInstance& fx)
{
    namespace k = afxkeys;
    const ParamSet& p = fx.params;
    double frames = 0.0;
    if (fx.type == QLatin1String(afxid::Reverb)) {
        if (param(p, k::Mix, 25, 0, 100) <= 0.0)
            return 0;
        const QString preset
            = p.staticValue(QLatin1String(k::RvPreset), QLatin1String(afxchoice::RvCustom)).toString();
        const int index = reverbPresetIndex(preset);
        const ReverbSettings s = index >= 0 ? reverbPreset(index) : ReverbSettings{
            param(p, k::RvPreDelayMs, 10, 0, kReverbMaxPreDelayMs), param(p, k::RvDecayS, 1.8, 0.1, 20)};
        // the tail falls 60 dB in Decay seconds; high frequencies die sooner, so this is a bit
        // more than needed
        frames = (s.preDelayMs * 1e-3 + s.decaySeconds * 1.05 + 0.1) * kRate;
    } else if (fx.type == QLatin1String(afxid::Delay)) {
        if (param(p, k::Mix, 30, 0, 100) <= 0.0)
            return 0;
        frames = feedbackTail(param(p, k::TimeMs, 250, 1, kAudioDelayMaxMs) * 1e-3 * kRate,
            param(p, k::Feedback, 35, 0, 95) / 100.0);
    } else if (fx.type == QLatin1String(afxid::Chorus)) {
        if (param(p, k::Mix, 50, 0, 100) <= 0.0)
            return 0;
        const double delay = param(p, k::DelayMs, 15, 1, 50) * 1e-3 * kRate;
        frames = param(p, k::Feedback, 0, 0, 90) > 0
            ? feedbackTail(delay * 1.5, param(p, k::Feedback, 0, 0, 90) / 100.0)
            : delay * 2.0;
    } else if (fx.type == QLatin1String(afxid::Flanger)) {
        if (param(p, k::Mix, 50, 0, 100) <= 0.0)
            return 0;
        frames = feedbackTail(param(p, k::DelayMs, 2, 0.1, 10) * 2.0e-3 * kRate,
            param(p, k::Feedback, 50, -95, 95) / 100.0);
    }
    return int(std::min<double>(std::ceil(frames), kMaxTailFrames));
}

} // namespace

int ClipEffectChain::tailFrames(const QVector<EffectInstance>& effects)
{
    double total = 0.0;
    for (const EffectInstance& fx : effects) {
        if (fx.enabled && fx.type.startsWith(QLatin1String("audio_")))
            total += effectTail(fx);
    }
    return int(std::min<double>(total, kMaxTailFrames));
}

{
    // Drop the processors of removed effects (or whose type changed).
    std::erase_if(m_slots, [&](const Slot& s) {
        return std::none_of(effects.begin(), effects.end(),
            [&](const EffectInstance& fx) { return fx.id == s.id && fx.type == s.type; });
    });
}

ClipEffectChain::Slot* ClipEffectChain::slotFor(const EffectInstance& fx)
{
    auto it = std::find_if(m_slots.begin(), m_slots.end(),
        [&](const Slot& s) { return s.id == fx.id && s.type == fx.type; });
    if (it != m_slots.end())
        return &*it;
    if (!fx.enabled)
        return nullptr;
    std::unique_ptr<ClipEffect> effect = create(fx.type);
    if (!effect)
        return nullptr; // unknown type: skipped, kept in the project
    m_slots.push_back({fx.id, fx.type, std::move(effect), false});
    return &m_slots.back();
}

int ClipEffectChain::latencyFrames(const QVector<EffectInstance>& effects)
{
    prepare(effects);
    int total = 0;
    for (const EffectInstance& fx : effects) {
        if (!fx.enabled)
            continue;
        if (Slot* slot = slotFor(fx)) {
            slot->effect->configure(fx.params);
            total += slot->effect->latencyFrames();
        }
    }
    return total;
}

bool ClipEffectChain::process(const QVector<EffectInstance>& effects, float* io, int frames)
{
    prepare(effects);
    bool any = false;
    for (const EffectInstance& fx : effects) {
        Slot* slot = slotFor(fx);
        if (!fx.enabled) {
            if (slot && slot->wasOn) { // bypassed: start clean when switched on
                slot->effect->reset();
                slot->wasOn = false;
            }
            continue;
        }
        if (!slot)
            continue;
        slot->effect->configure(fx.params);
        slot->effect->process(io, frames);
        slot->wasOn = true;
        any = true;
    }
    return any;
}

} // namespace kk::dsp
