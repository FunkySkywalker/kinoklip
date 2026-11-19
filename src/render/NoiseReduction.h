// SPDX-License-Identifier: GPL-3.0-or-later
// NoiseReduction — the processor of the Noise Reduction audio effect (spec 132): a spectral gate
// of our own (STFT, a noise estimate per bin, smoothed gains) and RNNoise (third_party/rnnoise,
// BSD-3-Clause) as an optional neural engine, with the Threshold / Ratio expander, Reduction,
// Dry / Wet and Output Gain around either. Interleaved 48 kHz stereo float, in place.
#pragma once

#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

namespace kk {
struct ClipAttributes;
}

namespace kk::dsp::noise {

inline constexpr int kFftSize = 1024; ///< spectral gate window
inline constexpr int kGateHop = kFftSize / 2; ///< 50 % overlap, sqrt-Hann windows
inline constexpr int kBins = kFftSize / 2 + 1;
inline constexpr int kRnnoiseHop = 480; ///< RNNoise frame: 10 ms at 48 kHz

/// A noise profile: the mean power per FFT bin (dB) of a stretch that holds only noise.
struct Profile
{
    std::vector<float> powerDb; ///< kBins values; empty = nothing learned

    [[nodiscard]] bool valid() const { return int(powerDb.size()) == kBins; }
    friend bool operator==(const Profile&, const Profile&) = default;
};

/// The profile as text stored in the project ("v1:" + base64 of one byte per bin, 0.67 dB
/// steps). Empty text for an empty profile.
[[nodiscard]] QString encodeProfile(const Profile& profile);
/// The profile of @p text; empty if the text is empty, damaged or from an unknown version.
[[nodiscard]] Profile decodeProfile(const QString& text);

/// Learns a profile from @p frames frames of interleaved stereo (the channels are averaged).
/// Empty if there is less than a few windows of sound (about 50 ms).
[[nodiscard]] Profile learnProfile(const float* interleavedStereo, int frames);

/// Learns a profile from the source range [startUs, endUs) of a media file (decoded at 48 kHz
/// stereo, at most the first 10 s of the range). Runs on any thread. @p error gets the reason
/// when the result is empty.
[[nodiscard]] Profile learnProfileFromFile(const QString& path, int64_t startUs, int64_t endUs,
    const ClipAttributes& attributes, int audioStream, QString* error = nullptr);

/// True if the RNNoise engine is part of this build (always, it is built from source).
[[nodiscard]] bool rnnoiseAvailable();

struct Settings
{
    bool learnMode = false; ///< Manual Learn: the gate with the learned profile
    bool rnnoise = true; ///< Auto Speech: RNNoise (true) or the adaptive spectral gate (false)
    double thresholdDb = -70.0;
    double attackMs = 20.0;
    double sensitivity = 50.0; ///< percent
    double ratio = 2.0;
    double reductionDb = 20.0;
    double mix = 100.0; ///< percent wet
    double outputDb = 0.0;
};

/// The stereo processor. Its output is delayed by latencyFrames() (the sound is cut into
/// blocks; the dry part of Dry / Wet is delayed the same, so the two stay aligned); the result
/// does not depend on how the input is split into calls. With Dry / Wet at 0 and no Output
/// Gain the input passes through untouched and without delay. Not thread-safe.
class Reducer
{
public:
    Reducer();
    ~Reducer();
    Reducer(const Reducer&) = delete;
    Reducer& operator=(const Reducer&) = delete;

    void setProfile(const Profile& profile);
    /// Takes the settings (before every block). A change of engine clears the memory.
    void configure(const Settings& settings);
    void reset();
    void process(float* io, int frames);

    /// The delay of the processed sound for the current engine, in frames.
    [[nodiscard]] int latencyFrames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace kk::dsp::noise
