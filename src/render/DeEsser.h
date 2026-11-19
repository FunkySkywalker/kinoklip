// SPDX-License-Identifier: GPL-3.0-or-later
// DeEsser — split-band de-esser and DialogueLeveler — speech level rider (spec 136): the
// processors of the "De-Esser" and "Dialogue Leveler" audio effects (clip and track).
#pragma once

#include "render/Biquad.h"

#include <array>

namespace kk::dsp {

/// De-esser settings (the effect's controls).
struct DeEsserSettings
{
    double frequencyHz = 6500.0; ///< centre of the band that sibilance is taken from
    double rangeDb = 9.0; ///< the most the band is lowered
    double amountPercent = 50.0; ///< how strongly it follows: 50 = to the threshold, 100 = twice
    double reactionMs = 10.0; ///< release time of the reduction (attack is a fifth of it)
    bool listen = false; ///< output the detected band instead of the sound
};

/// Split-band de-esser on interleaved stereo. A band-pass at the Frequency (Q 1.5) is the
/// detector: its level against the level of the whole sound (so a quiet and a loud voice
/// are treated alike) over a fixed threshold of -12 dB gives the reduction, scaled by Amount
/// and limited by Range, smoothed with the Reaction time in dB. The reduction is applied to the
/// band only (output = input + (gain - 1) * band), so the rest of the sound is not touched.
/// Listen outputs the band. No latency. Real-time safe. One instance per thread.
class DeEsser
{
public:
    explicit DeEsser(double sampleRate = 48000.0);
    void configure(const DeEsserSettings& s);
    void reset();
    void process(float* io, int frames);
    /// Current gain reduction of the band (dB, >= 0).
    [[nodiscard]] double gainReductionDb() const { return m_gr; }

private:
    double m_rate;
    DeEsserSettings m_s;
    BiquadCoeffs m_band;
    std::array<BiquadState, 2> m_state;
    double m_bandEnv = 0.0, m_wideEnv = 0.0; ///< linear envelopes
    double m_gr = 0.0;
    double m_attack = 0.0, m_release = 0.0; ///< one-pole coefficients of the reduction
    double m_envAttack = 0.0, m_bandRelease = 0.0, m_wideRelease = 0.0;
};

/// Speed of the Dialogue Leveler.
enum class LevelerSpeed { Slow, Medium, Fast };

/// Dialogue Leveler settings.
struct LevelerSettings
{
    double targetDb = -20.0; ///< wanted speech level (RMS, dBFS)
    double maxUpDb = 12.0; ///< the most speech is raised
    double maxDownDb = 12.0; ///< the most speech is lowered
    LevelerSpeed speed = LevelerSpeed::Medium;
    double noiseFloorDb = -55.0; ///< sound under this is not speech: not raised
    double backgroundDb = 0.0; ///< extra reduction of what is not speech (0 = off)
};

/// Rides the level of speech toward the Target with a slow gain (a wide, smooth fader ride,
/// not a compressor). The level is a 100 ms RMS of the input. While it is over the Noise Floor
/// (opens at the floor, closes 3 dB under it, holds 600 ms) the wanted gain is Target minus
/// level, limited to Max Gain Up / Down, and followed with the Speed time constant (a falling gain
/// three times faster, so loud words are caught). With no speech the gain is neither
/// raised nor held: it relaxes to 0 dB with a slower constant, so a pause never lifts the
/// noise; Background Reduction lowers that sound by up to its value. No latency.
class DialogueLeveler
{
public:
    explicit DialogueLeveler(double sampleRate = 48000.0);
    void configure(const LevelerSettings& s);
    void reset();
    void process(float* io, int frames);
    /// Current total gain (dB).
    [[nodiscard]] double gainDb() const { return m_gain + m_bg; }

private:
    double m_rate;
    LevelerSettings m_s;
    double m_power = 0.0; ///< smoothed mean square
    double m_gain = 0.0; ///< dB, the leveling ride
    double m_bg = 0.0; ///< dB (<= 0), the background reduction
    bool m_speech = false;
    long long m_hold = 0;
    double m_levelCoef = 0.0;
};

} // namespace kk::dsp
