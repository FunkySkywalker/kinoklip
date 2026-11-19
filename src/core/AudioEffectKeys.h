// SPDX-License-Identifier: GPL-3.0-or-later
// AudioEffectKeys — descriptor ids, parameter keys and limits of the clip audio effects
// (EffectCategory::AudioEffect). Shared by the catalogue (core/BuiltinAudioEffects.cpp) and
// the processors (render/AudioClipEffects.cpp).
#pragma once

namespace kk {

/// Descriptor ids (stable: stored in project files).
namespace afxid {
inline constexpr char Gain[] = "audio_gain";
inline constexpr char Eq[] = "audio_eq";
inline constexpr char Compressor[] = "audio_compressor";
inline constexpr char Limiter[] = "audio_limiter";
inline constexpr char HighPass[] = "audio_highpass";
inline constexpr char LowPass[] = "audio_lowpass";
inline constexpr char Delay[] = "audio_delay";
inline constexpr char Reverb[] = "audio_reverb";
inline constexpr char NoiseReduction[] = "audio_noise_reduction";
inline constexpr char DeEsserFx[] = "audio_deesser";
inline constexpr char LevelerFx[] = "audio_dialogue_leveler";
inline constexpr char Chorus[] = "audio_chorus";
inline constexpr char Flanger[] = "audio_flanger";
inline constexpr char Distortion[] = "audio_distortion";
inline constexpr char StereoWidth[] = "audio_stereo_width";
} // namespace afxid

/// Parameter keys (stored in EffectInstance::params). All audio effect parameters are plain
/// values: they are not keyframable.
namespace afxkeys {
inline constexpr char GainDb[] = "gain_db";
// Equalizer: low shelf, one bell, high shelf.
inline constexpr char LowGainDb[] = "low_gain_db";
inline constexpr char LowFreqHz[] = "low_freq_hz";
inline constexpr char MidGainDb[] = "mid_gain_db";
inline constexpr char MidFreqHz[] = "mid_freq_hz";
inline constexpr char MidQ[] = "mid_q";
inline constexpr char HighGainDb[] = "high_gain_db";
inline constexpr char HighFreqHz[] = "high_freq_hz";
// Compressor.
inline constexpr char ThresholdDb[] = "threshold_db";
inline constexpr char Ratio[] = "ratio";
inline constexpr char AttackMs[] = "attack_ms";
inline constexpr char ReleaseMs[] = "release_ms";
inline constexpr char MakeupDb[] = "makeup_db";
// Limiter.
inline constexpr char CeilingDb[] = "ceiling_db";
// High-pass / low-pass.
inline constexpr char FrequencyHz[] = "frequency_hz";
// Delay.
inline constexpr char TimeMs[] = "time_ms";
inline constexpr char Feedback[] = "feedback"; ///< percent
inline constexpr char Mix[] = "mix"; ///< percent wet (delay and reverb)
// Reverb.
inline constexpr char RoomSize[] = "room_size"; ///< percent
inline constexpr char Damping[] = "damping"; ///< percent
// Noise Reduction (spec 132). ThresholdDb, AttackMs, Ratio and Mix are shared with the other effects.
inline constexpr char NrMode[] = "nr_mode"; ///< afxchoice::NrAuto / NrLearn
inline constexpr char NrEngine[] = "nr_engine"; ///< afxchoice::NrRnnoise / NrGate (Auto Speech only)
inline constexpr char NrSensitivity[] = "sensitivity"; ///< percent
inline constexpr char NrReductionDb[] = "reduction_db";
inline constexpr char NrOutputDb[] = "output_db";
inline constexpr char NrProfile[] = "nr_profile"; ///< hidden: the learned noise profile (core text)
// De-Esser (spec 136). FrequencyHz is shared with the filters.
inline constexpr char DeRangeDb[] = "range_db";
inline constexpr char DeAmount[] = "amount"; ///< percent
inline constexpr char DeReactionMs[] = "reaction_ms";
inline constexpr char DeListen[] = "listen"; ///< bool
// Dialogue Leveler (spec 136).
inline constexpr char LvTargetDb[] = "target_db";
inline constexpr char LvMaxUpDb[] = "max_up_db";
inline constexpr char LvMaxDownDb[] = "max_down_db";
inline constexpr char LvSpeed[] = "speed"; ///< afxchoice::LvSlow / LvMedium / LvFast
inline constexpr char LvNoiseFloorDb[] = "noise_floor_db";
inline constexpr char LvBackgroundDb[] = "background_db";
// Reverb (spec 138; RoomSize, Damping and Mix are older), Chorus, Flanger, Distortion, Stereo Width.
// Feedback, Mix, FrequencyHz and the output key NrOutputDb are shared.
inline constexpr char RvPreset[] = "preset"; ///< afxchoice::RvCustom or a room preset id
inline constexpr char RvPreDelayMs[] = "pre_delay_ms";
inline constexpr char RvDecayS[] = "decay_s";
inline constexpr char RvEarly[] = "early_level"; ///< percent
inline constexpr char Width[] = "width"; ///< percent (Reverb and Stereo Width)
inline constexpr char RateHz[] = "rate_hz";
inline constexpr char DepthPercent[] = "depth";
inline constexpr char DelayMs[] = "delay_ms";
inline constexpr char Voices[] = "voices";
inline constexpr char Spread[] = "spread"; ///< percent
inline constexpr char DriveDb[] = "drive_db";
inline constexpr char Tone[] = "tone"; ///< percent
inline constexpr char DistMode[] = "mode"; ///< afxchoice::DistSoft / DistHard / DistCrush / DistTube
inline constexpr char MonoBelowHz[] = "mono_below_hz"; ///< 0 = off
} // namespace afxkeys

/// Choice values of the Noise Reduction effect.
namespace afxchoice {
inline constexpr char NrAuto[] = "auto";
inline constexpr char NrLearn[] = "learn";
inline constexpr char NrRnnoise[] = "rnnoise";
inline constexpr char NrGate[] = "gate";
inline constexpr char LvSlow[] = "slow";
inline constexpr char LvMedium[] = "medium";
inline constexpr char LvFast[] = "fast";
inline constexpr char DistSoft[] = "soft";
inline constexpr char DistHard[] = "hard";
inline constexpr char DistCrush[] = "crush";
inline constexpr char DistTube[] = "tube";
inline constexpr char RvCustom[] = "custom";
/// The Reverb presets, in the order of dsp::reverbPreset().
inline constexpr const char* kReverbPresetIds[] = {
    "small_room", "large_room", "hall", "plate", "chamber", "cathedral"};
} // namespace afxchoice

/// Longest delay time (ms); the processor's buffer is sized for it.
inline constexpr double kAudioDelayMaxMs = 2000.0;
/// Longest Reverb pre-delay (ms).
inline constexpr double kReverbMaxPreDelayMs = 250.0;

} // namespace kk
