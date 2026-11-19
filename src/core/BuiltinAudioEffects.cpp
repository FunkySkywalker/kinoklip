// SPDX-License-Identifier: GPL-3.0-or-later
// The clip audio effects catalogue (EffectCategory::AudioEffect). The processors are in
// src/render/AudioClipEffects.cpp, keyed by these ids.
#include "core/AudioEffectKeys.h"
#include "core/EffectBuilders.h"
#include "core/EffectRegistry.h"
#include "core/MixerData.h"

#include <QCoreApplication>

namespace kk {

using namespace builders;

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::BuiltinAudioEffects", text);
}

const QString kDb = QStringLiteral("dB");
const QString kHz = QStringLiteral("Hz");
const QString kMs = QStringLiteral("ms");
const QString kPercent = QStringLiteral("%");

/// A plain (not keyframable) number: the processors read one value per block.
ParamDescriptor value(const char* key, const QString& label, double min, double max, double def,
    double step, const QString& unit = {})
{
    ParamDescriptor p = number(QLatin1String(key), label, min, max, def, step, unit);
    p.animatable = false;
    return p;
}

/// @p p with the tooltip @p text.
ParamDescriptor tip(ParamDescriptor p, const QString& text)
{
    p.tooltip = text;
    return p;
}

EffectDescriptor effect(const char* id, const QString& name, const QString& description,
    QVector<ParamDescriptor> params)
{
    return descriptor(
        QLatin1String(id), name, EffectCategory::AudioEffect, description, std::move(params));
}

} // namespace

