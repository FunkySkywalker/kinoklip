// SPDX-License-Identifier: GPL-3.0-or-later
// Dynamics — the real-time compressor and limiter of a mixer channel.
#pragma once

#include "core/MixerData.h"
#include "render/Biquad.h"

#include <array>
#include <cstdint>

namespace kk::dsp {

/// Static curve of the compressor: gain reduction (dB, >= 0) for an input level @p levelDb
/// with threshold @p thresholdDb, @p ratio and soft knee width @p kneeDb (0 = hard knee;
/// the quadratic knee of Giannoulis, Massberg and Reiss, "Digital dynamic range compressor
/// design", JAES 2012).
[[nodiscard]] double compressorGainReductionDb(
    double levelDb, double thresholdDb, double ratio, double kneeDb);

/// Static curve of the expander / gate: gain reduction (dB, >= 0) for a key level
/// @p levelDb under @p thresholdDb. Gate mode: the whole @p rangeDb below the threshold;
/// Expander mode: (@p ratio - 1) dB per dB under the threshold, up to @p rangeDb.
[[nodiscard]] double gateGainReductionDb(
    double levelDb, double thresholdDb, double rangeDb, double ratio, GateMode mode);

/// Expander / gate, feed-forward stereo-linked compressor and a peak limiter, in this order,
/// on interleaved stereo.
///
/// Gate / Expander (spec 136): the detector is the larger absolute key sample of both channels
/// in dB. The gate opens when the key passes the threshold, stays open for the hold time after
/// the key fell under the threshold minus the hysteresis, then closes; the wanted reduction
/// (the Range, or the expander curve) is followed with the attack time constant while it
/// falls (opening) and the release time constant while it rises (closing).
///
/// Key: the detector of the gate and the compressor listens to the strip's own signal or, if
/// process() is given a key block, to that signal (side chain, e.g. a voice track that ducks
/// the music); the key goes through a high-pass and a low-pass (Key filters) first. The
/// compressor of an own-signal key listens after the gate.
///
/// Compressor: the detector is the larger absolute sample of both channels in dB; the static
/// curve gives the wanted gain reduction, which a branching one-pole smoother follows with
/// the attack time constant while it rises and the release time constant while it falls (in
/// the dB domain, so the timing does not depend on the level: after a step the reduction
/// reaches 1 - 1/e = 63 % of its final value in one attack time). Make-up gain is added
/// after it. Limiter: instantaneous attack (the reduction needed by the current sample is
/// applied to that sample, so no sample exceeds the ceiling) and a one-pole release.
/// Switching a section on or off, the make-up gain and the ceiling are smoothed (~10 ms).
///
/// Real-time safe: no allocations. Thread-safety: one instance per thread.
class Dynamics
{
public:
    static constexpr int kChannels = 2;

    /// Sets the sample rate (Hz).
    explicit Dynamics(double sampleRate = 48000.0);

    /// New target settings (cheap; call before every process()).
    void setTarget(const DynamicsSettings& settings);
    /// Clears the envelopes and jumps to the target (after a seek).
    void reset();
    /// Processes @p frames interleaved stereo frames in place. @p key (may be null): the
    /// interleaved stereo key signal of the same length that the detectors listen to instead
    /// of the signal itself.
    void process(float* interleaved, int frames, const float* key = nullptr);
    /// True if the processor currently changes the signal.
    [[nodiscard]] bool active() const;
    /// Current total gain reduction in dB (compressor + limiter, >= 0; for the meters).
    [[nodiscard]] float gainReductionDb() const
    {
        return float(m_compGr + m_limGr + m_gateGr * m_gateWet);
    }

private:
    double coefficient(double ms) const;

    double m_sampleRate;
    DynamicsSettings m_target;
    void designKeyFilters();

    double m_gateGr = 0.0; ///< current gate / expander reduction (dB)
    double m_gateWet = 0.0; ///< gate on / off crossfade
    double m_keyEnv = 0.0; ///< peak envelope of the key for the gate (linear)
    double m_keyEnvRelease = 0.0;
    bool m_gateOpen = false;
    bool m_gateFresh = true; ///< the next sample decides the gate state without ramping
    int64_t m_holdLeft = 0; ///< samples the open gate still holds
    double m_gateAttack = 0.0, m_gateRelease = 0.0; ///< one-pole coefficients
    bool m_keyFiltered = false; ///< a key filter is in use
    BiquadCoeffs m_keyHigh, m_keyLow;
    std::array<BiquadState, 2> m_keyHighState, m_keyLowState;
    double m_compGr = 0.0; ///< current compressor reduction (dB)
    double m_limGr = 0.0; ///< current limiter reduction (dB)
    double m_compWet = 0.0; ///< compressor on / off crossfade
    double m_limWet = 0.0;
    double m_makeupDb = 0.0; ///< smoothed make-up gain
    double m_ceilingDb = -1.0; ///< smoothed limiter ceiling
    double m_attack = 0.0, m_release = 0.0, m_limRelease = 0.0; ///< one-pole coefficients
    double m_smooth = 0.0; ///< per-sample smoothing coefficient (~10 ms)
    bool m_fresh = true;
};

} // namespace kk::dsp
