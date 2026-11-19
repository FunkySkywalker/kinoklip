// SPDX-License-Identifier: GPL-3.0-or-later
// NoiseReduction — see NoiseReduction.h. The spectral gate is a classic STFT noise reducer
// (windowed frames, a noise power per bin, a Wiener-like gain per bin, smoothed over frequency
// and time, overlap-add), written for Kinoklip; RNNoise is the unchanged BSD-3-Clause library in
// third_party/rnnoise.
#include "render/NoiseReduction.h"

#include "core/ClipAttributes.h"
#include "media/AudioDecoder.h"

extern "C" {
#include "rnnoise.h"
}

#include <QByteArray>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <numbers>

namespace kk::dsp::noise {

namespace {

constexpr double kRate = 48000.0;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr int kMaxHop = kGateHop; // 512 >= 480
constexpr int kMaxLearnSeconds = 10;

// ---- FFT: iterative radix-2, size kFftSize, split re / im arrays ----

struct Fft
{
    std::array<float, kFftSize / 2> cosT {}, sinT {};
    std::array<uint16_t, kFftSize> rev {};
    std::array<float, kFftSize> window {}; ///< sqrt of the periodic Hann window

    Fft()
    {
        for (int i = 0; i < kFftSize / 2; ++i) {
            cosT[size_t(i)] = float(std::cos(2.0 * std::numbers::pi * i / kFftSize));
            sinT[size_t(i)] = float(-std::sin(2.0 * std::numbers::pi * i / kFftSize));
        }
        int bits = 0;
        while ((1 << bits) < kFftSize)
            ++bits;
        for (int i = 0; i < kFftSize; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                r |= ((i >> b) & 1) << (bits - 1 - b);
            rev[size_t(i)] = uint16_t(r);
            window[size_t(i)]
                = float(std::sqrt(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kFftSize)));
        }
    }

    /// In place; @p inverse flips the sign of the exponent (unscaled).
    void run(float* re, float* im, bool inverse) const
    {
        for (int i = 0; i < kFftSize; ++i) {
            const int j = rev[size_t(i)];
            if (j > i) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (int len = 2; len <= kFftSize; len <<= 1) {
            const int half = len / 2;
            const int step = kFftSize / len;
            for (int base = 0; base < kFftSize; base += len) {
                for (int k = 0; k < half; ++k) {
                    const float wr = cosT[size_t(k * step)];
                    const float wi = inverse ? -sinT[size_t(k * step)] : sinT[size_t(k * step)];
                    const int a = base + k, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr;
                    im[b] = im[a] - ti;
                    re[a] += tr;
                    im[a] += ti;
                }
            }
        }
    }
};

const Fft& fft()
{
    static const Fft instance;
    return instance;
}

float dbToLin(double db)
{
    return float(std::pow(10.0, db / 20.0));
}

// ---- The spectral gate of one channel ----

struct GateChannel
{
    std::array<float, kMaxHop> prevIn {}, tail {};
    std::array<float, kBins> noise {}, ps {}, pl {}, gain {};
    std::array<float, kFftSize> re {}, im {};
    int frames = 0;

    void reset() { *this = GateChannel(); }

    /// The first noise estimate of bin @p k: the lower quarter of the slow power over 17 bins,
    /// so a steady tone or a voice harmonic that is there from the start is not taken for noise.
    float startNoise(int k) const
    {
        std::array<float, 17> w;
        for (int i = 0; i < 17; ++i)
            w[size_t(i)] = pl[size_t(std::clamp(k - 8 + i, 0, kBins - 1))];
        std::nth_element(w.begin(), w.begin() + 4, w.end());
        return w[4];
    }

