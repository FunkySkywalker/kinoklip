// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/DeEsser.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::dsp {

namespace {
constexpr double kBandQ = 1.5;
constexpr double kThresholdRelDb = -12.0; ///< band level against the whole sound
constexpr double kQuietDb = -70.0; ///< under this whole-sound level nothing is reduced
constexpr double kFloor = 1e-9;

double onePole(double ms, double rate)
{
    return std::exp(-1.0 / (std::max(ms, 0.01) * 1e-3 * rate));
}

double toDb(double lin)
{
    return 20.0 * std::log10(std::max(lin, kFloor));
}
} // namespace

DeEsser::DeEsser(double sampleRate)
    : m_rate(sampleRate)
{
    configure(m_s);
    reset();
}

void DeEsser::configure(const DeEsserSettings& s)
{
    const bool redesign = s.frequencyHz != m_s.frequencyHz || m_band.isIdentity();
    m_s = s;
    if (redesign) {
        // RBJ band-pass, constant 0 dB peak gain.
        const double f = std::clamp(s.frequencyHz, 100.0, m_rate * 0.45);
        const double w0 = 2.0 * std::numbers::pi * f / m_rate;
        const double alpha = std::sin(w0) / (2.0 * kBandQ);
        const double a0 = 1.0 + alpha;
        m_band.b0 = alpha / a0;
        m_band.b1 = 0.0;
        m_band.b2 = -alpha / a0;
        m_band.a1 = -2.0 * std::cos(w0) / a0;
        m_band.a2 = (1.0 - alpha) / a0;
    }
    m_attack = onePole(s.reactionMs / 5.0, m_rate);
    m_release = onePole(s.reactionMs, m_rate);
    m_envAttack = onePole(0.1, m_rate);
    m_bandRelease = onePole(3.0, m_rate);
    m_wideRelease = onePole(30.0, m_rate);
}

void DeEsser::reset()
{
    for (BiquadState& st : m_state)
        st.reset();
    m_bandEnv = m_wideEnv = 0.0;
    m_gr = 0.0;
}

void DeEsser::process(float* io, int frames)
{
    const double range = std::max(m_s.rangeDb, 0.0);
    const double slope = m_s.amountPercent / 50.0;
    for (int i = 0; i < frames; ++i) {
        const double x[2] = {double(io[size_t(i) * 2]), double(io[size_t(i) * 2 + 1])};
        double band[2];
        for (int c = 0; c < 2; ++c)
            band[c] = m_state[size_t(c)].process(m_band, x[c]);
        const double bandPeak = std::max(std::abs(band[0]), std::abs(band[1]));
        const double widePeak = std::max(std::abs(x[0]), std::abs(x[1]));
        m_bandEnv = bandPeak > m_bandEnv ? bandPeak + (m_bandEnv - bandPeak) * m_envAttack
                                         : bandPeak + (m_bandEnv - bandPeak) * m_bandRelease;
        m_wideEnv = widePeak > m_wideEnv ? widePeak + (m_wideEnv - widePeak) * m_envAttack
                                         : widePeak + (m_wideEnv - widePeak) * m_wideRelease;
        const double wideDb = toDb(m_wideEnv);
        double want = 0.0;
        if (wideDb > kQuietDb) {
            const double over = toDb(m_bandEnv) - wideDb - kThresholdRelDb;
            want = std::clamp(over * slope, 0.0, range);
        }
        m_gr = want + (m_gr - want) * (want > m_gr ? m_attack : m_release);
        if (m_s.listen) {
            io[size_t(i) * 2] = float(band[0]);
            io[size_t(i) * 2 + 1] = float(band[1]);
        } else if (m_gr > 1e-4) {
            const double g = std::pow(10.0, -m_gr / 20.0) - 1.0;
            io[size_t(i) * 2] = float(x[0] + g * band[0]);
            io[size_t(i) * 2 + 1] = float(x[1] + g * band[1]);
        }
    }
}

// ---- Dialogue Leveler ----

namespace {
constexpr double kLevelMs = 100.0;
constexpr double kHoldMs = 600.0;
constexpr double kHysteresisDb = 3.0;

double speedMs(LevelerSpeed s)
{
    switch (s) {
    case LevelerSpeed::Slow:
        return 2000.0;
    case LevelerSpeed::Fast:
        return 250.0;
    case LevelerSpeed::Medium:
        break;
    }
    return 700.0;
}
} // namespace

DialogueLeveler::DialogueLeveler(double sampleRate)
    : m_rate(sampleRate)
    , m_levelCoef(onePole(kLevelMs, sampleRate))
{
    reset();
}

void DialogueLeveler::configure(const LevelerSettings& s)
{
    m_s = s;
}

void DialogueLeveler::reset()
{
    m_power = 0.0;
    m_gain = 0.0;
    m_bg = 0.0;
    m_speech = false;
    m_hold = 0;
}

void DialogueLeveler::process(float* io, int frames)
{
    const double tauMs = speedMs(m_s.speed);
    const double upCoef = onePole(tauMs, m_rate);
    const double downCoef = onePole(tauMs / 3.0, m_rate);
    const double relaxCoef = onePole(tauMs * 3.0, m_rate);
    const double bgCoef = onePole(300.0, m_rate);
    const long long holdSamples = (long long)(kHoldMs * 1e-3 * m_rate);
    for (int i = 0; i < frames; ++i) {
        float& l = io[size_t(i) * 2];
        float& r = io[size_t(i) * 2 + 1];
        const double ms = 0.5 * (double(l) * l + double(r) * r);
        m_power = ms + (m_power - ms) * m_levelCoef;
        const double levelDb = 10.0 * std::log10(std::max(m_power, 1e-18));
        if (levelDb >= m_s.noiseFloorDb) {
            m_speech = true;
            m_hold = holdSamples;
        } else if (levelDb >= m_s.noiseFloorDb - kHysteresisDb) {
            if (m_speech)
                m_hold = holdSamples;
        } else if (m_speech) {
            if (m_hold > 0)
                --m_hold;
            else
                m_speech = false;
        }
        double want, coef;
        if (m_speech) {
            // Slow while the level is being tracked; the gain follows the 100 ms level.
            want = std::clamp(m_s.targetDb - levelDb, -m_s.maxDownDb, m_s.maxUpDb);
            coef = want < m_gain ? downCoef : upCoef;
        } else {
            want = 0.0;
            coef = relaxCoef;
        }
        m_gain = want + (m_gain - want) * coef;
        const double bgWant = m_speech ? 0.0 : -std::max(m_s.backgroundDb, 0.0);
        m_bg = bgWant + (m_bg - bgWant) * bgCoef;
        const double total = m_gain + m_bg;
        if (total != 0.0) {
            const float g = float(std::pow(10.0, total / 20.0));
            l *= g;
            r *= g;
        }
    }
}

} // namespace kk::dsp
