// SPDX-License-Identifier: GPL-3.0-or-later
// Dynamics — compressor (branching dB-domain envelope) and peak limiter.
#include "render/Dynamics.h"

#include <algorithm>
#include <cmath>

namespace kk::dsp {

namespace {
/// Smoothing time of the on / off switches, make-up gain and ceiling.
constexpr double kSmoothSeconds = 0.010;
/// Detector floor: -180 dBFS (silence would be -inf).
constexpr double kFloor = 1e-9;
constexpr double kSnap = 1e-6;
/// Release of the key's peak envelope that the gate listens to (a sine's dips do not close it).
constexpr double kKeyEnvMs = 10.0;
} // namespace

double compressorGainReductionDb(double levelDb, double thresholdDb, double ratio, double kneeDb)
{
    const double slope = 1.0 - 1.0 / std::max(ratio, 1.0);
    const double over = levelDb - thresholdDb;
    if (kneeDb > 0.0) {
        if (2.0 * over < -kneeDb)
            return 0.0;
        if (2.0 * std::abs(over) <= kneeDb) {
            const double x = over + kneeDb / 2.0;
            return slope * x * x / (2.0 * kneeDb);
        }
    }
    return over > 0.0 ? slope * over : 0.0;
}

double gateGainReductionDb(
    double levelDb, double thresholdDb, double rangeDb, double ratio, GateMode mode)
{
    const double under = thresholdDb - levelDb;
    if (under <= 0.0)
        return 0.0;
    if (mode == GateMode::Gate)
        return std::max(rangeDb, 0.0);
    return std::min(under * (std::max(ratio, 1.0) - 1.0), std::max(rangeDb, 0.0));
}

Dynamics::Dynamics(double sampleRate)
    : m_sampleRate(sampleRate)
    , m_smooth(1.0 - std::exp(-1.0 / (kSmoothSeconds * sampleRate)))
{
    // Coefficients of the defaults; the first setTarget() still jumps (m_fresh).
    m_attack = coefficient(m_target.attackMs);
    m_release = coefficient(m_target.releaseMs);
    m_limRelease = coefficient(m_target.limiterReleaseMs);
    m_gateAttack = coefficient(m_target.gateAttackMs);
    m_gateRelease = coefficient(m_target.gateReleaseMs);
    designKeyFilters();
    reset();
}

void Dynamics::designKeyFilters()
{
    const DynamicsSettings& t = m_target;
    constexpr double kQ = 0.7071;
    m_keyEnvRelease = std::exp(-1.0 / (kKeyEnvMs * 1e-3 * m_sampleRate));
    m_keyFiltered = t.keyLowCutHz > kKeyMinHz + 0.01 || t.keyHighCutHz < kKeyMaxHz - 0.01;
    m_keyHigh = t.keyLowCutHz > kKeyMinHz + 0.01
        ? designBand(EqBandType::HighPass, t.keyLowCutHz, 0.0, kQ, m_sampleRate)
        : BiquadCoeffs {};
    m_keyLow = t.keyHighCutHz < kKeyMaxHz - 0.01
        ? designBand(EqBandType::LowPass, t.keyHighCutHz, 0.0, kQ, m_sampleRate)
        : BiquadCoeffs {};
}

double Dynamics::coefficient(double ms) const
{
    return std::exp(-1.0 / (std::max(ms, kDynMinTimeMs) * 1e-3 * m_sampleRate));
}

void Dynamics::setTarget(const DynamicsSettings& s)
{
    if (!(s == m_target) || m_fresh) {
        m_target = s;
        m_attack = coefficient(s.attackMs);
        m_release = coefficient(s.releaseMs);
        m_limRelease = coefficient(s.limiterReleaseMs);
        m_gateAttack = coefficient(s.gateAttackMs);
        m_gateRelease = coefficient(s.gateReleaseMs);
        designKeyFilters();
    }
    if (m_fresh) {
        m_fresh = false;
        reset();
    }
}

void Dynamics::reset()
{
    m_gateGr = 0.0;
    m_gateWet = m_target.gateEnabled ? 1.0 : 0.0;
    m_gateOpen = false;
    m_gateFresh = true;
    m_holdLeft = 0;
    m_keyEnv = 0.0;
    for (BiquadState& st : m_keyHighState)
        st.reset();
    for (BiquadState& st : m_keyLowState)
        st.reset();
    m_compGr = 0.0;
    m_limGr = 0.0;
    m_compWet = m_target.compressorEnabled ? 1.0 : 0.0;
    m_limWet = m_target.limiterEnabled ? 1.0 : 0.0;
    m_makeupDb = m_target.makeupDb;
    m_ceilingDb = m_target.limiterCeilingDb;
}

bool Dynamics::active() const
{
    return m_target.gateEnabled || m_target.compressorEnabled || m_target.limiterEnabled
        || m_gateWet > 0.0 || m_compWet > 0.0 || m_limWet > 0.0;
}

void Dynamics::process(float* io, int frames, const float* key)
{
    if (!active())
        return;
    const DynamicsSettings& t = m_target;
    const double gateTarget = t.gateEnabled ? 1.0 : 0.0;
    const double compTarget = t.compressorEnabled ? 1.0 : 0.0;
    const double limTarget = t.limiterEnabled ? 1.0 : 0.0;
    const bool external = key != nullptr;
    const int64_t holdSamples = int64_t(std::llround(t.gateHoldMs * 1e-3 * m_sampleRate));
    const double closeDb = t.gateThresholdDb - t.gateHysteresisDb;
    for (int i = 0; i < frames; ++i) {
        float& l = io[size_t(i) * 2];
        float& r = io[size_t(i) * 2 + 1];
        // Switches, make-up and ceiling glide (per sample: cheap next to the log / exp).
        m_gateWet += (gateTarget - m_gateWet) * m_smooth;
        m_compWet += (compTarget - m_compWet) * m_smooth;
        m_limWet += (limTarget - m_limWet) * m_smooth;
        m_makeupDb += (t.makeupDb - m_makeupDb) * m_smooth;
        m_ceilingDb += (t.limiterCeilingDb - m_ceilingDb) * m_smooth;
        const double sigPeak = std::max(std::abs(double(l)), std::abs(double(r)));
        // The key sample (own signal or the side chain), through the key filters.
        double kl = external ? double(key[size_t(i) * 2]) : double(l);
        double kr = external ? double(key[size_t(i) * 2 + 1]) : double(r);
        if (m_keyFiltered) {
            kl = m_keyLowState[0].process(m_keyLow, m_keyHighState[0].process(m_keyHigh, kl));
            kr = m_keyLowState[1].process(m_keyLow, m_keyHighState[1].process(m_keyHigh, kr));
        }
        const double keyPeak = std::max(std::abs(kl), std::abs(kr));
        const double keyDb = 20.0 * std::log10(std::max(keyPeak, kFloor));
        m_keyEnv = keyPeak > m_keyEnv ? keyPeak : keyPeak + (m_keyEnv - keyPeak) * m_keyEnvRelease;
        const double gateKeyDb = 20.0 * std::log10(std::max(m_keyEnv, kFloor));

        double gainDb = 0.0;
        double gateLin = 1.0;
        if (m_gateWet > kSnap) {
            if (m_gateFresh) {
                m_gateOpen = gateKeyDb >= t.gateThresholdDb;
                m_holdLeft = holdSamples;
            }
            if (gateKeyDb >= t.gateThresholdDb) {
                m_gateOpen = true;
                m_holdLeft = holdSamples;
            } else if (gateKeyDb >= closeDb) {
                if (m_gateOpen)
                    m_holdLeft = holdSamples; // between the thresholds: stays open
            } else if (m_gateOpen) {
                if (m_holdLeft > 0)
                    --m_holdLeft;
                else
                    m_gateOpen = false;
            }
            const double want = m_gateOpen ? 0.0
                                           : gateGainReductionDb(gateKeyDb, t.gateThresholdDb,
                                               t.gateRangeDb, t.gateRatio, t.gateMode);
            if (m_gateFresh) {
                m_gateGr = want;
                m_gateFresh = false;
            } else {
                const double k = want < m_gateGr ? m_gateAttack : m_gateRelease;
                m_gateGr = want + (m_gateGr - want) * k;
            }
            gainDb = -m_gateWet * m_gateGr;
            gateLin = std::pow(10.0, gainDb / 20.0);
        } else {
            m_gateGr = 0.0;
            m_gateOpen = false;
            m_gateFresh = true;
        }
        if (m_compWet > kSnap) {
            // An own-signal key is heard after the gate; a side chain is not gated.
            const double compKeyDb = external || gateLin == 1.0
                ? keyDb
                : 20.0 * std::log10(std::max(keyPeak * gateLin, kFloor));
            const double want
                = compressorGainReductionDb(compKeyDb, t.thresholdDb, t.ratio, t.kneeDb);
            const double k = want > m_compGr ? m_attack : m_release;
            m_compGr = want + (m_compGr - want) * k;
            gainDb += m_compWet * (m_makeupDb - m_compGr);
        } else {
            m_compGr = 0.0;
        }
        if (m_limWet > kSnap) {
            const double levelDb = 20.0 * std::log10(std::max(sigPeak, kFloor));
            const double need = std::max(0.0, levelDb + gainDb - m_ceilingDb);
            m_limGr = need >= m_limGr ? need : need + (m_limGr - need) * m_limRelease;
            gainDb -= m_limWet * m_limGr;
        } else {
            m_limGr = 0.0;
        }
        if (gainDb != 0.0) {
            const float g = float(std::pow(10.0, gainDb / 20.0));
            l *= g;
            r *= g;
        }
    }
    if (gateTarget == 0.0 && m_gateWet < kSnap)
        m_gateWet = 0.0;
    if (compTarget == 0.0 && m_compWet < kSnap)
        m_compWet = 0.0;
    if (limTarget == 0.0 && m_limWet < kSnap)
        m_limWet = 0.0;
}

} // namespace kk::dsp
