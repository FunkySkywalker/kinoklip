// SPDX-License-Identifier: GPL-3.0-or-later
// AudioNormalize — measuring clips for Clip > Normalize Audio Levels (sample peak, true peak,
// BS.1770 integrated loudness) and turning the measurements into clip gains (spec 37.35).
#pragma once

#include "core/ClipAttributes.h"
#include "core/MixerOps.h"
#include "render/RenderTypes.h"

#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace kk::audionorm {

/// What the target level measures (Resolve's normalization modes).
enum class Mode {
    SamplePeak, ///< Sample Peak Program: the largest sample, dBFS
    TruePeak, ///< the largest inter-sample peak (4x oversampled, BS.1770 Annex 2), dBTP
    Loudness ///< ITU-R BS.1770-4 integrated (gated) loudness, LUFS
};

/// How several clips are levelled.
enum class Scope {
    Relative, ///< one common gain: the loudest clip reaches the target, the others keep
              ///< their level relative to it
    Independent ///< every clip gets its own gain and reaches the target
};

/// Default target level of @p mode (dBFS / dBTP / LUFS): -9 dBFS sample peak, -2 dBTP true
/// peak, -23 LUFS loudness (EBU R128).
[[nodiscard]] double defaultTarget(Mode mode);

/// The audio of one clip to measure: a media file and its source range.
struct Source
{
    QString path;
    int64_t startUs = 0; ///< first source microsecond of the clip
    int64_t endUs = 0; ///< end of the clip's source range (exclusive)
    ClipAttributes attributes; ///< channel routing (Clip Attributes)
    int audioStream = -1; ///< ClipData::audioStream
};

/// Levels of one clip's source audio (before the clip's own volume).
struct Measurement
{
    bool valid = false; ///< false: unreadable file, no audio, or cancelled
    double samplePeakDb = -200.0; ///< dBFS
    double truePeakDb = -200.0; ///< dBTP
    double loudnessLufs = -200.0; ///< integrated, LUFS (-200 = silence / too short)
    double loudnessRange = 0.0; ///< LRA in LU (EBU Tech 3342)
    QString error;
};

/// Level of @p m in @p mode's unit.
[[nodiscard]] double levelOf(const Measurement& m, Mode mode);

/// Measures @p source: decodes its range at 48 kHz stereo (media::AudioDecoder) in blocks,
/// feeding dsp::LoudnessMeter (true peak, integrated loudness) and a plain sample peak.
/// @p cancel (may be null) is polled between blocks; @p progress (may be empty) receives the
/// fraction done of this clip. Runs on any thread (one decoder per call).
[[nodiscard]] Measurement measure(const Source& source, const std::atomic<bool>* cancel = nullptr,
    const std::function<void(double)>& progress = {});

/// Measures the mix of @p timeline over the timeline frames [@p firstFrame, @p endFrame) with
/// the real mixer (AudioMixer, the same code as playback and render): the signal of mixer
/// channel @p channel (a track after its fader, or a bus after its fader) or, with no channel,
/// the main mix (the output bus, as heard and rendered). Blocks of 100 ms feed dsp::LoudnessMeter
/// and a plain sample peak; @p cancel and @p progress work as in measure(). The result is
/// invalid (error "no audio") for an empty range. Runs on any thread: it only reads
/// @p timeline and uses its own mixer.
[[nodiscard]] Measurement measureMix(const TimelineData& timeline,
    std::shared_ptr<const MediaPathMap> media, const std::optional<MixTarget>& channel,
    int64_t firstFrame, int64_t endFrame, const std::atomic<bool>* cancel = nullptr,
    const std::function<void(double)>& progress = {});

/// The gain (and limiting) that brings a mix to a loudness target under a true peak ceiling.
struct NormalizePlan
{
    bool valid = false; ///< false: cancelled, empty or silent mix (see error)
    QString error;
    double measuredLufs = -200.0; ///< integrated loudness of the mix as it is
    double gainDb = 0.0; ///< gain applied before the limiter
    bool limited = false; ///< the true peak ceiling needs the look-ahead limiter
    double limiterCeilingDb = 0.0; ///< the ceiling the limiter works to (dBTP)
    double resultLufs = -200.0; ///< loudness after gain and limiter, re-measured
    double resultTruePeakDb = -200.0;
    double maxReductionDb = 0.0; ///< deepest limiter gain reduction
    bool reached = true; ///< resultLufs is within kNormalizeToleranceLu of the target
    bool heavyLimiting = false; ///< the limiter takes more than kHeavyLimitingDb off a peak
    int passes = 0; ///< measuring passes after the first
};

constexpr double kNormalizeToleranceLu = 0.5; ///< "within 0.5 LU" of the target
constexpr double kHeavyLimitingDb = 6.0;

/// Plans "Normalize audio to" for @p timeline over [@p firstFrame, @p endFrame): measures the
/// integrated loudness, sets the linear gain target - measured and, if that would push the true
/// peak over @p ceilingDb, runs the look-ahead true-peak limiter (dsp::TruePeakLimiter) and
/// corrects the gain in measured passes (as FFmpeg's loudnorm does in two-pass mode): the
/// limited result is re-measured and the gain adjusted (secant steps on the measured error)
/// until it lands within 0.1 LU of @p targetLufs, or the target cannot be reached under the
/// ceiling (the limiter's loudness saturates), in which case `reached` is false and
/// `resultLufs` tells how far it got. Runs on any thread; @p cancel and @p progress as in
/// measureMix().
[[nodiscard]] NormalizePlan planNormalize(const TimelineData& timeline,
    std::shared_ptr<const MediaPathMap> media, int64_t firstFrame, int64_t endFrame,
    double targetLufs, double ceilingDb, const std::atomic<bool>* cancel = nullptr,
    const std::function<void(double)>& progress = {});

/// The timeline range a mix is normalized over: the In / Out marks (an unset mark keeps the
/// edge of the timeline), as the render's In / Out range; [first, end) in timeline frames.
[[nodiscard]] std::pair<int64_t, int64_t> mixRange(const TimelineData& timeline);

/// The change (dB) of one fader that brings @p measured to @p target (both in the mode's
/// unit): target - measured; NaN for a silent measurement (nothing to normalize).
[[nodiscard]] double faderChange(double measured, double target);

/// The clip gains (dB) that bring @p levels (in the mode's unit, one per clip) to @p target.
/// Relative: target - max(levels) for every clip; Independent: target - level per clip.
/// Entries with a silent level (<= -150, nothing to normalize) get NaN (left unchanged).
[[nodiscard]] std::vector<double> gains(
    const std::vector<double>& levels, double target, Scope scope);

} // namespace kk::audionorm
