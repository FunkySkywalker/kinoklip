// SPDX-License-Identifier: GPL-3.0-or-later
// ModFx — Chorus, Flanger, Distortion, Stereo Width and the FDN Reverb (spec 138): the
// processors of the audio effects of the same names (clip and track slot). All work on
// interleaved 48 kHz stereo float, in place, one sample at a time (so the result does not
// depend on the block size), with no latency and flushed feedback paths (no denormals).
#pragma once

#include "render/Biquad.h"

#include <array>
#include <vector>

namespace kk::dsp {

/// Chorus: Voices (1..4) modulated delays per channel, panned across the stereo field by Spread.
struct ChorusSettings
{
    double rateHz = 0.8;
    double depthPercent = 40.0; ///< sweep as a share of the delay (100 = +-50 %)
    double delayMs = 15.0;
    double feedbackPercent = 0.0;
    int voices = 2;
    double spreadPercent = 60.0;
    double mixPercent = 50.0;
};

class Chorus
{
public:
    explicit Chorus(double sampleRate = 48000.0);
    void configure(const ChorusSettings& s);
    void reset();
    void process(float* io, int frames);

private:
    static constexpr int kMaxVoices = 4;
    double m_rate;
    ChorusSettings m_s;
    std::array<std::vector<float>, 2> m_line;
    int m_size = 0, m_write = 0;
    double m_phase = 0.0; ///< LFO phase in cycles
};

/// Flanger: one short swept delay per channel with signed feedback.
struct FlangerSettings
{
    double rateHz = 0.3;
    double depthPercent = 70.0; ///< sweep as a share of the delay (100 = 0.1 .. 1.9 times)
    double delayMs = 2.0;
    double feedbackPercent = 50.0; ///< -95 .. 95
    double mixPercent = 50.0;
};

class Flanger
{
public:
    explicit Flanger(double sampleRate = 48000.0);
    void configure(const FlangerSettings& s);
    void reset();
    void process(float* io, int frames);

private:
    double m_rate;
    FlangerSettings m_s;
    std::array<std::vector<float>, 2> m_line;
    int m_size = 0, m_write = 0;
    double m_phase = 0.0;
};

enum class DistortionMode { SoftClip, HardClip, BitCrush, Tube };

struct DistortionSettings
{
    double driveDb = 12.0;
    double tonePercent = 70.0; ///< 0 dark .. 100 bright (low-pass 800 Hz .. 20 kHz)
    DistortionMode mode = DistortionMode::SoftClip;
    double outputDb = -6.0;
    double mixPercent = 100.0;
};

class Distortion
{
public:
    explicit Distortion(double sampleRate = 48000.0);
    void configure(const DistortionSettings& s);
    void reset();
    void process(float* io, int frames);

private:
    double m_rate;
    DistortionSettings m_s;
    double m_toneCoef = 0.0, m_dcCoef = 0.0;
    std::array<double, 2> m_tone{}, m_dcX{}, m_dcY{};
};

/// Stereo width by mid / side. Width 100 % (and Mono Below off) leaves the sound unchanged.
struct StereoWidthSettings
{
    double widthPercent = 100.0; ///< 0 = mono, 200 = side doubled
    double monoBelowHz = 0.0; ///< 0 = off: sound under this frequency is made mono
    double outputDb = 0.0;
};

class StereoWidth
{
public:
    explicit StereoWidth(double sampleRate = 48000.0);
    void configure(const StereoWidthSettings& s);
    void reset();
    void process(float* io, int frames);

private:
    double m_rate;
    StereoWidthSettings m_s;
    BiquadCoeffs m_hp;
    BiquadState m_state;
};

/// Reverb settings. Presets overwrite the room fields (see reverbPreset()).
struct ReverbSettings
{
    double preDelayMs = 10.0;
    double decaySeconds = 1.8; ///< time for the tail to fall by 60 dB (at low frequencies)
    double roomSizePercent = 50.0; ///< length of the delay lines and the early reflections
    double dampingPercent = 30.0; ///< high cut of the tail (0 = bright, 100 = dark)
    double earlyPercent = 40.0; ///< level of the early reflections
    double widthPercent = 100.0; ///< stereo width of the wet sound
    double mixPercent = 25.0;
};

/// The room preset @p index (0 = Small Room, 1 = Large Room, 2 = Hall, 3 = Plate, 4 = Chamber,
/// 5 = Cathedral) as settings (Mix is left at the default).
[[nodiscard]] ReverbSettings reverbPreset(int index);
inline constexpr int kReverbPresetCount = 6;

/// Reverb: pre-delay, 8 early reflections, two diffusers and an 8-line feedback delay network
/// (Householder matrix; the loop gain of every line is set from its length so the tail falls by
/// 60 dB in Decay seconds, with a one-pole high cut in each line for Damping).
class Reverb
{
public:
    explicit Reverb(double sampleRate = 48000.0);
    void configure(const ReverbSettings& s);
    void reset();
    void process(float* io, int frames);

private:
    static constexpr int kLines = 8;
    static constexpr int kTaps = 8;
    struct Line
    {
        std::vector<float> buf;
        int pos = 0, len = 1;
        float lp = 0.0f, gain = 0.0f;
    };
    struct Allpass
    {
        std::vector<float> buf;
        int pos = 0;
    };
    double m_rate;
    ReverbSettings m_s;
    std::vector<float> m_pre; ///< pre-delay (mono)
    int m_preWrite = 0, m_preLen = 0;
    std::vector<float> m_er; ///< early reflection line (mono)
    int m_erWrite = 0;
    std::array<int, kTaps> m_tap{};
    std::array<float, kTaps> m_tapGain{};
    std::array<Line, kLines> m_lines;
    std::array<Allpass, 2> m_diff;
    float m_damp = 0.0f, m_erGain = 0.0f, m_mix = 0.0f, m_side = 1.0f;
    int m_lastRoom = -1;
};

} // namespace kk::dsp
