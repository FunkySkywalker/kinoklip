// SPDX-License-Identifier: GPL-3.0-or-later
// Spectrum — the Audio page's analysers (spec 141): an FFT spectrum on a log frequency axis, a
// stereo phase correlation meter and a goniometer snapshot. All run on interleaved 48 kHz
// stereo float, one block at a time, and give the same readings for the same sound at any
// block size (the spectrum looks at the last kSpectrumFft samples, the correlation uses a
// block-independent one-pole average).
#pragma once

#include <array>
#include <vector>

namespace kk::dsp {

inline constexpr int kSpectrumFft = 4096; ///< analysis window (11.7 Hz per bin at 48 kHz)
inline constexpr int kSpectrumBands = 256; ///< display bands, log spaced
inline constexpr double kSpectrumMinHz = 20.0;
inline constexpr double kSpectrumMaxHz = 20000.0;
inline constexpr int kSpectrumFloorDb = -120; ///< what silence shows
inline constexpr int kGoniometerPoints = 128; ///< stereo points per snapshot

/// Centre frequency (Hz) of display band @p band (log spaced from 20 Hz to 20 kHz).
[[nodiscard]] double spectrumBandHz(int band);
/// The display band whose centre is closest to @p hz (clamped).
[[nodiscard]] int spectrumBandOf(double hz);

/// A Hann-windowed FFT spectrum of the mono sum (L + R) / 2 of the last kSpectrumFft samples,
/// shown as kSpectrumBands levels in dBFS where a full-scale sine reads 0 dB. A band shows the
/// loudest FFT bin inside it (bins are interpolated where a band is narrower than a bin), so a
/// sine puts its peak in the band nearest its frequency.
class SpectrumAnalyzer
{
public:
    SpectrumAnalyzer();
    void reset();
    /// Appends @p frames interleaved stereo frames.
    void process(const float* interleavedStereo, int frames);
    /// The levels of the last kSpectrumFft samples (kSpectrumFloorDb for silence).
    void bandsDb(float* out) const;
    /// Magnitude of FFT bin @p bin in dBFS of the last window (tests).
    [[nodiscard]] float binDb(int bin) const;

private:
    void analyse() const;
    std::vector<float> m_ring; ///< last kSpectrumFft mono samples, oldest at m_pos
    int m_pos = 0;
    mutable std::vector<float> m_mag; ///< kSpectrumFft / 2 + 1 amplitudes
    mutable bool m_dirty = true;
};

/// Phase correlation of left and right: +1 for the same signal on both sides (mono), 0 for
/// unrelated signals, -1 for opposite phase. Σ LR / sqrt(Σ L² · Σ R²) over an exponential
/// window of about 300 ms; 0 while silent.
class CorrelationMeter
{
public:
    void reset();
    void process(const float* interleavedStereo, int frames);
    [[nodiscard]] double value() const;

private:
    double m_lr = 0.0, m_ll = 0.0, m_rr = 0.0;
};

/// Picks kGoniometerPoints evenly spaced stereo frames of the block as (side, mid) pairs
/// rotated 45 degrees (x = (L - R) / sqrt(2), y = (L + R) / sqrt(2)), so mono is a vertical
/// line. Writes 2 * kGoniometerPoints floats (x, y); fewer frames repeat the last.
void goniometerPoints(const float* interleavedStereo, int frames, float* out);

} // namespace kk::dsp
