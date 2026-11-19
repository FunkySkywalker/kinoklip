// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/ModFx.h"

#include "core/AudioEffectKeys.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::dsp {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// Values this small are flushed to zero in every feedback path: no denormals can build up.
inline float flush(float x)
{
    return std::fabs(x) < 1e-20f ? 0.0f : x;
}

int powerOfTwoAtLeast(int n)
{
    int p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

/// Reads @p buf (power-of-two size) @p delay samples (fractional, >= 1) behind @p write,
/// with linear interpolation.
inline float readInterpolated(const std::vector<float>& buf, int write, double delay)
{
    const int mask = int(buf.size()) - 1;
    const int whole = int(delay);
    const float frac = float(delay - whole);
    const float a = buf[size_t((write - whole) & mask)];
    const float b = buf[size_t((write - whole - 1) & mask)];
    return a + (b - a) * frac;
}

double dbToGain(double db)
{
    return std::pow(10.0, db / 20.0);
}

} // namespace

// ---- Chorus ----

Chorus::Chorus(double sampleRate)
    : m_rate(sampleRate)
{
    m_size = powerOfTwoAtLeast(int(std::ceil(0.1 * m_rate)));
    for (auto& l : m_line)
        l.assign(size_t(m_size), 0.0f);
}

void Chorus::configure(const ChorusSettings& s)
{
    m_s = s;
    m_s.voices = std::clamp(m_s.voices, 1, kMaxVoices);
}

void Chorus::reset()
{
    for (auto& l : m_line)
        std::fill(l.begin(), l.end(), 0.0f);
    m_write = 0;
    m_phase = 0.0;
}

void Chorus::process(float* io, int frames)
{
    const int n = m_s.voices;
    const double mix = m_s.mixPercent / 100.0;
    const double fb = m_s.feedbackPercent / 100.0;
    const double spread = m_s.spreadPercent / 100.0;
    const double depth = m_s.depthPercent / 100.0;
    const double baseDelay = m_s.delayMs * 1e-3 * m_rate;
    const double inc = m_s.rateHz / m_rate;
    const float norm = float(1.0 / std::sqrt(double(n)));
    const int mask = m_size - 1;
    for (int i = 0; i < frames; ++i) {
        const float in[2] = {io[size_t(i) * 2], io[size_t(i) * 2 + 1]};
        float voice[2][kMaxVoices];
        float fbSig[2] = {0.0f, 0.0f};
        for (int c = 0; c < 2; ++c) {
            for (int v = 0; v < n; ++v) {
                const double ph = m_phase + double(v) / n + (c ? 0.25 : 0.0);
                const double lfo = std::sin(kTwoPi * ph);
                const double d
                    = std::clamp(baseDelay * (1.0 + 0.5 * depth * lfo), 1.0, double(m_size - 3));
                voice[c][v] = readInterpolated(m_line[size_t(c)], m_write, d);
                fbSig[c] += voice[c][v] / float(n);
            }
        }
        float wet[2] = {0.0f, 0.0f};
        for (int v = 0; v < n; ++v) {
            const double q = n == 1 ? 0.0 : (2.0 * v / (n - 1) - 1.0);
            const double p = spread * q;
            const float keep = float(1.0 - std::fabs(p));
            const float side = float(std::fabs(p));
            const float sum = voice[0][v] + voice[1][v];
            wet[0] += voice[0][v] * keep + (p < 0 ? sum * side : 0.0f);
            wet[1] += voice[1][v] * keep + (p > 0 ? sum * side : 0.0f);
        }
        for (int c = 0; c < 2; ++c) {
            m_line[size_t(c)][size_t(m_write & mask)] = flush(in[c] + float(fb) * fbSig[c]);
            io[size_t(i) * 2 + size_t(c)] = in[c] * float(1.0 - mix) + wet[c] * norm * float(mix);
        }
        m_write = (m_write + 1) & mask;
        m_phase += inc;
        if (m_phase >= 1.0)
            m_phase -= 1.0;
    }
}

// ---- Flanger ----

Flanger::Flanger(double sampleRate)
    : m_rate(sampleRate)
{
    m_size = powerOfTwoAtLeast(int(std::ceil(0.03 * m_rate)));
    for (auto& l : m_line)
        l.assign(size_t(m_size), 0.0f);
}

