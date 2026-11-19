// SPDX-License-Identifier: GPL-3.0-or-later
// TruePeakLimiter — look-ahead true-peak limiter on 48 kHz interleaved stereo (used by the
// render's "Normalize audio to", spec 134).
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace kk::dsp {

/// Look-ahead limiter that keeps the inter-sample (true) peak under a ceiling.
///
/// Detection: every input sample is interpolated 4x (windowed sinc, 12 taps per phase) and the
/// largest of the four phases is the sample's peak. The gain that brings it under the ceiling
/// is minimum-filtered over the look-ahead window, followed with a release (the gain falls at
/// once and recovers with the release time constant) and smoothed with a moving average as
/// long as the window, so it ramps down over the attack time and every sample sees a gain no
/// larger than the one its own peak needs. The audio is delayed by latency() frames so the
/// ramp reaches full depth when the peak arrives.
///
/// The limiter delays: the first latency() output frames are silence and the last latency()
/// input frames come out when latency() frames of silence are processed after them. Real-time
/// safe after construction. Thread-safety: one instance per thread.
class TruePeakLimiter
{
public:
    static constexpr int kChannels = 2;

    /// @p ceilingDb true peak ceiling (dBTP), @p attackMs look-ahead and attack time,
    /// @p releaseMs release time constant.
    explicit TruePeakLimiter(double ceilingDb, double attackMs = 2.0, double releaseMs = 120.0,
        double sampleRate = 48000.0);

    /// Output delay in frames.
    [[nodiscard]] int latency() const { return m_window - 1; }
    /// Processes @p frames interleaved frames of @p in into @p out (may be the same buffer):
    /// the output is the input delayed by latency() frames, limited.
    void process(const float* in, float* out, int frames);
    /// Largest gain reduction (dB, >= 0) applied so far.
    [[nodiscard]] double maxReductionDb() const { return m_maxReductionDb; }
    /// Clears the state.
    void reset();

private:
    static constexpr int kPhases = 4;
    static constexpr int kTaps = 48;
    static constexpr int kPerPhase = kTaps / kPhases;

    double m_ceiling; ///< linear
    int m_window; ///< look-ahead window, frames
    double m_releaseCoef;
    std::array<float, kTaps> m_kernel {};
    std::array<std::array<float, kPerPhase>, 2> m_hist {}; ///< recent input per channel
    int m_histHead = 0;
    std::vector<float> m_req; ///< ring (window) of the required gain per frame
    std::vector<int64_t> m_deque; ///< monotonic deque of frame positions (ring, window + 1)
    int m_dqHead = 0, m_dqCount = 0;
    std::vector<float> m_rel; ///< ring (window) of the released gains
    std::vector<float> m_audio; ///< ring (window) of delayed audio, 2 per frame
    int64_t m_pos = 0; ///< frames processed
    double m_released = 1.0;
    double m_relSum = 0.0; ///< sum of the last window released gains
    double m_maxReductionDb = 0.0;
};

} // namespace kk::dsp
