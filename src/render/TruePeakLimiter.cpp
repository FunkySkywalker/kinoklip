// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/TruePeakLimiter.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::dsp {

namespace {

constexpr double kKaiserBeta = 5.0; ///< the window of LoudnessMeter's true-peak interpolator

double besselI0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 32; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

} // namespace

TruePeakLimiter::TruePeakLimiter(
    double ceilingDb, double attackMs, double releaseMs, double sampleRate)
    : m_ceiling(std::pow(10.0, ceilingDb / 20.0))
    , m_window(std::max(2, int(std::lround(attackMs * 0.001 * sampleRate))))
    , m_releaseCoef(std::exp(-1.0 / (std::max(1.0, releaseMs) * 0.001 * sampleRate)))
{
    // Kaiser-windowed sinc (the same interpolator as LoudnessMeter's true peak), 4x: tap n sits (n - centre) / 4 input samples from the centre; every
    // phase is normalised to unity gain at DC.
    const double centre = (kTaps - 1) / 2.0;
    std::array<double, kTaps> h {};
    for (int n = 0; n < kTaps; ++n) {
        const double t = (n - centre) / kPhases;
        const double sinc
            = t == 0.0 ? 1.0 : std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
        const double r = (n - centre) / centre;
        const double w
            = besselI0(kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(kKaiserBeta);
        h[size_t(n)] = sinc * w;
    }
    for (int p = 0; p < kPhases; ++p) {
        double sum = 0.0;
        for (int k = 0; k < kPerPhase; ++k)
            sum += h[size_t(p + kPhases * k)];
        for (int k = 0; k < kPerPhase; ++k)
            m_kernel[size_t(p + kPhases * k)] = float(h[size_t(p + kPhases * k)] / sum);
    }
    m_req.assign(size_t(m_window), 1.0f);
    m_deque.assign(size_t(m_window) + 1, 0);
    m_rel.assign(size_t(m_window), 1.0f);
    m_audio.assign(size_t(m_window) * kChannels, 0.0f);
    reset();
}

void TruePeakLimiter::reset()
{
    for (auto& h : m_hist)
        h.fill(0.0f);
    m_histHead = 0;
    std::fill(m_req.begin(), m_req.end(), 1.0f);
    std::fill(m_rel.begin(), m_rel.end(), 1.0f);
    std::fill(m_audio.begin(), m_audio.end(), 0.0f);
    m_dqHead = m_dqCount = 0;
    m_pos = 0;
    m_released = 1.0;
    m_relSum = double(m_window);
    m_maxReductionDb = 0.0;
}

void TruePeakLimiter::process(const float* in, float* out, int frames)
{
    const int W = m_window;
    const int dqSize = W + 1;
    for (int f = 0; f < frames; ++f) {
        const float l = in[size_t(f) * 2], r = in[size_t(f) * 2 + 1];
        // Peak of this frame: the sample itself and the three interpolated points around it
        // (the history is the last 12 inputs per channel; the interpolation is centred 6
        // samples back, which is a constant delay of the detector, covered by the window).
        m_hist[0][size_t(m_histHead)] = l;
        m_hist[1][size_t(m_histHead)] = r;
        m_histHead = (m_histHead + 1) % kPerPhase;
        float peak = std::max(std::abs(l), std::abs(r));
        for (int c = 0; c < 2; ++c) {
            for (int p = 0; p < kPhases; ++p) {
                float y = 0.0f;
                for (int k = 0; k < kPerPhase; ++k) {
                    const int idx = (m_histHead - 1 - k + kPerPhase * 2) % kPerPhase;
                    y += m_kernel[size_t(p + kPhases * k)] * m_hist[size_t(c)][size_t(idx)];
                }
                peak = std::max(peak, std::abs(y));
            }
        }
        const float req = peak > float(m_ceiling) ? float(m_ceiling / double(peak)) : 1.0f;
        const int64_t n = m_pos;
        m_req[size_t(n % W)] = req;
        // Monotonic deque for the minimum over the last W required gains.
        while (m_dqCount > 0) {
            const int64_t back = m_deque[size_t((m_dqHead + m_dqCount - 1) % dqSize)];
            if (m_req[size_t(back % W)] >= req)
                --m_dqCount;
            else
                break;
        }
        m_deque[size_t((m_dqHead + m_dqCount) % dqSize)] = n;
        ++m_dqCount;
        while (m_deque[size_t(m_dqHead)] <= n - W) {
            m_dqHead = (m_dqHead + 1) % dqSize;
            --m_dqCount;
        }
        const double minGain = double(m_req[size_t(m_deque[size_t(m_dqHead)] % W)]);
        // Release: fall at once, recover with the time constant.
        m_released = std::min(minGain, 1.0 - (1.0 - m_released) * m_releaseCoef);
        const int slot = int(n % W);
        m_relSum += m_released - double(m_rel[size_t(slot)]);
        m_rel[size_t(slot)] = float(m_released);
        const double gain = m_relSum / W;
        // The audio delay line: read the frame leaving, store the one arriving.
        const size_t old = size_t((n + 1) % W) * 2; // the frame from W - 1 frames ago
        const float dl = m_audio[old], dr = m_audio[old + 1];
        m_audio[size_t(slot) * 2] = l;
        m_audio[size_t(slot) * 2 + 1] = r;
        // The delayed frame is W - 1 frames old: every gain averaged into `gain` was
        // minimum-filtered over a window that contains that frame.
        out[size_t(f) * 2] = float(double(dl) * gain);
        out[size_t(f) * 2 + 1] = float(double(dr) * gain);
        if (gain < 1.0)
            m_maxReductionDb = std::max(m_maxReductionDb, -20.0 * std::log10(std::max(gain, 1e-6)));
        ++m_pos;
    }
}

} // namespace kk::dsp
