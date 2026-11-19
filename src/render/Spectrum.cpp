// SPDX-License-Identifier: GPL-3.0-or-later
// Spectrum — FFT spectrum, correlation meter and goniometer points. The meter ideas (log
// frequency display of the loudest bin, one-pole correlation average, rotated vectorscope)
// follow x42 meters (GPL-2.0-or-later, Robin Gareus); the code is our own.
#include "render/Spectrum.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::dsp {

namespace {

constexpr double kRate = 48000.0;
constexpr int kBins = kSpectrumFft / 2 + 1;
constexpr double kCorrelationSeconds = 0.3;

struct Tables
{
    std::vector<float> window; ///< Hann
    std::vector<float> cosT, sinT; ///< twiddles
    std::vector<int> bitrev;
    float norm = 1.0f; ///< 2 / sum(window): a full-scale sine gives amplitude 1

    Tables()
    {
        window.resize(kSpectrumFft);
        double sum = 0.0;
        for (int n = 0; n < kSpectrumFft; ++n) {
            window[size_t(n)] = float(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * n / kSpectrumFft));
            sum += window[size_t(n)];
        }
        norm = float(2.0 / sum);
        cosT.resize(kSpectrumFft / 2);
        sinT.resize(kSpectrumFft / 2);
        for (int k = 0; k < kSpectrumFft / 2; ++k) {
            cosT[size_t(k)] = float(std::cos(2.0 * std::numbers::pi * k / kSpectrumFft));
            sinT[size_t(k)] = float(-std::sin(2.0 * std::numbers::pi * k / kSpectrumFft));
        }
        bitrev.resize(kSpectrumFft);
        int bits = 0;
        while ((1 << bits) < kSpectrumFft)
            ++bits;
        for (int i = 0; i < kSpectrumFft; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                r |= ((i >> b) & 1) << (bits - 1 - b);
            bitrev[size_t(i)] = r;
        }
    }

