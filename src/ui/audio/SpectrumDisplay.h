// SPDX-License-Identifier: GPL-3.0-or-later
// SpectrumDisplay — the display state of the spectrum analyser (spec 141): slope compensation,
// smoothing and peak hold applied to the bands of one MeterFrame. Pure logic, unit-tested; the
// widget (AnalyzerPanel.cpp) only paints what it holds.
#pragma once

#include "render/Spectrum.h"

#include <array>

namespace kk {

/// The tilt of the display, in dB per octave around 1 kHz: 0 is the plain spectrum, 3 makes
/// pink noise flat, 4.5 is the usual mastering view (the high end is lifted so a full mix is
/// level).
enum class SpectrumSlope { Flat = 0, Pink3 = 1, Mix45 = 2 };
/// How much the bars lag the signal.
enum class SpectrumSmoothing { Off = 0, Fast = 1, Slow = 2 };

[[nodiscard]] double slopeDbPerOctave(SpectrumSlope slope);
/// Time constant (ms) of the smoothing, 0 for Off.
[[nodiscard]] double smoothingMs(SpectrumSmoothing smoothing);

class SpectrumDisplay
{
public:
    static constexpr double kHoldMs = 1500.0; ///< the peak mark stays this long
    static constexpr double kHoldFallDbPerSecond = 12.0; ///< then it falls

    void setSlope(SpectrumSlope slope) { m_slope = slope; }
    void setSmoothing(SpectrumSmoothing smoothing) { m_smoothing = smoothing; }
    void setPeakHold(bool on);
    [[nodiscard]] SpectrumSlope slope() const { return m_slope; }
    [[nodiscard]] SpectrumSmoothing smoothing() const { return m_smoothing; }
    [[nodiscard]] bool peakHold() const { return m_peakHold; }

    /// Takes the bands (dBFS, dsp::kSpectrumBands values) of a reading @p dtMs after the last
    /// one. @p silent (no sound, stopped): the bars fall to the floor.
    void update(const float* bandsDb, double dtMs);
    /// Lets the display fall (no new reading).
    void decay(double dtMs);
    void clear();

    /// The bar of band @p band (dB, tilted and smoothed) and its peak mark.
    [[nodiscard]] float barDb(int band) const { return m_bar[size_t(band)]; }
    [[nodiscard]] float peakDb(int band) const { return m_peak[size_t(band)]; }

private:
    SpectrumSlope m_slope = SpectrumSlope::Flat;
    SpectrumSmoothing m_smoothing = SpectrumSmoothing::Fast;
    bool m_peakHold = true;
    std::array<float, dsp::kSpectrumBands> m_bar {}, m_peak {};
    std::array<double, dsp::kSpectrumBands> m_peakAgeMs {};
    bool m_fresh = true;
};

} // namespace kk