void EffectRegistry::registerAudioEffects()
{
    using namespace afxid;
    namespace k = afxkeys;
    add(effect(Gain, tr("Gain"), tr("Raise or lower the level of the clip"),
        {value(k::GainDb, tr("Gain"), -60, 24, 0, 0.1, kDb)}));
    add(effect(Eq, tr("Equalizer"), tr("Low shelf, mid bell and high shelf"),
        {
            value(k::LowGainDb, tr("Low Gain"), -kEqMaxGainDb, kEqMaxGainDb, 0, 0.1, kDb),
            value(k::LowFreqHz, tr("Low Frequency"), kEqMinFrequencyHz, 1000, 100, 1, kHz),
            value(k::MidGainDb, tr("Mid Gain"), -kEqMaxGainDb, kEqMaxGainDb, 0, 0.1, kDb),
            value(k::MidFreqHz, tr("Mid Frequency"), 100, kEqMaxFrequencyHz, 1000, 1, kHz),
            value(k::MidQ, tr("Mid Q"), kEqMinQ, kEqMaxQ, 1, 0.1),
            value(k::HighGainDb, tr("High Gain"), -kEqMaxGainDb, kEqMaxGainDb, 0, 0.1, kDb),
            value(k::HighFreqHz, tr("High Frequency"), 1000, kEqMaxFrequencyHz, 8000, 1, kHz),
        }));
    add(effect(Compressor, tr("Compressor"), tr("Reduce the level of loud parts"),
        {
            value(k::ThresholdDb, tr("Threshold"), kDynMinThresholdDb, 0, -20, 0.1, kDb),
            value(k::Ratio, tr("Ratio"), 1, kDynMaxRatio, 4, 0.1),
            value(k::AttackMs, tr("Attack"), kDynMinTimeMs, kDynMaxAttackMs, 5, 0.1, kMs),
            value(k::ReleaseMs, tr("Release"), kDynMinTimeMs, kDynMaxReleaseMs, 120, 1, kMs),
            value(k::MakeupDb, tr("Make-up Gain"), 0, kDynMaxMakeupDb, 0, 0.1, kDb),
        }));
    add(effect(Limiter, tr("Limiter"), tr("Never let a sample go above the ceiling"),
        {
            value(k::CeilingDb, tr("Ceiling"), kLimiterMinCeilingDb, 0, -1, 0.1, kDb),
            value(k::ReleaseMs, tr("Release"), 1, kDynMaxReleaseMs, 50, 1, kMs),
        }));
    add(effect(HighPass, tr("High-pass Filter"), tr("Remove low frequencies"),
        {value(k::FrequencyHz, tr("Frequency"), kEqMinFrequencyHz, 2000, 80, 1, kHz)}));
    add(effect(LowPass, tr("Low-pass Filter"), tr("Remove high frequencies"),
        {value(k::FrequencyHz, tr("Frequency"), 200, kEqMaxFrequencyHz, 12000, 1, kHz)}));
    add(effect(Delay, tr("Delay"), tr("Echoes of the sound"),
        {
            value(k::TimeMs, tr("Time"), 1, kAudioDelayMaxMs, 250, 1, kMs),
            value(k::Feedback, tr("Feedback"), 0, 95, 35, 1, kPercent),
            value(k::Mix, tr("Mix"), 0, 100, 30, 1, kPercent),
        }));
    add(effect(Reverb, tr("Reverb"), tr("A simple room: the sound dies away slowly"),
        {
            value(k::RoomSize, tr("Room Size"), 0, 100, 50, 1, kPercent),
            value(k::Damping, tr("Damping"), 0, 100, 50, 1, kPercent),
            value(k::Mix, tr("Mix"), 0, 100, 25, 1, kPercent),
        }));
    add(effect(NoiseReduction, tr("Noise Reduction"),
        tr("Take hiss, hum and room noise out of speech (the RNNoise model is optional AI; a "
           "spectral gate always works)"),
        {
            tip(choice(QLatin1String(k::NrMode), tr("Mode"),
                    {QLatin1String(afxchoice::NrAuto), QLatin1String(afxchoice::NrLearn)},
                    {tr("Auto Speech"), tr("Manual Learn")}, QLatin1String(afxchoice::NrAuto)),
                tr("Auto Speech finds the noise by itself. Manual Learn removes the noise you "
                   "taught it with Learn.")),
            tip(choice(QLatin1String(k::NrEngine), tr("Model"),
                    {QLatin1String(afxchoice::NrRnnoise), QLatin1String(afxchoice::NrGate)},
                    {tr("RNNoise (AI, optional)"), tr("Spectral Gate")},
                    QLatin1String(afxchoice::NrRnnoise)),
                tr("The engine of Auto Speech. RNNoise is a small open neural network that runs "
                   "on this computer; the spectral gate needs no model.")),
            tip(value(k::ThresholdDb, tr("Threshold"), -80, 0, -70, 0.5, kDb),
                tr("Sound quieter than this is treated as noise only and gets the reduction "
                   "set by Ratio.")),
            tip(value(k::AttackMs, tr("Attack"), 1, 500, 20, 1, kMs),
                tr("How fast the reduction lets go when sound returns.")),
            tip(value(k::NrSensitivity, tr("Sensitivity"), 0, 100, 50, 1, kPercent),
                tr("Higher removes more noise but can make speech sound thin or watery.")),
            tip(value(k::Ratio, tr("Ratio"), 1, 10, 2, 0.1),
                tr("How much quieter the sound below the Threshold gets: 1 is not at all, "
                   "higher is more (never more than the Reduction).")),
            tip(value(k::NrReductionDb, tr("Reduction"), 0, 60, 20, 0.5, kDb),
                tr("The most the noise is made quieter.")),
            tip(value(k::Mix, tr("Dry / Wet"), 0, 100, 100, 1, kPercent),
                tr("0 is the original sound, 100 is the cleaned sound.")),
            tip(value(k::NrOutputDb, tr("Output Gain"), -24, 24, 0, 0.1, kDb),
                tr("Level of the result.")),
            [] {
                ParamDescriptor p = text(QLatin1String(afxkeys::NrProfile), tr("Noise Profile"), QString());
                p.hidden = true;
                return p;
            }(),
        }));
}

} // namespace kk