    /// One hop in, one hop out (the output is the previous hop's sound, see the overlap-add).
    void process(const Settings& s, const std::array<float, kBins>* profile, const float* in,
        float* out)
    {
        const Fft& f = fft();
        for (int n = 0; n < kGateHop; ++n) {
            re[size_t(n)] = prevIn[size_t(n)] * f.window[size_t(n)];
            re[size_t(n + kGateHop)] = in[n] * f.window[size_t(n + kGateHop)];
        }
        std::fill(im.begin(), im.end(), 0.0f);
        f.run(re.data(), im.data(), false);

        const float alpha = float(std::pow(10.0, (3.0 + 0.09 * s.sensitivity) / 10.0));
        const float floorGain = dbToLin(-s.reductionDb);
        const float open = float(1.0 - std::exp(-(kGateHop / kRate * 1000.0) / std::max(s.attackMs, 1.0)));
        constexpr float kClose = 0.6f;
        constexpr float kRise = 1.003f; // the adaptive estimate climbs about 1.4 dB / s

        for (int k = 0; k < kBins; ++k) {
            const float p = re[size_t(k)] * re[size_t(k)] + im[size_t(k)] * im[size_t(k)];
            ps[size_t(k)] = frames == 0 ? p : 0.5f * ps[size_t(k)] + 0.5f * p;
            // The noise estimate follows a slower average (about 100 ms): the minimum of a
            // jumpy power would sit far below the real noise level.
            pl[size_t(k)] = frames == 0 ? p : 0.9f * pl[size_t(k)] + 0.1f * p;
        }
        std::array<float, kBins> raw {};
        for (int k = 0; k < kBins; ++k) {
            float n;
            if (profile) {
                n = (*profile)[size_t(k)];
            } else {
                if (frames < 2)
                    noise[size_t(k)] = startNoise(k);
                else
                    noise[size_t(k)] = std::min(pl[size_t(k)], noise[size_t(k)] * kRise);
                n = noise[size_t(k)];
            }
            // Three bins side by side decide together: a component spreads over a few bins
            // (the window's main lobe), and the sum of three is far steadier than one bin.
            const float local = (ps[size_t(std::max(k - 1, 0))] + ps[size_t(k)]
                                    + ps[size_t(std::min(k + 1, kBins - 1))])
                * (1.0f / 3.0f);
            const float r = alpha * n / (local + 1e-20f);
            raw[size_t(k)] = r >= 1.0f ? floorGain : std::max(floorGain, std::sqrt(1.0f - r * r));
        }
        for (int k = 0; k < kBins; ++k) {
            const float target = raw[size_t(k)];
            float& g = gain[size_t(k)];
            g = frames == 0 ? target : g + (target - g) * (target > g ? open : kClose);
        }
        for (int k = 0; k < kBins; ++k) {
            re[size_t(k)] *= gain[size_t(k)];
            im[size_t(k)] *= gain[size_t(k)];
            if (k > 0 && k < kBins - 1) {
                re[size_t(kFftSize - k)] *= gain[size_t(k)];
                im[size_t(kFftSize - k)] *= gain[size_t(k)];
            }
        }
        f.run(re.data(), im.data(), true);
        constexpr float kScale = 1.0f / float(kFftSize);
        for (int n = 0; n < kGateHop; ++n) {
            out[n] = tail[size_t(n)] + re[size_t(n)] * f.window[size_t(n)] * kScale;
            tail[size_t(n)]
                = re[size_t(n + kGateHop)] * f.window[size_t(n + kGateHop)] * kScale;
            prevIn[size_t(n)] = in[n];
        }
        ++frames;
    }
};

// ---- RNNoise of one channel ----

void warmUpRnnoise()
{
    // RNNoise fills static tables the first time a state is made; do it once, on one thread.
    static std::once_flag once;
    std::call_once(once, [] { rnnoise_destroy(rnnoise_create(nullptr)); });
}

struct RnnChannel
{
    DenoiseState* state = nullptr;

    RnnChannel()
    {
        warmUpRnnoise();
        state = rnnoise_create(nullptr);
    }
    ~RnnChannel() { rnnoise_destroy(state); }
    RnnChannel(const RnnChannel&) = delete;
    RnnChannel& operator=(const RnnChannel&) = delete;

    void reset() { rnnoise_init(state, nullptr); }

