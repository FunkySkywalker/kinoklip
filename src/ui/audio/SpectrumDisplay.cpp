// SPDX-License-Identifier: GPL-3.0-or-later
// SpectrumDisplay implementation.
#include "ui/audio/SpectrumDisplay.h"

#include <algorithm>
#include <cmath>

namespace kk {

double slopeDbPerOctave(SpectrumSlope slope)
{
    switch (slope) {
    case SpectrumSlope::Flat:
        return 0.0;
    case SpectrumSlope::Pink3:
        return 3.0;
    case SpectrumSlope::Mix45:
        return 4.5;
    }
    return 0.0;
}

double smoothingMs(SpectrumSmoothing smoothing)
{
    switch (smoothing) {
    case SpectrumSmoothing::Off:
        return 0.0;
    case SpectrumSmoothing::Fast:
        return 60.0;
    case SpectrumSmoothing::Slow:
        return 250.0;
    }
    return 0.0;
}

void SpectrumDisplay::setPeakHold(bool on)
{
    m_peakHold = on;
    if (!on) {
        for (int b = 0; b < dsp::kSpectrumBands; ++b)
            m_peak[size_t(b)] = m_bar[size_t(b)];
    }
}

void SpectrumDisplay::clear()
{
    m_bar.fill(float(dsp::kSpectrumFloorDb));
    m_peak.fill(float(dsp::kSpectrumFloorDb));
    m_peakAgeMs.fill(0.0);
    m_fresh = false;
}

void SpectrumDisplay::update(const float* bandsDb, double dtMs)
{
    if (m_fresh)
        clear();
    const double tilt = slopeDbPerOctave(m_slope);
    const double tau = smoothingMs(m_smoothing);
    const double k = tau <= 0.0 ? 1.0 : 1.0 - std::exp(-std::max(dtMs, 0.0) / tau);
    for (int b = 0; b < dsp::kSpectrumBands; ++b) {
        const double octaves = std::log2(dsp::spectrumBandHz(b) / 1000.0);
        const float target = bandsDb[b] <= float(dsp::kSpectrumFloorDb)
            ? float(dsp::kSpectrumFloorDb)
            : float(double(bandsDb[b]) + tilt * octaves);
        float& bar = m_bar[size_t(b)];
        bar += float((double(target) - double(bar)) * k);
        if (m_peakHold) {
            if (bar >= m_peak[size_t(b)]) {
                m_peak[size_t(b)] = bar;
                m_peakAgeMs[size_t(b)] = 0.0;
            } else {
                m_peakAgeMs[size_t(b)] += dtMs;
                if (m_peakAgeMs[size_t(b)] > kHoldMs)
                    m_peak[size_t(b)] = std::max(bar,
                        m_peak[size_t(b)] - float(kHoldFallDbPerSecond * dtMs / 1000.0));
            }
        } else {
            m_peak[size_t(b)] = bar;
        }
    }
}

void SpectrumDisplay::decay(double dtMs)
{
    std::array<float, dsp::kSpectrumBands> floor;
    floor.fill(float(dsp::kSpectrumFloorDb));
    // Tilt is added on top of the floor reading, so a falling bar settles on the floor.
    const SpectrumSlope keep = m_slope;
    m_slope = SpectrumSlope::Flat;
    update(floor.data(), dtMs);
    m_slope = keep;
}

} // namespace kk