void Flanger::configure(const FlangerSettings& s)
{
    m_s = s;
}

void Flanger::reset()
{
    for (auto& l : m_line)
        std::fill(l.begin(), l.end(), 0.0f);
    m_write = 0;
    m_phase = 0.0;
}

void Flanger::process(float* io, int frames)
{
    const double mix = m_s.mixPercent / 100.0;
    const float fb = float(std::clamp(m_s.feedbackPercent, -95.0, 95.0) / 100.0);
    const double depth = m_s.depthPercent / 100.0;
    const double baseDelay = m_s.delayMs * 1e-3 * m_rate;
    const double inc = m_s.rateHz / m_rate;
    const int mask = m_size - 1;
    for (int i = 0; i < frames; ++i) {
        for (int c = 0; c < 2; ++c) {
            float& x = io[size_t(i) * 2 + size_t(c)];
            const double lfo = std::sin(kTwoPi * (m_phase + (c ? 0.25 : 0.0)));
            const double d
                = std::clamp(baseDelay * (1.0 + 0.9 * depth * lfo), 1.0, double(m_size - 3));
            const float delayed = readInterpolated(m_line[size_t(c)], m_write, d);
            m_line[size_t(c)][size_t(m_write & mask)] = flush(x + fb * delayed);
            x = x * float(1.0 - mix) + delayed * float(mix);
        }
        m_write = (m_write + 1) & mask;
        m_phase += inc;
        if (m_phase >= 1.0)
            m_phase -= 1.0;
    }
}

// ---- Distortion ----

Distortion::Distortion(double sampleRate)
    : m_rate(sampleRate)
{
    m_dcCoef = std::exp(-kTwoPi * 20.0 / m_rate);
}

void Distortion::configure(const DistortionSettings& s)
{
    const int os = s.oversampling >= 4 ? 4 : s.oversampling >= 2 ? 2 : 1;
    const bool changed = os != (m_s.oversampling >= 4 ? 4 : m_s.oversampling >= 2 ? 2 : 1)
        || m_over.latency() == 0;
    m_s = s;
    m_s.oversampling = os;
    if (changed && os > 1) {
        m_over.setFactor(os);
        for (auto& d : m_dry)
            d.assign(size_t(m_over.latency()), 0.0f);
        m_dryPos = 0;
        m_over.reset();
    }
    const double fc = 800.0 * std::pow(25.0, std::clamp(s.tonePercent, 0.0, 100.0) / 100.0);
    m_toneCoef = std::exp(-kTwoPi * std::min(fc, 0.45 * m_rate) / m_rate);
}

void Distortion::reset()
{
    m_tone = {};
    m_dcX = {};
    m_dcY = {};
    m_over.reset();
    for (auto& d : m_dry)
        std::fill(d.begin(), d.end(), 0.0f);
    m_dryPos = 0;
}

double Distortion::shape(double in, double g, double step) const
{
    constexpr double kBias = 0.3;
    switch (m_s.mode) {
    case DistortionMode::SoftClip:
        return std::tanh(g * in);
    case DistortionMode::HardClip:
        return std::clamp(g * in, -1.0, 1.0);
    case DistortionMode::BitCrush:
        return std::round(in * step) / step;
    case DistortionMode::Tube:
        return std::tanh(g * in + kBias) - std::tanh(kBias);
    }
    return in;
}

