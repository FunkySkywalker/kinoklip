// SPDX-License-Identifier: GPL-3.0-or-later
// MonitorControls — Dim, Mono and Mute of the Audio page's monitor section (spec 141). They
// change only what the sound device plays: the ring the device reads is the same, the meters,
// the loudness measurement and every render and export never see them. The settings belong to
// the user (QSettings "monitor/..."), not to the project.
#pragma once

#include <atomic>

namespace kk {

/// The state shared between the GUI (sets it) and the audio thread (reads it).
class MonitorControls
{
public:
    static constexpr double kDefaultDimDb = -20.0;
    static constexpr double kMinDimDb = -60.0;
    static constexpr double kMaxDimDb = 0.0;

    /// The one the sound device uses; loaded from the user's settings the first time.
    [[nodiscard]] static MonitorControls& shared();

    void setDim(bool on) { m_dim.store(on, std::memory_order_relaxed); }
    void setDimDb(double db);
    void setMono(bool on) { m_mono.store(on, std::memory_order_relaxed); }
    void setMute(bool on) { m_mute.store(on, std::memory_order_relaxed); }
    [[nodiscard]] bool dim() const { return m_dim.load(std::memory_order_relaxed); }
    [[nodiscard]] double dimDb() const { return m_dimDb.load(std::memory_order_relaxed); }
    [[nodiscard]] bool mono() const { return m_mono.load(std::memory_order_relaxed); }
    [[nodiscard]] bool mute() const { return m_mute.load(std::memory_order_relaxed); }

    /// Saves to / loads from the user's settings (QSettings "monitor/dim", "dimDb", "mono",
    /// "mute"; Mute is not restored on start so the app never opens silent: it loads as off).
    void save() const;
    void load();

private:
    std::atomic<bool> m_dim {false}, m_mono {false}, m_mute {false};
    std::atomic<double> m_dimDb {kDefaultDimDb};
};

/// The audio-thread side: applies the controls to blocks of interleaved stereo float with a
/// 5 ms gain ramp, so switching never clicks. Mono sums L and R ((L + R) / 2 on both sides).
class MonitorProcessor
{
public:
    /// Processes @p frames frames in place. With every control off the block is untouched.
    void process(const MonitorControls& controls, float* io, int frames);
    /// Forgets the ramp (the next block starts at its target).
    void reset() { m_fresh = true; }

private:
    float m_gain = 1.0f;
    float m_mono = 0.0f; ///< 0 = stereo .. 1 = mono (crossfaded)
    bool m_fresh = true;
};

} // namespace kk
