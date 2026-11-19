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

/// The Reverb Preset choice: choosing a room writes its values into the six room sliders.
ParamDescriptor reverbPresetChoice(const QString& label, const QStringList& labels)
{
    namespace k = afxkeys;
    QStringList ids{QLatin1String(afxchoice::RvCustom)};
    for (const char* id : afxchoice::kReverbPresetIds)
        ids.append(QLatin1String(id));
    ParamDescriptor p
        = choice(QLatin1String(k::RvPreset), label, ids, labels, QLatin1String(afxchoice::RvCustom));
    p.customChoice = QLatin1String(afxchoice::RvCustom);
    int i = 0;
    for (const afxchoice::ReverbPresetValues& v : afxchoice::kReverbPresetValues) {
        p.choiceValues.insert(QLatin1String(afxchoice::kReverbPresetIds[i++]),
            {{QLatin1String(k::RvPreDelayMs), v.preDelayMs}, {QLatin1String(k::RvDecayS), v.decayS},
                {QLatin1String(k::RoomSize), v.roomSize}, {QLatin1String(k::Damping), v.damping},
                {QLatin1String(k::RvEarly), v.early}, {QLatin1String(k::Width), v.width}});
    }
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
    add(effect(Reverb, tr("Reverb"),
        tr("A room, plate or hall: pre-delay, a tail you set in seconds and room presets"),
        {
            tip(reverbPresetChoice(tr("Preset"),
                    {tr("Custom"), tr("Small Room"), tr("Large Room"), tr("Hall"), tr("Plate"),
                        tr("Chamber"), tr("Cathedral")}),
                tr("A ready-made room. Choosing one sets Pre-delay, Decay, Room Size, Damping, "
                   "Early Reflections and Width to its values; change any of them and the "
                   "preset becomes Custom. Mix always stays yours.")),
            tip(value(k::RvPreDelayMs, tr("Pre-delay"), 0, kReverbMaxPreDelayMs, 10, 1, kMs),
                tr("How long after the sound the reverb starts. A longer pre-delay keeps the "
                   "voice clear in front of the room.")),
            tip(value(k::RvDecayS, tr("Decay"), 0.1, 20, 1.8, 0.1, QStringLiteral("s")),
                tr("How long the tail takes to die away (to a thousandth of its loudness).")),
            tip(value(k::RoomSize, tr("Room Size"), 0, 100, 50, 1, kPercent),
                tr("The size of the room: larger spaces have wider-spaced, smoother echoes.")),
            tip(value(k::Damping, tr("Damping"), 0, 100, 30, 1, kPercent),
                tr("Cuts the high frequencies of the tail, like soft walls and curtains do. "
                   "0 is bright, 100 is dark.")),
            tip(value(k::RvEarly, tr("Early Reflections"), 0, 100, 40, 1, kPercent),
                tr("The level of the first echoes from the walls that give the sense of a room.")),
            tip(value(k::Width, tr("Width"), 0, 200, 100, 1, kPercent),
                tr("The stereo width of the reverb: 0 is mono, 100 is natural, more is wider.")),
            tip(value(k::Mix, tr("Mix"), 0, 100, 25, 1, kPercent),
                tr("0 is only the original sound, 100 is only the reverb.")),
        }));
    add(effect(Chorus, tr("Chorus"), tr("Thicken the sound with slightly detuned, moving copies"),
        {
            tip(value(k::RateHz, tr("Rate"), 0.05, 10, 0.8, 0.05, kHz),
                tr("How fast the copies sweep.")),
            tip(value(k::DepthPercent, tr("Depth"), 0, 100, 40, 1, kPercent),
                tr("How far the copies sweep: more is a wobblier, more obvious effect.")),
            tip(value(k::DelayMs, tr("Delay"), 1, 50, 15, 0.1, kMs),
                tr("The delay of the copies: short is tight and metallic, long is a loose double.")),
            tip(value(k::Feedback, tr("Feedback"), 0, 90, 0, 1, kPercent),
                tr("How much of the copies is fed back in for a richer, more resonant sound.")),
            tip(value(k::Voices, tr("Voices"), 1, 4, 2, 1),
                tr("The number of copies. More voices sound like a bigger ensemble.")),
            tip(value(k::Spread, tr("Stereo Spread"), 0, 100, 60, 1, kPercent),
                tr("How far the voices are spread between left and right.")),
            tip(value(k::Mix, tr("Mix"), 0, 100, 50, 1, kPercent),
                tr("0 is only the original sound, 100 is only the copies.")),
        }));
    add(effect(Flanger, tr("Flanger"), tr("A sweeping, jet-plane comb filter"),
        {
            tip(value(k::RateHz, tr("Rate"), 0.05, 10, 0.3, 0.05, kHz),
                tr("How fast the sweep goes up and down.")),
            tip(value(k::DepthPercent, tr("Depth"), 0, 100, 70, 1, kPercent),
                tr("How wide the sweep is.")),
            tip(value(k::DelayMs, tr("Delay"), 0.1, 10, 2, 0.1, kMs),
                tr("The middle delay of the sweep: shorter sounds higher and thinner.")),
            tip(value(k::Feedback, tr("Feedback"), -95, 95, 50, 1, kPercent),
                tr("How much of the effect is fed back in. Positive is a bright ring, negative is "
                   "a hollow, darker one.")),
            tip(value(k::Mix, tr("Mix"), 0, 100, 50, 1, kPercent),
                tr("0 is only the original sound; 50 gives the deepest notches.")),
        }));
    add(effect(Distortion, tr("Distortion"), tr("Add grit and overdrive, or crush the sound"),
        {
            tip(value(k::DriveDb, tr("Drive"), 0, 40, 12, 0.5, kDb),
                tr("How hard the sound is pushed into the distortion. For Bit Crush, more drive "
                   "means fewer bits.")),
            tip(value(k::Tone, tr("Tone"), 0, 100, 70, 1, kPercent),
                tr("A low-pass after the distortion: 0 is dark and smooth, 100 is bright and "
                   "harsh.")),
            tip(choice(QLatin1String(k::DistMode), tr("Mode"),
                    {QLatin1String(afxchoice::DistSoft), QLatin1String(afxchoice::DistHard),
                        QLatin1String(afxchoice::DistCrush), QLatin1String(afxchoice::DistTube)},
                    {tr("Soft Clip"), tr("Hard Clip"), tr("Bit Crush"), tr("Tube-like")},
                    QLatin1String(afxchoice::DistSoft)),
                tr("Soft Clip rounds the peaks, Hard Clip cuts them flat, Bit Crush lowers the "
                   "resolution, Tube-like adds warm even harmonics.")),
            tip(choice(QLatin1String(k::Oversampling), tr("Oversampling"),
                    {QLatin1String(afxchoice::OsOff), QLatin1String(afxchoice::Os2),
                        QLatin1String(afxchoice::Os4)},
                    {tr("Off"), tr("2x"), tr("4x")}, QLatin1String(afxchoice::Os2)),
                tr("Runs the distortion at two or four times the sample rate so the harsh "
                   "harmonics it makes do not fold back as inharmonic aliasing. Costs some "
                   "processing and about a millisecond of delay (which is compensated).")),
            tip(value(k::NrOutputDb, tr("Output"), -24, 24, -6, 0.1, kDb),
                tr("Level of the distorted sound. Distortion is loud: lower this as you raise "
                   "the Drive.")),
            tip(value(k::Mix, tr("Mix"), 0, 100, 100, 1, kPercent),
                tr("0 is only the original sound, 100 is only the distorted sound.")),
        }));
    add(effect(StereoWidth, tr("Stereo Width"), tr("Make the stereo picture narrower or wider"),
        {
            tip(value(k::Width, tr("Width"), 0, 200, 100, 1, kPercent),
                tr("0 is mono, 100 leaves the sound as it is, 200 doubles the difference "
                   "between left and right.")),
            tip(value(k::MonoBelowHz, tr("Mono Below"), 0, 500, 0, 1, kHz),
                tr("Makes the sound below this frequency mono, so the bass stays solid when "
                   "the sound is widened. 0 is off.")),
            tip(value(k::NrOutputDb, tr("Output"), -24, 24, 0, 0.1, kDb),
                tr("Level of the result.")),
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
    add(effect(DeEsserFx, tr("De-Esser"), tr("Soften harsh s and sh sounds in speech"),
        {
            tip(value(k::FrequencyHz, tr("Frequency"), 2000, 12000, 6500, 10, kHz),
                tr("The middle of the sound that is lowered: about 5000 for a deep voice, 7000 "
                   "for a high one.")),
            tip(value(k::DeRangeDb, tr("Range"), 0, 24, 9, 0.5, kDb),
                tr("The most the hissing sound is made quieter.")),
            tip(value(k::DeAmount, tr("Amount"), 0, 100, 50, 1, kPercent),
                tr("How strongly the hissing sound is held back. 0 does nothing; higher takes "
                   "more off every s.")),
            tip(value(k::DeReactionMs, tr("Reaction Time"), 1, 100, 10, 1, kMs),
                tr("How fast the reduction lets go after an s. Short is more exact, long is "
                   "smoother.")),
            tip(boolean(QLatin1String(k::DeListen), tr("Listen"), false),
                tr("Hear only the band that is being watched, to set the Frequency by ear. "
                   "Switch it off again before you render.")),
        }));
    add(effect(LevelerFx, tr("Dialogue Leveler"),
        tr("Even out how loud the speech is, quiet words up and loud words down"),
        {
            tip(value(k::LvTargetDb, tr("Target"), -50, -6, -20, 0.5, kDb),
                tr("The level the speech is brought to (average level of about a tenth of a "
                   "second).")),
            tip(value(k::LvMaxUpDb, tr("Max Gain Up"), 0, 30, 12, 0.5, kDb),
                tr("The most quiet speech is raised.")),
            tip(value(k::LvMaxDownDb, tr("Max Gain Down"), 0, 30, 12, 0.5, kDb),
                tr("The most loud speech is lowered.")),
            tip(choice(QLatin1String(k::LvSpeed), tr("Speed"),
                    {QLatin1String(afxchoice::LvSlow), QLatin1String(afxchoice::LvMedium),
                        QLatin1String(afxchoice::LvFast)},
                    {tr("Slow"), tr("Medium"), tr("Fast")}, QLatin1String(afxchoice::LvMedium)),
                tr("How fast the level follows the speech. Slow sounds natural, Fast evens out "
                   "single words.")),
            tip(value(k::LvNoiseFloorDb, tr("Noise Floor"), -80, -20, -55, 0.5, kDb),
                tr("Sound quieter than this is not speech and is never raised: breaths, pauses "
                   "and room noise stay as they are.")),
            tip(value(k::LvBackgroundDb, tr("Background Reduction"), 0, 24, 0, 0.5, kDb),
                tr("Optional: lower everything that is not speech by up to this much. 0 is "
                   "off.")),
        }));
    add(effect(VoiceIsolation, tr("Voice Isolation"),
        tr("Keep only the voice: takes out noise, music and room sound (Optional AI: the RNNoise "
           "model runs on this computer)"),
        {
            tip(value(k::DeAmount, tr("Amount"), 0, 100, 100, 1, kPercent),
                tr("How strongly everything that is not a voice is taken away. 0 leaves the "
                   "sound as it is, 100 removes as much as possible.")),
        }));
}

} // namespace kk