    /// One 480-frame hop in, one out (RNNoise's output is the previous hop's sound). Returns
    /// the voice probability of the frame.
    float process(const float* in, float* out)
    {
        std::array<float, kRnnoiseHop> x, y;
        for (int i = 0; i < kRnnoiseHop; ++i)
            x[size_t(i)] = in[i] * 32768.0f; // RNNoise works on 16-bit sized values
        const float vad = rnnoise_process_frame(state, y.data(), x.data());
        for (int i = 0; i < kRnnoiseHop; ++i)
            out[i] = y[size_t(i)] * (1.0f / 32768.0f);
        return vad;
    }
};

} // namespace

// ---- Profile ----

QString encodeProfile(const Profile& profile)
{
    if (!profile.valid())
        return {};
    QByteArray bytes;
    bytes.reserve(kBins);
    for (const float db : profile.powerDb)
        bytes.append(char(uint8_t(std::clamp(std::lround((double(db) + 100.0) * 1.5), 0L, 255L))));
    return QStringLiteral("v1:") + QString::fromLatin1(bytes.toBase64());
}

Profile decodeProfile(const QString& text)
{
    Profile p;
    if (!text.startsWith(QLatin1String("v1:")))
        return p;
    const QByteArray bytes = QByteArray::fromBase64(text.mid(3).toLatin1());
    if (bytes.size() != kBins)
        return p;
    p.powerDb.resize(kBins);
    for (int k = 0; k < kBins; ++k)
        p.powerDb[size_t(k)] = float(double(uint8_t(bytes[k])) / 1.5 - 100.0);
    return p;
}

Profile learnProfile(const float* interleavedStereo, int frames)
{
    Profile result;
    constexpr int kMinWindows = 4;
    if (!interleavedStereo || frames < kFftSize + (kMinWindows - 1) * kGateHop)
        return result;
    const Fft& f = fft();
    std::array<float, kFftSize> re, im;
    std::array<double, kBins> sum {};
    int windows = 0;
    for (int start = 0; start + kFftSize <= frames; start += kGateHop, ++windows) {
        for (int n = 0; n < kFftSize; ++n) {
            const size_t i = size_t(start + n) * 2;
            re[size_t(n)] = 0.5f * (interleavedStereo[i] + interleavedStereo[i + 1])
                * f.window[size_t(n)];
        }
        std::fill(im.begin(), im.end(), 0.0f);
        f.run(re.data(), im.data(), false);
        for (int k = 0; k < kBins; ++k)
            sum[size_t(k)] += double(re[size_t(k)]) * re[size_t(k)] + double(im[size_t(k)]) * im[size_t(k)];
    }
    result.powerDb.resize(kBins);
    for (int k = 0; k < kBins; ++k)
        result.powerDb[size_t(k)] = float(10.0 * std::log10(sum[size_t(k)] / windows + 1e-20));
    return result;
}

Profile learnProfileFromFile(const QString& path, int64_t startUs, int64_t endUs,
    const ClipAttributes& attributes, int audioStream, QString* error)
{
    media::AudioDecoder decoder;
    QString why;
    if (!decoder.open(path, &why, audioStream)) {
        if (error)
            *error = why;
        return {};
    }
    decoder.setChannelMapping(attributes);
    constexpr int rate = media::AudioDecoder::kSampleRate;
    const int64_t first = std::max<int64_t>(0, startUs) * rate / 1000000;
    const int64_t last = std::min(endUs * rate / 1000000, first + int64_t(kMaxLearnSeconds) * rate);
    if (last <= first) {
        if (error)
            *error = QStringLiteral("nothing to learn from");
        return {};
    }
    std::vector<float> buffer(size_t(last - first) * media::AudioDecoder::kChannels);
    decoder.read(first, buffer.data(), int(last - first));
    Profile p = learnProfile(buffer.data(), int(last - first));
    if (!p.valid() && error)
        *error = QStringLiteral("the selection is too short");
    return p;
}

bool rnnoiseAvailable()
{
    return true;
}

// ---- Reducer ----

struct Reducer::Impl
{
    Settings s;
    Profile profile;
    std::array<float, kBins> profilePower {};
    bool useProfile = false;
    bool rnnEngine = false;
    bool fresh = true; ///< memory is clean (no processing yet)
    bool bypassed = true;

    std::array<GateChannel, 2> gate;
    std::unique_ptr<std::array<RnnChannel, 2>> rnn;

    int hop() const { return rnnEngine ? kRnnoiseHop : kGateHop; }

    std::array<std::array<float, kMaxHop>, 2> inHop {}, playHop {}, wetHop {};
    std::array<std::array<float, 2 * kMaxHop>, 2> dry {};
    int pos = 0, dryPos = 0;

    // Expander / voice gate: smoothed dB gain; the value computed for the hop that just came
    // in is applied one hop later, to the sound of that hop.
    double curDb = 0.0;
    float gainBegin = 1.0f, gainEnd = 1.0f;

    float mixNow = 1.0f, gainNow = 1.0f;
    bool rampFresh = true;

    void clear()
    {
        for (GateChannel& g : gate)
            g.reset();
        if (rnn) {
            for (RnnChannel& r : *rnn)
                r.reset();
        }
        for (auto& a : inHop)
            a.fill(0.0f);
        for (auto& a : playHop)
            a.fill(0.0f);
        for (auto& a : dry)
            a.fill(0.0f);
        pos = dryPos = 0;
        curDb = 0.0;
        gainBegin = gainEnd = 1.0f;
        rampFresh = true;
        fresh = true;
    }