void Distortion::process(float* io, int frames)
{
    const double g = dbToGain(m_s.driveDb);
    const double out = dbToGain(m_s.outputDb);
    const double mix = m_s.mixPercent / 100.0;
    const double bits = std::clamp(16.0 - m_s.driveDb / 40.0 * 13.0, 3.0, 16.0);
    const double step = std::pow(2.0, bits - 1.0);
    const bool toneOn = m_s.tonePercent < 100.0;
    const bool over = m_s.oversampling > 1;
    if (mix <= 0.0) {
        m_idle = true; // the input as it is, no delay; the memory restarts afterwards
        return;
    }
    if (m_idle) {
        reset();
        m_idle = false;
    }
    const int latency = over ? m_over.latency() : 0;
    for (int i = 0; i < frames; ++i) {
        for (size_t c = 0; c < 2; ++c) {
            float& x = io[size_t(i) * 2 + c];
            const double in = x;
            double dry = in; // the dry sound as late as the wet
            double y = 0.0;
            if (over) {
                float hi[4];
                m_over.up(int(c), x, hi);
                for (int p = 0; p < m_s.oversampling; ++p)
                    hi[p] = float(shape(hi[p], g, step));
                y = m_over.down(int(c), hi);
                std::vector<float>& line = m_dry[c];
                dry = line[size_t(m_dryPos)];
                line[size_t(m_dryPos)] = x;
            } else {
                y = shape(in, g, step);
            }
            if (m_s.mode == DistortionMode::Tube) {
                const double hp = y - m_dcX[c] + m_dcCoef * m_dcY[c]; // removes the offset it makes
                m_dcX[c] = y;
                m_dcY[c] = std::fabs(hp) < 1e-20 ? 0.0 : hp;
                y = hp;
            }
            if (toneOn) {
                m_tone[c] = (1.0 - m_toneCoef) * y + m_toneCoef * m_tone[c];
                if (std::fabs(m_tone[c]) < 1e-20)
                    m_tone[c] = 0.0; // no denormals while the sound is silent
                y = m_tone[c];
            }
            x = flush(float(dry * (1.0 - mix) + y * out * mix));
        }
        if (over && ++m_dryPos >= latency)
            m_dryPos = 0;
    }
}

// ---- Oversampler ----

void Oversampler::setFactor(int factor)
{
    m_l = factor >= 4 ? 4 : 2;
    const int half = 32 * m_l; // 64 for x2, 128 for x4
    m_n = 2 * half + 1;
    m_latency = (m_n - 1) / m_l;
    // Blackman-windowed sinc, cut at 0.9 of the original Nyquist, gain L (the zero stuffing
    // loses it) folded in at up().
    const double fc = 0.45 / m_l; // cycles per high-rate sample
    m_coef.assign(size_t(m_n), 0.0f);
    double sum = 0.0;
    for (int j = 0; j < m_n; ++j) {
        const double t = double(j - half);
        const double sinc = t == 0.0 ? 2.0 * fc : std::sin(kTwoPi * fc * t) / (std::numbers::pi * t);
        const double w = 0.42 - 0.5 * std::cos(kTwoPi * j / (m_n - 1))
            + 0.08 * std::cos(2.0 * kTwoPi * j / (m_n - 1));
        m_coef[size_t(j)] = float(sinc * w);
        sum += sinc * w;
    }
    for (float& c : m_coef) // unity gain at DC
        c = float(c / sum);
    reset();
}

void Oversampler::reset()
{
    for (auto& h : m_upHist)
        h.assign(size_t(m_n / m_l + 1), 0.0f);
    for (auto& h : m_downHist)
        h.assign(size_t(m_n), 0.0f);
}

void Oversampler::up(int ch, float in, float* out)
{
    std::vector<float>& h = m_upHist[size_t(ch)];
    std::move_backward(h.begin(), h.end() - 1, h.end());
    h[0] = in;
    const int taps = int(h.size());
    for (int p = 0; p < m_l; ++p) {
        double acc = 0.0;
        for (int k = 0; k < taps; ++k) {
            const int j = k * m_l + p;
            if (j < m_n)
                acc += double(h[size_t(k)]) * double(m_coef[size_t(j)]);
        }
        out[p] = float(acc * m_l);
    }
}

float Oversampler::down(int ch, const float* in)
{
    std::vector<float>& h = m_downHist[size_t(ch)];
    // newest first: the L new samples go in front, the last of them first
    std::move_backward(h.begin(), h.end() - m_l, h.end());
    for (int p = 0; p < m_l; ++p)
        h[size_t(m_l - 1 - p)] = in[p];
    // the output sits at the first of the L new samples (index L - 1 from the newest)
    double acc = 0.0;
    for (int j = 0; j < m_n; ++j) {
        const size_t idx = size_t(j + m_l - 1);
        if (idx < h.size())
            acc += double(h[idx]) * double(m_coef[size_t(j)]);
    }
    return float(acc);
}

// ---- Stereo Width ----

StereoWidth::StereoWidth(double sampleRate)
    : m_rate(sampleRate)
{ }

void StereoWidth::configure(const StereoWidthSettings& s)
{
    if (s.monoBelowHz != m_s.monoBelowHz && s.monoBelowHz > 0.0)
        m_hp = designBand(EqBandType::HighPass, s.monoBelowHz, 0.0, 0.7071, m_rate);
    m_s = s;
}

