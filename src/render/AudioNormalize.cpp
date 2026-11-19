// SPDX-License-Identifier: GPL-3.0-or-later
// AudioNormalize implementation.
#include "render/AudioNormalize.h"

#include "media/AudioDecoder.h"
#include "render/AudioMixer.h"
#include "render/LoudnessMeter.h"
#include "render/TruePeakLimiter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kk::audionorm {

namespace {

constexpr int kBlockFrames = 4800; ///< 100 ms per read: one meter block, cheap cancel checks
constexpr double kSilentDb = -150.0; ///< levels at or below this have nothing to normalize

double toDb(double linear)
{
    return linear > 0.0 ? 20.0 * std::log10(linear) : dsp::LoudnessMeter::kSilence;
}

} // namespace

double defaultTarget(Mode mode)
{
    switch (mode) {
    case Mode::SamplePeak:
        return -9.0;
    case Mode::TruePeak:
        return -2.0;
    case Mode::Loudness:
        break;
    }
    return -23.0;
}

double levelOf(const Measurement& m, Mode mode)
{
    switch (mode) {
    case Mode::SamplePeak:
        return m.samplePeakDb;
    case Mode::TruePeak:
        return m.truePeakDb;
    case Mode::Loudness:
        break;
    }
    return m.loudnessLufs;
}

Measurement measure(const Source& source, const std::atomic<bool>* cancel,
    const std::function<void(double)>& progress)
{
    Measurement result;
    media::AudioDecoder decoder;
    if (!decoder.open(source.path, &result.error, source.audioStream))
        return result;
    decoder.setChannelMapping(source.attributes);
    constexpr int rate = media::AudioDecoder::kSampleRate;
    const int64_t first = source.startUs * rate / 1000000;
    const int64_t last = std::max(first, source.endUs * rate / 1000000);
    const int64_t total = last - first;

    dsp::LoudnessMeter meter; // one per measurement: the integrated value covers this clip
    std::vector<float> block(size_t(kBlockFrames) * media::AudioDecoder::kChannels);
    float peak = 0.0f;
    for (int64_t at = first; at < last; at += kBlockFrames) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            result.error = QStringLiteral("cancelled");
            return result;
        }
        const int frames = int(std::min<int64_t>(kBlockFrames, last - at));
        decoder.read(at, block.data(), frames);
        for (int i = 0; i < frames * media::AudioDecoder::kChannels; ++i)
            peak = std::max(peak, std::abs(block[size_t(i)]));
        meter.process(block.data(), frames);
        if (progress && total > 0)
            progress(double(at + frames - first) / double(total));
    }
    result.valid = true;
    result.samplePeakDb = toDb(peak);
    result.truePeakDb = meter.truePeakMax();
    result.loudnessLufs = meter.integrated();
    result.loudnessRange = meter.loudnessRange();
    return result;
}

Measurement measureMix(const TimelineData& timeline, std::shared_ptr<const MediaPathMap> media,
    const std::optional<MixTarget>& channel, int64_t firstFrame, int64_t endFrame,
    const std::atomic<bool>* cancel, const std::function<void(double)>& progress)
{
    Measurement result;
    if (!timeline.fps.isValid() || endFrame <= firstFrame) {
        result.error = QStringLiteral("no audio");
        return result;
    }
    const int64_t first = framesToSamples48k(firstFrame, timeline.fps);
    const int64_t last = framesToSamples48k(endFrame, timeline.fps);
    const int64_t total = last - first;
    AudioMixer mixer;
    mixer.setMediaPaths(std::move(media));
    constexpr int ch = AudioMixer::kChannels;
    std::vector<float> out(size_t(kBlockFrames) * ch), tap(size_t(kBlockFrames) * ch);
    if (channel)
        mixer.setTap(*channel, tap.data());
    dsp::LoudnessMeter meter;
    float peak = 0.0f;
    for (int64_t at = first; at < last; at += kBlockFrames) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            result.error = QStringLiteral("cancelled");
            return result;
        }
        const int frames = int(std::min<int64_t>(kBlockFrames, last - at));
        mixer.mix(timeline, at, frames, out.data());
        const float* heard = channel ? tap.data() : out.data();
        for (int i = 0; i < frames * ch; ++i)
            peak = std::max(peak, std::abs(heard[size_t(i)]));
        meter.process(heard, frames);
        if (progress && total > 0)
            progress(double(at + frames - first) / double(total));
    }
    result.valid = true;
    result.samplePeakDb = toDb(peak);
    result.truePeakDb = meter.truePeakMax();
    result.loudnessLufs = meter.integrated();
    result.loudnessRange = meter.loudnessRange();
    return result;
}