    void fft(float* re, float* im) const
    {
        for (int i = 0; i < kSpectrumFft; ++i) {
            const int j = bitrev[size_t(i)];
            if (j > i) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (int len = 2; len <= kSpectrumFft; len <<= 1) {
            const int half = len / 2;
            const int step = kSpectrumFft / len;
            for (int i = 0; i < kSpectrumFft; i += len) {
                for (int k = 0; k < half; ++k) {
                    const float wr = cosT[size_t(k * step)], wi = sinT[size_t(k * step)];
                    const int a = i + k, b = i + k + half;
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

const Tables& tables()
{
    static const Tables t;
    return t;
}

float toDb(float amplitude)
{
    return amplitude <= 1e-6f ? float(kSpectrumFloorDb)
                              : std::max(float(kSpectrumFloorDb), 20.0f * std::log10(amplitude));
}

} // namespace

double spectrumBandHz(int band)
{
    const double t = double(std::clamp(band, 0, kSpectrumBands - 1)) / double(kSpectrumBands - 1);
    return kSpectrumMinHz * std::pow(kSpectrumMaxHz / kSpectrumMinHz, t);
}

int spectrumBandOf(double hz)
{
    if (hz <= kSpectrumMinHz)
        return 0;
    const double t = std::log(hz / kSpectrumMinHz) / std::log(kSpectrumMaxHz / kSpectrumMinHz);
    return std::clamp(int(std::lround(t * (kSpectrumBands - 1))), 0, kSpectrumBands - 1);
}

SpectrumAnalyzer::SpectrumAnalyzer()
    : m_ring(kSpectrumFft, 0.0f)
    , m_mag(kBins, 0.0f)
{ }

void SpectrumAnalyzer::reset()
{
    std::fill(m_ring.begin(), m_ring.end(), 0.0f);
    m_pos = 0;
    m_dirty = true;
}

void SpectrumAnalyzer::process(const float* in, int frames)
{
    for (int i = 0; i < frames; ++i) {
        m_ring[size_t(m_pos)] = 0.5f * (in[size_t(i) * 2] + in[size_t(i) * 2 + 1]);
        if (++m_pos == kSpectrumFft)
            m_pos = 0;
    }
    if (frames > 0)
        m_dirty = true;
}

void SpectrumAnalyzer::analyse() const
{
    if (!m_dirty)
        return;
    const Tables& t = tables();
    std::vector<float> re(kSpectrumFft), im(kSpectrumFft, 0.0f);
    for (int n = 0; n < kSpectrumFft; ++n)
        re[size_t(n)] = m_ring[size_t((m_pos + n) % kSpectrumFft)] * t.window[size_t(n)];
    t.fft(re.data(), im.data());
    for (int k = 0; k < kBins; ++k)
        m_mag[size_t(k)] = std::sqrt(re[size_t(k)] * re[size_t(k)] + im[size_t(k)] * im[size_t(k)])
            * t.norm;
    m_dirty = false;
}

float SpectrumAnalyzer::binDb(int bin) const
{
    analyse();
    return toDb(m_mag[size_t(std::clamp(bin, 0, kBins - 1))]);
}

void SpectrumAnalyzer::bandsDb(float* out) const
{
    analyse();
    const double binHz = kRate / kSpectrumFft;
    const double ratio = std::pow(kSpectrumMaxHz / kSpectrumMinHz, 0.5 / double(kSpectrumBands - 1));
    for (int b = 0; b < kSpectrumBands; ++b) {
        const double centre = spectrumBandHz(b);
        const double lo = centre / ratio / binHz, hi = centre * ratio / binHz; // in bins
        const int first = int(std::ceil(lo)), last = int(std::floor(hi));
        float amp = 0.0f;
        if (first <= last) {
            for (int k = std::max(first, 0); k <= std::min(last, kBins - 1); ++k)
                amp = std::max(amp, m_mag[size_t(k)]);
        } else {
            // narrower than a bin: interpolate the neighbouring bins at the centre
            const double pos = centre / binHz;
            const int k0 = std::clamp(int(std::floor(pos)), 0, kBins - 2);
            const float f = float(pos - k0);
            amp = m_mag[size_t(k0)] * (1.0f - f) + m_mag[size_t(k0 + 1)] * f;
        }
        out[b] = toDb(amp);
    }
}

void CorrelationMeter::reset()
{
    m_lr = m_ll = m_rr = 0.0;
}

void CorrelationMeter::process(const float* in, int frames)
{
    const double a = std::exp(-1.0 / (kCorrelationSeconds * kRate));
    for (int i = 0; i < frames; ++i) {
        const double l = in[size_t(i) * 2], r = in[size_t(i) * 2 + 1];
        m_lr = a * m_lr + (1.0 - a) * l * r;
        m_ll = a * m_ll + (1.0 - a) * l * l;
        m_rr = a * m_rr + (1.0 - a) * r * r;
        if (m_ll < 1e-30 && m_rr < 1e-30) // no denormals in silence
            m_lr = m_ll = m_rr = 0.0;
    }
}

double CorrelationMeter::value() const
{
    const double d = std::sqrt(m_ll * m_rr);
    if (d < 1e-12)
        return 0.0;
    return std::clamp(m_lr / d, -1.0, 1.0);
}

void goniometerPoints(const float* in, int frames, float* out)
{
    constexpr float kS = 0.70710678f;
    for (int p = 0; p < kGoniometerPoints; ++p) {
        float x = 0.0f, y = 0.0f;
        if (frames > 0) {
            const int i = std::min(frames - 1, p * frames / kGoniometerPoints);
            const float l = in[size_t(i) * 2], r = in[size_t(i) * 2 + 1];
            x = (l - r) * kS;
            y = (l + r) * kS;
        }
        out[size_t(p) * 2] = x;
        out[size_t(p) * 2 + 1] = y;
    }
}

} // namespace kk::dsp