void StereoWidth::reset()
{
    m_state.reset();
}

void StereoWidth::process(float* io, int frames)
{
    const double w = m_s.widthPercent / 100.0;
    const double out = dbToGain(m_s.outputDb);
    const bool filter = m_s.monoBelowHz > 0.0;
    const bool plain = !filter && w == 1.0;
    for (int i = 0; i < frames; ++i) {
        float& l = io[size_t(i) * 2];
        float& r = io[size_t(i) * 2 + 1];
        if (plain) {
            l = float(l * out);
            r = float(r * out);
            continue;
        }
        const double mid = 0.5 * (double(l) + r);
        double side = 0.5 * (double(l) - r);
        if (filter)
            side = m_state.process(m_hp, side);
        side *= w;
        l = float((mid + side) * out);
        r = float((mid - side) * out);
    }
}

// ---- Reverb ----

ReverbSettings reverbPreset(int index)
{
    ReverbSettings s;
    const int i = std::clamp(index, 0, kReverbPresetCount - 1);
    const afxchoice::ReverbPresetValues& v = afxchoice::kReverbPresetValues[i];
    s.preDelayMs = v.preDelayMs, s.decaySeconds = v.decayS, s.roomSizePercent = v.roomSize;
    s.dampingPercent = v.damping, s.earlyPercent = v.early, s.widthPercent = v.width;
    return s;
}

namespace {

constexpr int kLineBase[8] = {1087, 1283, 1481, 1699, 1913, 2111, 2347, 2593}; // at 48 kHz
constexpr int kDiffuserBase[2] = {142, 107};
constexpr double kTapMs[8] = {6.3, 9.7, 13.9, 18.1, 23.3, 29.9, 37.7, 46.1};
constexpr float kInject[8] = {1, -1, 1, 1, -1, 1, -1, -1};
constexpr float kOutL[8] = {1, 1, 1, 1, -1, -1, -1, -1};
constexpr float kOutR[8] = {1, -1, 1, -1, 1, -1, 1, -1};
constexpr double kMaxPreDelayMs = 250.0;

} // namespace

Reverb::Reverb(double sampleRate)
    : m_rate(sampleRate)
{
    const double scale = m_rate / 48000.0;
    m_preLen = 0;
    m_pre.assign(size_t(powerOfTwoAtLeast(int(kMaxPreDelayMs * 1e-3 * m_rate) + 2)), 0.0f);
    m_er.assign(size_t(powerOfTwoAtLeast(int(0.15 * m_rate))), 0.0f);
    for (int i = 0; i < kLines; ++i) {
        m_lines[size_t(i)].buf.assign(size_t(std::ceil(kLineBase[i] * 1.6 * scale)) + 2, 0.0f);
        m_lines[size_t(i)].len = int(kLineBase[i] * scale);
    }
    for (int i = 0; i < 2; ++i)
        m_diff[size_t(i)].buf.assign(size_t(std::lround(kDiffuserBase[i] * scale)), 0.0f);
}

void Reverb::configure(const ReverbSettings& s)
{
    m_s = s;
    const double scale = m_rate / 48000.0;
    const double room = 0.4 + 1.2 * std::clamp(s.roomSizePercent, 0.0, 100.0) / 100.0;
    const double decay = std::max(s.decaySeconds, 0.05);
    for (int i = 0; i < kLines; ++i) {
        Line& l = m_lines[size_t(i)];
        l.len = std::clamp(int(std::lround(kLineBase[i] * room * scale)), 1, int(l.buf.size()));
        if (l.pos >= l.len)
            l.pos = 0;
        l.gain = float(std::pow(10.0, -3.0 * l.len / (decay * m_rate)));
    }
    for (int i = 0; i < kTaps; ++i) {
        m_tap[size_t(i)] = std::clamp(
            int(std::lround(kTapMs[i] * 1e-3 * room * m_rate)), 1, int(m_er.size()) - 2);
        m_tapGain[size_t(i)] = float(0.7 * std::pow(0.82, i)) * ((i % 3 == 2) ? -1.0f : 1.0f);
    }
    m_preLen = std::clamp(
        int(std::lround(std::clamp(s.preDelayMs, 0.0, kMaxPreDelayMs) * 1e-3 * m_rate)), 0,
        int(m_pre.size()) - 2);
    // Damping: the high cut of each line, 20 kHz (0 %) down to 1.5 kHz (100 %).
    const double d = std::clamp(s.dampingPercent, 0.0, 100.0) / 100.0;
    m_damp = d <= 0.0
        ? 0.0f
        : float(std::exp(-kTwoPi * 20000.0 * std::pow(1500.0 / 20000.0, d) / m_rate));
    m_erGain = float(std::clamp(s.earlyPercent, 0.0, 100.0) / 100.0 * 0.6);
    m_mix = float(std::clamp(s.mixPercent, 0.0, 100.0) / 100.0);
    m_side = float(std::clamp(s.widthPercent, 0.0, 200.0) / 100.0);
}