namespace {

constexpr int64_t kCacheFrames = 48000LL * 60 * 15; ///< mixes up to 15 min are kept in memory
constexpr double kLimiterMarginDb = 0.1; ///< the limiter works this far under the ceiling
constexpr double kSolveToleranceLu = 0.1; ///< the correction passes stop inside this
constexpr int kMaxPasses = 8;
constexpr double kMaxExtraGainDb = 40.0;

} // namespace

NormalizePlan planNormalize(const TimelineData& timeline, std::shared_ptr<const MediaPathMap> media,
    int64_t firstFrame, int64_t endFrame, double targetLufs, double ceilingDb,
    const std::atomic<bool>* cancel, const std::function<void(double)>& progress)
{
    NormalizePlan plan;
    if (!timeline.fps.isValid() || endFrame <= firstFrame) {
        plan.error = QStringLiteral("no audio");
        return plan;
    }
    const int64_t first = framesToSamples48k(firstFrame, timeline.fps);
    const int64_t last = framesToSamples48k(endFrame, timeline.fps);
    const int64_t total = last - first;
    constexpr int ch = AudioMixer::kChannels;
    AudioMixer mixer;
    mixer.setMediaPaths(std::move(media));
    std::vector<float> cache;
    const bool cached = total <= kCacheFrames;
    if (cached)
        cache.resize(size_t(total) * ch);
    std::vector<float> block(size_t(kBlockFrames) * ch);
    bool cancelled = false;
    // Calls fn(samples, frames) for every block of the mix: mixed the first time, then read
    // back from the cache (or mixed again for a long range).
    bool mixed = false;
    auto forEachBlock = [&](double passFrom, double passTo, auto&& fn) {
        for (int64_t at = 0; at < total; at += kBlockFrames) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                cancelled = true;
                return;
            }
            const int frames = int(std::min<int64_t>(kBlockFrames, total - at));
            const float* data = nullptr;
            if (cached && mixed) {
                data = cache.data() + size_t(at) * ch;
            } else {
                float* dest = cached ? cache.data() + size_t(at) * ch : block.data();
                mixer.mix(timeline, first + at, frames, dest);
                data = dest;
            }
            fn(data, frames);
            if (progress)
                progress(passFrom + (passTo - passFrom) * double(at + frames) / double(total));
        }
        if (cached)
            mixed = true;
    };
    // Pass 0: the loudness of the mix as it is.
    dsp::LoudnessMeter meter;
    forEachBlock(0.0, 0.3, [&](const float* data, int frames) { meter.process(data, frames); });
    if (cancelled) {
        plan.error = QStringLiteral("cancelled");
        return plan;
    }
    plan.measuredLufs = meter.integrated();
    if (plan.measuredLufs <= -70.0) {
        plan.error = QStringLiteral("silent");
        return plan;
    }
    plan.valid = true;
    const double truePeak = meter.truePeakMax();
    const double g0 = targetLufs - plan.measuredLufs;
    plan.gainDb = g0;
    plan.resultLufs = targetLufs;
    plan.resultTruePeakDb = truePeak + g0;
    plan.limiterCeilingDb = ceilingDb - kLimiterMarginDb;
    if (truePeak + g0 <= plan.limiterCeilingDb)
        return plan; // linear gain reaches the target under the ceiling: nothing to limit
    plan.limited = true;

    // Loudness of the mix after gain @p gainDb and the limiter (re-measured).
    struct Eval
    {
        double lufs = -200.0, truePeak = -200.0, reduction = 0.0;
    };
    int pass = 0;
    auto evaluate = [&](double gainDb) {
        ++pass;
        const double from = 0.3 + 0.7 * double(pass - 1) / kMaxPasses;
        const double to = 0.3 + 0.7 * double(pass) / kMaxPasses;
        const float gain = float(AudioMixer::dbToGain(gainDb));
        dsp::TruePeakLimiter limiter(plan.limiterCeilingDb);
        dsp::LoudnessMeter m;
        std::vector<float> tmp(size_t(kBlockFrames) * ch);
        forEachBlock(from, to, [&](const float* data, int frames) {
            for (int i = 0; i < frames * ch; ++i)
                tmp[size_t(i)] = data[size_t(i)] * gain;
            limiter.process(tmp.data(), tmp.data(), frames);
            m.process(tmp.data(), frames);
        });
        // The limiter delays by latency() frames: the last ones come out of silence.
        std::vector<float> silence(size_t(limiter.latency()) * ch, 0.0f);
        limiter.process(silence.data(), silence.data(), limiter.latency());
        m.process(silence.data(), limiter.latency());
        return Eval {m.integrated(), m.truePeakMax(), limiter.maxReductionDb()};
    };
    double gain = g0;
    double prevGain = 0.0, prevLufs = 0.0;
    bool havePrev = false;
    Eval best;
    double bestGain = g0, bestError = std::numeric_limits<double>::infinity();
    for (int i = 0; i < kMaxPasses; ++i) {
        const Eval e = evaluate(gain);
        if (cancelled) {
            plan.valid = false;
            plan.error = QStringLiteral("cancelled");
            return plan;
        }
        const double error = targetLufs - e.lufs;
        if (std::abs(error) < bestError) {
            bestError = std::abs(error);
            best = e;
            bestGain = gain;
        }
        if (std::abs(error) <= kSolveToleranceLu)
            break;
        // Slope of the limited loudness against the gain: 1 below the limiter, towards 0 when
        // everything is squashed against the ceiling.
        double slope = 1.0;
        if (havePrev && std::abs(gain - prevGain) > 1e-6)
            slope = (e.lufs - prevLufs) / (gain - prevGain);
        if (havePrev && slope < 0.03)
            break; // saturated: more gain does not make it louder
        slope = std::clamp(slope, 0.05, 1.0);
        prevGain = gain;
        prevLufs = e.lufs;
        havePrev = true;
        gain += error / slope;
        if (gain > g0 + kMaxExtraGainDb) {
            gain = g0 + kMaxExtraGainDb;
            if (std::abs(gain - prevGain) < 0.01)
                break;
        }
    }
    plan.passes = pass;
    plan.gainDb = bestGain;
    plan.resultLufs = best.lufs;
    plan.resultTruePeakDb = best.truePeak;
    plan.maxReductionDb = best.reduction;
    plan.reached = std::abs(targetLufs - best.lufs) <= kNormalizeToleranceLu;
    plan.heavyLimiting = best.reduction > kHeavyLimitingDb;
    return plan;
}

std::pair<int64_t, int64_t> mixRange(const TimelineData& timeline)
{
    const int64_t first = timeline.markIn >= 0 ? timeline.markIn : 0;
    const int64_t end = timeline.markOut >= 0 ? timeline.markOut : timeline.duration();
    return {std::max<int64_t>(0, first), std::max<int64_t>(0, end)};
}

double faderChange(double measured, double target)
{
    return measured <= kSilentDb ? std::numeric_limits<double>::quiet_NaN() : target - measured;
}

std::vector<double> gains(const std::vector<double>& levels, double target, Scope scope)
{
    std::vector<double> out(levels.size(), std::numeric_limits<double>::quiet_NaN());
    double loudest = -std::numeric_limits<double>::infinity();
    for (const double l : levels) {
        if (l > kSilentDb)
            loudest = std::max(loudest, l);
    }
    for (size_t i = 0; i < levels.size(); ++i) {
        if (levels[i] <= kSilentDb)
            continue;
        out[i] = scope == Scope::Relative ? target - loudest : target - levels[i];
    }
    return out;
}

} // namespace kk::audionorm