    void completeHop()
    {
        const int n = hop();
        float vad = 1.0f;
        if (rnnEngine) {
            if (!rnn)
                rnn = std::make_unique<std::array<RnnChannel, 2>>();
            vad = 0.0f;
            for (int c = 0; c < 2; ++c)
                vad = std::max(vad, (*rnn)[size_t(c)].process(inHop[size_t(c)].data(),
                                        wetHop[size_t(c)].data()));
        } else {
            for (int c = 0; c < 2; ++c)
                gate[size_t(c)].process(s, useProfile ? &profilePower : nullptr,
                    inHop[size_t(c)].data(), wetHop[size_t(c)].data());
        }
        // The expander gain computed for the previous hop goes onto this output (the sound of
        // that hop), ramped across it.
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < n; ++i) {
                const float t = float(i) / float(n);
                playHop[size_t(c)][size_t(i)]
                    = wetHop[size_t(c)][size_t(i)] * (gainBegin + (gainEnd - gainBegin) * t);
            }
        }
        // The target for the hop that just arrived: below the Threshold the sound gets
        // (Threshold - level) * (Ratio - 1) dB of reduction, never more than Reduction.
        double sumSq = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                sumSq += double(inHop[size_t(c)][size_t(i)]) * inHop[size_t(c)][size_t(i)];
        const double levelDb = 10.0 * std::log10(sumSq / (2.0 * n) + 1e-18);
        double targetDb = 0.0;
        if (levelDb < s.thresholdDb)
            targetDb -= (s.thresholdDb - levelDb) * (std::max(s.ratio, 1.0) - 1.0);
        if (rnnEngine) {
            const double thr = 0.2 * s.sensitivity / 100.0; // voice probability below this = noise
            if (thr > 0.001)
                targetDb += -s.reductionDb * (1.0 - std::clamp(double(vad) / thr, 0.0, 1.0));
        }
        targetDb = std::max(targetDb, -s.reductionDb);
        const double hopMs = n / kRate * 1000.0;
        const double open = 1.0 - std::exp(-hopMs / std::max(s.attackMs, 1.0));
        constexpr double kClose = 0.6;
        curDb += (targetDb - curDb) * (targetDb > curDb ? open : kClose);
        gainBegin = gainEnd;
        gainEnd = float(std::pow(10.0, curDb / 20.0));
        fresh = false;
    }
};

Reducer::Reducer()
    : m_impl(std::make_unique<Impl>())
{ }

Reducer::~Reducer() = default;

void Reducer::setProfile(const Profile& profile)
{
    if (profile == m_impl->profile)
        return;
    m_impl->profile = profile;
    m_impl->useProfile = profile.valid();
    if (profile.valid()) {
        for (int k = 0; k < kBins; ++k)
            m_impl->profilePower[size_t(k)] = std::pow(10.0f, profile.powerDb[size_t(k)] / 10.0f);
    }
}

void Reducer::configure(const Settings& settings)
{
    Impl& d = *m_impl;
    d.s = settings;
    d.s.reductionDb = std::clamp(d.s.reductionDb, 0.0, 60.0);
    const bool wantRnn = !settings.learnMode && settings.rnnoise && rnnoiseAvailable();
    if (wantRnn != d.rnnEngine) {
        d.rnnEngine = wantRnn;
        d.clear();
    }
}

void Reducer::reset()
{
    m_impl->clear();
}

int Reducer::latencyFrames() const
{
    return 2 * m_impl->hop();
}

void Reducer::process(float* io, int frames)
{
    Impl& d = *m_impl;
    const float mixTarget = float(std::clamp(d.s.mix, 0.0, 100.0) / 100.0);
    const float gainTarget = dbToLin(d.s.outputDb);
    if (mixTarget <= 0.0f && std::abs(d.s.outputDb) < 1e-9) {
        d.bypassed = true; // the input as it is, no delay; the memory restarts afterwards
        return;
    }
    if (d.bypassed) {
        d.clear();
        d.bypassed = false;
    }
    if (d.rampFresh) {
        d.mixNow = mixTarget;
        d.gainNow = gainTarget;
        d.rampFresh = false;
    }
    const int hop = d.hop();
    const int dryLen = 2 * hop;
    // Learn mode with a learned profile; the capped blend keeps RNNoise's result within
    // Reduction dB of the original.
    const float fl = d.rnnEngine ? dbToLin(-d.s.reductionDb) : 0.0f;
    const float mixStep = (mixTarget - d.mixNow) / float(std::max(frames, 1));
    const float gainStep = (gainTarget - d.gainNow) / float(std::max(frames, 1));
    for (int i = 0; i < frames; ++i) {
        for (int c = 0; c < 2; ++c) {
            float& x = io[size_t(i) * 2 + size_t(c)];
            float& slot = d.dry[size_t(c)][size_t(d.dryPos)];
            const float dry = slot; // the input of 2 hops ago
            slot = x;
            const float y = d.playHop[size_t(c)][size_t(d.pos)];
            d.inHop[size_t(c)][size_t(d.pos)] = x;
            const float wet = fl > 0.0f ? y * (1.0f - fl) + dry * fl : y;
            x = (dry + (wet - dry) * d.mixNow) * d.gainNow;
        }
        d.mixNow += mixStep;
        d.gainNow += gainStep;
        if (++d.dryPos == dryLen)
            d.dryPos = 0;
        if (++d.pos == hop) {
            d.pos = 0;
            d.completeHop();
        }
    }
    d.mixNow = mixTarget;
    d.gainNow = gainTarget;
}

} // namespace kk::dsp::noise