void Reverb::reset()
{
    std::fill(m_pre.begin(), m_pre.end(), 0.0f);
    std::fill(m_er.begin(), m_er.end(), 0.0f);
    m_preWrite = m_erWrite = 0;
    for (Line& l : m_lines) {
        std::fill(l.buf.begin(), l.buf.end(), 0.0f);
        l.pos = 0;
        l.lp = 0.0f;
    }
    for (Allpass& a : m_diff) {
        std::fill(a.buf.begin(), a.buf.end(), 0.0f);
        a.pos = 0;
    }
}

void Reverb::process(float* io, int frames)
{
    constexpr float kInjectGain = 0.35f;
    constexpr float kOutGain = 0.25f;
    constexpr float kAllpassG = 0.6f;
    const int preMask = int(m_pre.size()) - 1;
    const int erMask = int(m_er.size()) - 1;
    for (int i = 0; i < frames; ++i) {
        float& l = io[size_t(i) * 2];
        float& r = io[size_t(i) * 2 + 1];
        const float dryL = l, dryR = r;
        // pre-delay
        m_pre[size_t(m_preWrite)] = 0.5f * (dryL + dryR);
        const float delayed = m_pre[size_t((m_preWrite - m_preLen) & preMask)];
        m_preWrite = (m_preWrite + 1) & preMask;
        // early reflections
        m_er[size_t(m_erWrite)] = delayed;
        float erL = 0.0f, erR = 0.0f;
        for (int t = 0; t < kTaps; ++t) {
            const float v
                = m_er[size_t((m_erWrite - m_tap[size_t(t)]) & erMask)] * m_tapGain[size_t(t)];
            (t % 2 ? erR : erL) += v;
            (t % 2 ? erL : erR) += 0.3f * v;
        }
        m_erWrite = (m_erWrite + 1) & erMask;
        // diffusion
        float x = delayed;
        for (Allpass& a : m_diff) {
            const float buffered = a.buf[size_t(a.pos)];
            const float w = flush(x + kAllpassG * buffered);
            x = -kAllpassG * w + buffered;
            a.buf[size_t(a.pos)] = w;
            if (++a.pos == int(a.buf.size()))
                a.pos = 0;
        }
        // feedback delay network
        float v[kLines];
        float sum = 0.0f, fdnL = 0.0f, fdnR = 0.0f;
        for (int k = 0; k < kLines; ++k) {
            Line& line = m_lines[size_t(k)];
            const float out = line.buf[size_t(line.pos)];
            line.lp = flush((1.0f - m_damp) * out + m_damp * line.lp);
            v[k] = line.lp;
            sum += v[k];
            fdnL += kOutL[k] * v[k];
            fdnR += kOutR[k] * v[k];
        }
        const float mean = sum * (2.0f / kLines);
        for (int k = 0; k < kLines; ++k) {
            Line& line = m_lines[size_t(k)];
            line.buf[size_t(line.pos)]
                = flush((v[k] - mean) * line.gain + kInjectGain * kInject[k] * x);
            if (++line.pos >= line.len)
                line.pos = 0;
        }
        float wetL = fdnL * kOutGain + erL * m_erGain;
        float wetR = fdnR * kOutGain + erR * m_erGain;
        const float mid = 0.5f * (wetL + wetR);
        const float side = 0.5f * (wetL - wetR) * m_side;
        wetL = mid + side;
        wetR = mid - side;
        l = dryL * (1.0f - m_mix) + wetL * m_mix;
        r = dryR * (1.0f - m_mix) + wetR * m_mix;
    }
}

} // namespace kk::dsp
