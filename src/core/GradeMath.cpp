// SPDX-License-Identifier: GPL-3.0-or-later
// GradeMath — primaries, curve splines / tables, HSL, the qualifier key and the window matte on
// the CPU. Keep every formula in step with the GLSL in render/GlGrade.cpp (test_graderender
// compares them).
#include "core/GradeMath.h"

#include "core/Mask.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <tuple>
#include <vector>

namespace kk::grademath {

namespace {

/// Points closer than this in x are treated as one (the spline needs increasing x).
constexpr double kMinPointGap = 1e-6;
/// Distance of the neutral neighbours addCurvePoint() puts around the first point of a hue
/// or lum-vs-sat curve.
constexpr double kNeighbourGap = 1.0 / 12.0;

/// Monotone cubic Hermite interpolation through (xs, ys) at x (xs strictly increasing, at least
/// two points; x outside the range holds the end values).
double monotoneCubic(const std::vector<double>& xs, const std::vector<double>& ys, double x)
{
    const size_t n = xs.size();
    if (x <= xs.front())
        return ys.front();
    if (x >= xs.back())
        return ys.back();
    std::vector<double> d(n - 1);
    for (size_t k = 0; k + 1 < n; ++k)
        d[k] = (ys[k + 1] - ys[k]) / (xs[k + 1] - xs[k]);
    std::vector<double> m(n);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (size_t k = 1; k + 1 < n; ++k)
        m[k] = d[k - 1] * d[k] <= 0.0 ? 0.0 : (d[k - 1] + d[k]) * 0.5;
    // Fritsch-Carlson: limit the tangents so every segment stays monotone (no overshoot).
    for (size_t k = 0; k + 1 < n; ++k) {
        if (d[k] == 0.0) {
            m[k] = 0.0;
            m[k + 1] = 0.0;
            continue;
        }
        const double a = m[k] / d[k];
        const double b = m[k + 1] / d[k];
        const double s = a * a + b * b;
        if (s > 9.0) {
            const double t = 3.0 / std::sqrt(s);
            m[k] = t * a * d[k];
            m[k + 1] = t * b * d[k];
        }
    }
    const size_t k = size_t(std::upper_bound(xs.begin(), xs.end(), x) - xs.begin()) - 1;
    const double h = xs[k + 1] - xs[k];
    const double t = (x - xs[k]) / h;
    const double t2 = t * t;
    const double t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * ys[k] + (t3 - 2 * t2 + t) * h * m[k]
        + (-2 * t3 + 3 * t2) * ys[k + 1] + (t3 - t2) * h * m[k + 1];
}

} // namespace

PrimaryValues PrimaryValues::at(const GradeNode& node, int64_t localFrame)
{
    using gradekeys::Wheel;
    PrimaryValues p;
    for (int ch = 0; ch < 4; ++ch) {
        p.lift[ch] = float(node.value(gradekeys::wheelKey(Wheel::Lift, ch), localFrame));
        p.gamma[ch] = float(node.value(gradekeys::wheelKey(Wheel::Gamma, ch), localFrame));
        p.gain[ch] = float(node.value(gradekeys::wheelKey(Wheel::Gain, ch), localFrame));
        p.offset[ch] = float(node.value(gradekeys::wheelKey(Wheel::Offset, ch), localFrame));
    }
    p.contrast = float(node.value(gradekeys::Contrast, localFrame));
    p.pivot = float(node.value(gradekeys::Pivot, localFrame));
    p.saturation = float(node.value(gradekeys::Saturation, localFrame));
    p.hueDegrees = float(node.value(gradekeys::Hue, localFrame));
    p.temperature = float(node.value(gradekeys::Temperature, localFrame));
    p.tint = float(node.value(gradekeys::Tint, localFrame));
    p.lumMix = float(std::clamp(node.value(gradekeys::LumMix, localFrame), 0.0, 1.0));
    for (int ch = 0; ch < 4; ++ch) {
        p.logShadow[ch] = float(node.value(gradekeys::wheelKey(Wheel::LogShadow, ch), localFrame));
        p.logMid[ch] = float(node.value(gradekeys::wheelKey(Wheel::LogMidtone, ch), localFrame));
        p.logHigh[ch] = float(node.value(gradekeys::wheelKey(Wheel::LogHighlight, ch), localFrame));
        p.logOffset[ch] = float(node.value(gradekeys::wheelKey(Wheel::LogOffset, ch), localFrame));
    }
    p.logLowRange = float(std::clamp(node.value(gradekeys::LogLowRange, localFrame), 0.0, 1.0));
    p.logHighRange = float(std::clamp(node.value(gradekeys::LogHighRange, localFrame), 0.0, 1.0));
    p.colorBoost
        = float(std::clamp(node.value(gradekeys::ColorBoost, localFrame) / 100.0, -1.0, 1.0));
    p.shadows = float(std::clamp(node.value(gradekeys::Shadows, localFrame) / 100.0, -1.0, 1.0));
    p.highlights
        = float(std::clamp(node.value(gradekeys::Highlights, localFrame) / 100.0, -1.0, 1.0));
    if (p.hueDegrees != 0.0f) {
        // Rotation about the grey axis (1, 1, 1) (Rodrigues; keeps (r + g + b) / 3).
        const float a = p.hueDegrees * float(std::numbers::pi) / 180.0f;
        const float cosA = std::cos(a);
        const float sinA = std::sin(a);
        const float k = (1.0f - cosA) / 3.0f;
        const float s = sinA / std::sqrt(3.0f);
        const float m[9] = {cosA + k, k - s, k + s, k + s, cosA + k, k - s, k - s, k + s, cosA + k};
        std::copy(std::begin(m), std::end(m), std::begin(p.hueMatrix));
    }
    return p;
}

bool PrimaryValues::isIdentity() const
{
    for (int ch = 0; ch < 4; ++ch) {
        if (lift[ch] != 0.0f || gamma[ch] != 0.0f || gain[ch] != 1.0f || offset[ch] != 0.0f)
            return false;
    }
    return contrast == 1.0f && saturation == 1.0f && hueDegrees == 0.0f && temperature == 0.0f
        && tint == 0.0f && !logActive() && colorBoost == 0.0f && shadows == 0.0f
        && highlights == 0.0f;
}

bool PrimaryValues::logActive() const
{
    for (int ch = 0; ch < 4; ++ch) {
        if (logShadow[ch] != 0.0f || logMid[ch] != 0.0f || logHigh[ch] != 0.0f
            || logOffset[ch] != 0.0f)
            return true;
    }
    return false;
}

void logZoneWeights(float y, float low, float high, float w[3])
{
    const float shadow = 1.0f - smoothStep(low - kLogRamp, low + kLogRamp, y);
    const float highlight = smoothStep(high - kLogRamp, high + kLogRamp, y);
    w[0] = shadow;
    w[2] = highlight;
    w[1] = std::max(1.0f - shadow - highlight, 0.0f);
}

MixerValues MixerValues::at(const GradeNode& node, int64_t f)
{
    namespace gk = gradekeys;
    MixerValues v;
    const QString* const keys[9] = {&gk::MixerRR, &gk::MixerRG, &gk::MixerRB, &gk::MixerGR,
        &gk::MixerGG, &gk::MixerGB, &gk::MixerBR, &gk::MixerBG, &gk::MixerBB};
    for (int i = 0; i < 9; ++i)
        v.m[i] = float(node.value(*keys[i], f));
    v.grey[0] = float(node.value(gk::MixerMonoR, f));
    v.grey[1] = float(node.value(gk::MixerMonoG, f));
    v.grey[2] = float(node.value(gk::MixerMonoB, f));
    v.mono = node.value(gk::MixerMono, f) >= 0.5;
    v.preserveLum = node.value(gk::MixerPreserve, f) >= 0.5;
    return v;
}

bool MixerValues::isIdentity() const
{
    if (mono)
        return false;
    const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    return std::equal(std::begin(id), std::end(id), std::begin(m));
}

void applyMixer(const MixerValues& v, float c[3])
{
    if (v.mono) {
        float w[3] = {v.grey[0], v.grey[1], v.grey[2]};
        if (v.preserveLum) {
            const float sum = w[0] + w[1] + w[2];
            if (std::fabs(sum) > kMixerEpsilon)
                for (float& x : w)
                    x /= sum;
        }
        const float g = std::max(w[0] * c[0] + w[1] * c[1] + w[2] * c[2], 0.0f);
        c[0] = c[1] = c[2] = g;
        return;
    }
    const float in[3] = {c[0], c[1], c[2]};
    float out[3];
    for (int r = 0; r < 3; ++r)
        out[r] = v.m[r * 3] * in[0] + v.m[r * 3 + 1] * in[1] + v.m[r * 3 + 2] * in[2];
    if (v.preserveLum) {
        const float yIn = luma(in);
        const float yOut = luma(out);
        if (yOut > kMixerEpsilon) {
            const float k = yIn / yOut;
            for (float& x : out)
                x *= k;
        }
    }
    std::copy(out, out + 3, c);
}

// AgX (applyAgx, agxLinearize and the constants below): the inset / outset matrices, the log2
// range and the polynomial are those of the minimal AgX implementation, whose licence is:
//
// MIT License
//
// Copyright (c) 2024 Missing Deadlines (Benjamin Wrensch)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this
// software and associated documentation files (the "Software"), to deal in the Software
// without restriction, including without limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
// to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
// PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
// FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
// Source: https://iolite-engine.com/blog_posts/minimal_agx_implementation (values from
// https://github.com/sobotka/AgX).

AgxValues AgxValues::at(const GradeNode& node, int64_t f)
{
    namespace gk = gradekeys;
    AgxValues v;
    v.on = node.value(gk::AgxOn, f) >= 0.5;
    const auto pick = [&](const QString& key, int count) {
        return std::clamp(int(std::lround(node.value(key, f))), 0, count - 1);
    };
    v.input = AgxInput(pick(gk::AgxInput, int(AgxInput::Count)));
    v.exposure = float(std::clamp(node.value(gk::AgxExposure, f), -8.0, 8.0));
    v.look = AgxLook(pick(gk::AgxLook, int(AgxLook::Count)));
    v.output = AgxOutput(pick(gk::AgxOutput, int(AgxOutput::Count)));
    return v;
}

AgxValues AgxValues::fromToneMap(const ToneMap& map)
{
    AgxValues v;
    v.on = map.agx;
    v.look = map.look;
    return v;
}

float agxLinearize(AgxInput input, float x)
{
    switch (input) {
    case AgxInput::Rec709:
        return std::pow(std::max(x, 0.0f), 2.4f); // BT.1886 display, the usual video picture
    case AgxInput::Srgb:
        return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
    case AgxInput::Linear:
        return x;
    case AgxInput::AppleLog: { // Apple Log Profile white paper
        constexpr float r0 = -0.05641088f, rt = 0.01f, c = 47.28711236f, beta = 0.00964052f,
                        gamma = 0.08550479f, delta = 0.69336945f;
        const float pt = c * (rt - r0) * (rt - r0);
        return x >= pt ? std::exp2((x - delta) / gamma) - beta : std::sqrt(std::max(x, 0.0f) / c) + r0;
    }
    case AgxInput::SLog3: // Sony S-Log3 technical summary
        if (x >= 171.2102946929f / 1023.0f)
            return std::pow(10.0f, (x * 1023.0f - 420.0f) / 261.5f) * (0.18f + 0.01f) - 0.01f;
        return (x * 1023.0f - 95.0f) * 0.01125f / (171.2102946929f - 95.0f);
    case AgxInput::VLog: // Panasonic V-Log / V-Gamut reference manual
        if (x < 0.181f)
            return (x - 0.125f) / 5.6f;
        return std::pow(10.0f, (x - 0.598206f) / 0.241514f) - 0.00873f;
    case AgxInput::LogC3: // ARRI LogC3, EI 800
        if (x > 5.367655f * 0.010591f + 0.092809f)
            return (std::pow(10.0f, (x - 0.385537f) / 0.247190f) - 0.052272f) / 5.555556f;
        return (x - 0.092809f) / 5.367655f;
    case AgxInput::Count:
        break;
    }
    return x;
}

namespace {
// Sobotka's AgX inset (row-major: out = M * in) and its inverse, from his OCIO config.
constexpr float kAgxInset[9] = {0.842479062253094f, 0.0784335999999992f, 0.0792237451477643f,
    0.0423282422610123f, 0.878468636469772f, 0.0791661274605434f, 0.0423756549057051f,
    0.0784336f, 0.879142973793104f};
constexpr float kAgxOutset[9] = {1.19687900512017f, -0.0980208811401368f, -0.0990297440797205f,
    -0.0528968517574562f, 1.15190312990417f, -0.0989611768448433f, -0.0529716355144438f,
    -0.0980434501171241f, 1.15107367264116f};
constexpr float kAgxMinEv = -12.47393f;
constexpr float kAgxMaxEv = 4.026069f;

/// The sigmoid of AgX's default contrast, a 6th degree polynomial of the 0..1 log encoding.
float agxCurve(float x)
{
    const float x2 = x * x;
    const float x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2
        + 0.1191f * x - 0.00232f;
}
} // namespace

void applyAgx(const AgxValues& v, float c[3])
{
    const float gain = std::exp2(v.exposure);
    float lin[3];
    for (int i = 0; i < 3; ++i)
        lin[i] = agxLinearize(v.input, c[i]) * gain;
    float e[3];
    for (int r = 0; r < 3; ++r) {
        const float m = kAgxInset[r * 3] * lin[0] + kAgxInset[r * 3 + 1] * lin[1]
            + kAgxInset[r * 3 + 2] * lin[2];
        const float ev = std::clamp(std::log2(std::max(m, 1e-10f)), kAgxMinEv, kAgxMaxEv);
        e[r] = agxCurve((ev - kAgxMinEv) / (kAgxMaxEv - kAgxMinEv));
    }
    if (v.look != AgxLook::None) {
        float slope[3] = {1, 1, 1};
        float power = 1.0f, sat = 1.0f;
        if (v.look == AgxLook::Punchy) {
            power = 1.35f;
            sat = 1.4f;
        } else if (v.look == AgxLook::Golden) {
            slope[1] = 0.9f;
            slope[2] = 0.5f;
            power = 0.8f;
            sat = 0.8f;
        } else {
            sat = 0.0f; // Greyscale
        }
        for (int i = 0; i < 3; ++i)
            e[i] = std::pow(std::max(e[i] * slope[i], 0.0f), power);
        const float y = luma(e);
        for (float& x : e)
            x = y + sat * (x - y);
    }
    for (int r = 0; r < 3; ++r) {
        float o = kAgxOutset[r * 3] * e[0] + kAgxOutset[r * 3 + 1] * e[1]
            + kAgxOutset[r * 3 + 2] * e[2];
        o = std::max(o, 0.0f);
        // The curve is fitted for a display of gamma 2.2 (sRGB); Rec. 709 video is shown at 2.4.
        if (v.output == AgxOutput::Rec709)
            o = std::pow(std::pow(o, 2.2f), 1.0f / 2.4f);
        c[r] = std::min(o, 1.0f);
    }
}

AutoColorResult autoColor(const float* rgb, size_t pixels)
{
    AutoColorResult out;
    constexpr int kBins = 1024;
    if (pixels < 16)
        return out;
    float low[3], high[3], median[3];
    for (int ch = 0; ch < 3; ++ch) {
        std::vector<uint32_t> hist(kBins, 0);
        for (size_t i = 0; i < pixels; ++i) {
            const float v = std::clamp(rgb[i * 3 + size_t(ch)], 0.0f, 1.0f);
            ++hist[size_t(std::min(int(v * kBins), kBins - 1))];
        }
        auto percentile = [&](double fraction) {
            const double target = fraction * double(pixels);
            double seen = 0.0;
            for (int b = 0; b < kBins; ++b) {
                seen += hist[size_t(b)];
                if (seen >= target)
                    return (float(b) + 0.5f) / float(kBins);
            }
            return 1.0f;
        };
        low[ch] = percentile(kAutoColorClip);
        high[ch] = percentile(1.0 - kAutoColorClip);
        median[ch] = percentile(0.5);
    }
    // Flat picture: nothing to stretch.
    for (int ch = 0; ch < 3; ++ch) {
        if (high[ch] - low[ch] < 0.05f)
            return out;
    }
    float stretched[3];
    for (int ch = 0; ch < 3; ++ch) {
        const float b = low[ch];
        out.lift[ch] = -b / (1.0f - b);
        out.gain[ch] = 1.0f / (high[ch] + out.lift[ch] * (1.0f - high[ch]));
        stretched[ch] = std::clamp(
            out.gain[ch] * (median[ch] + out.lift[ch] * (1.0f - median[ch])), 0.02f, 0.98f);
    }
    const float target = (stretched[0] + stretched[1] + stretched[2]) / 3.0f;
    for (int ch = 0; ch < 3; ++ch) {
        // pow(stretched, e) = target with e = 2^(-kGammaRange gamma)
        const float e = std::log(target) / std::log(stretched[ch]);
        out.gamma[ch] = std::clamp(-std::log2(e) / kGammaRange, -kAutoColorMaxGamma, kAutoColorMaxGamma);
    }
    out.valid = true;
    return out;
}

float liftGammaGain(float x, float lift, float gamma, float gain)
{
    const float v = std::max(gain * (x + lift * (1.0f - x)), 0.0f);
    return std::pow(v, std::exp2(-kGammaRange * gamma));
}

void applyPrimaries(const PrimaryValues& p, float c[3])
{
    // 1. Offset: shifts everything (Resolve's offset wheel, "printer lights" style).
    for (int ch = 0; ch < 3; ++ch)
        c[ch] += p.offset[ch] + p.offset[3] + p.logOffset[ch] + p.logOffset[3];
    // 2. Temperature (red <-> blue) and tint (green <-> magenta), simple channel gains.
    c[0] *= 1.0f + kTemperatureRange * p.temperature;
    c[2] *= 1.0f - kTemperatureRange * p.temperature;
    c[1] *= 1.0f - kTintRange * p.tint;
    // 2b. Log palette zones: the Shadow, Midtone and Highlight wheels as offsets weighted by the
    //     luma of the colour so far (additive: a gain in linear light is an offset in log).
    if (p.logActive()) {
        float w[3];
        logZoneWeights(std::clamp(luma(c), 0.0f, 1.0f), p.logLowRange, p.logHighRange, w);
        for (int ch = 0; ch < 3; ++ch) {
            c[ch] += w[0] * (p.logShadow[ch] + p.logShadow[3])
                + w[1] * (p.logMid[ch] + p.logMid[3]) + w[2] * (p.logHigh[ch] + p.logHigh[3]);
        }
    }
    // 3. Per-channel lift / gamma / gain.
    for (int ch = 0; ch < 3; ++ch)
        c[ch] = liftGammaGain(c[ch], p.lift[ch], p.gamma[ch], p.gain[ch]);
    // 4. Master wheels: per channel (RGB) or on luma only, preserving the colour (YRGB),
    //    mixed by lum mix (1 = luma only, Resolve's default).
    const float y = luma(c);
    const float y2 = liftGammaGain(y, p.lift[3], p.gamma[3], p.gain[3]);
    for (int ch = 0; ch < 3; ++ch) {
        const float rgbMaster = liftGammaGain(c[ch], p.lift[3], p.gamma[3], p.gain[3]);
        const float lumaMaster = c[ch] + (y2 - y);
        c[ch] = rgbMaster + (lumaMaster - rgbMaster) * p.lumMix;
    }
    // 5. Contrast around the pivot.
    for (int ch = 0; ch < 3; ++ch)
        c[ch] = (c[ch] - p.pivot) * p.contrast + p.pivot;
    // 5b. Shad / Hi: luma weighted lift or crush of the shadows, raise or recovery of the
    //     highlights. A positive amount moves the channel towards white, a negative one towards
    //     black, so nothing overshoots.
    if (p.shadows != 0.0f || p.highlights != 0.0f) {
        const float y = std::clamp(luma(c), 0.0f, 1.0f);
        const float wShadow = 1.0f - smoothStep(0.0f, 0.5f, y);
        const float wHigh = smoothStep(0.5f, 1.0f, y);
        for (int ch = 0; ch < 3; ++ch) {
            const float x = std::clamp(c[ch], 0.0f, 1.0f);
            c[ch] += p.shadows * wShadow * kShadHiStrength * (p.shadows > 0.0f ? 1.0f - x : x);
            c[ch] += p.highlights * wHigh * kShadHiStrength * (p.highlights > 0.0f ? 1.0f - x : x);
        }
    }
    // 6. Hue: rotation about the grey axis (PrimaryValues::hueMatrix).
    {
        const float* m = p.hueMatrix;
        const float r = c[0], g = c[1], b = c[2];
        c[0] = m[0] * r + m[1] * g + m[2] * b;
        c[1] = m[3] * r + m[4] * g + m[5] * b;
        c[2] = m[6] * r + m[7] * g + m[8] * b;
    }
    // 7. Saturation around Rec. 709 luma.
    const float y3 = luma(c);
    for (int ch = 0; ch < 3; ++ch)
        c[ch] = y3 + (c[ch] - y3) * p.saturation;
    // 8. Color Boost: saturation scaled by 1 - the colour's own (HSV) saturation, so dull
    //    colours gain most and strong ones are left alone (negative: the opposite way round).
    if (p.colorBoost != 0.0f) {
        const float mx = std::max(c[0], std::max(c[1], c[2]));
        const float mn = std::min(c[0], std::min(c[1], c[2]));
        const float s = mx > 1e-5f ? std::clamp((mx - mn) / mx, 0.0f, 1.0f) : 0.0f;
        const float factor = 1.0f + p.colorBoost * (1.0f - s);
        const float y4 = luma(c);
        for (int ch = 0; ch < 3; ++ch)
            c[ch] = y4 + (c[ch] - y4) * factor;
    }
}

double evaluateCurve(const CurvePoints& points, GradeCurve curve, double x)
{
    const double neutralY = curveNeutral(curve);
    if (points.isEmpty())
        return neutralY < 0 ? x : neutralY;
    std::vector<double> xs;
    std::vector<double> ys;
    xs.reserve(size_t(points.size()) * 3 + 2);
    ys.reserve(xs.capacity());
    auto push = [&](double px, double py) {
        if (!xs.empty() && px - xs.back() < kMinPointGap)
            return;
        xs.push_back(px);
        ys.push_back(py);
    };
    if (isHueCurve(curve)) {
        // Periodic: the points one turn earlier and later around the real ones.
        x -= std::floor(x);
        if (points.size() == 1)
            return points[0].y;
        for (int pass = -1; pass <= 1; ++pass) {
            for (const CurvePoint& p : points)
                push(p.x + pass, p.y);
        }
    } else {
        // Neutral end points where the user's points do not reach the ends.
        if (points.front().x > kMinPointGap)
            push(0.0, neutralY < 0 ? 0.0 : neutralY);
        for (const CurvePoint& p : points)
            push(p.x, p.y);
        if (points.back().x < 1.0 - kMinPointGap)
            push(1.0, neutralY < 0 ? 1.0 : neutralY);
    }
    if (xs.size() == 1)
        return ys[0];
    return monotoneCubic(xs, ys, x);
}

int addCurvePoint(CurvePoints& points, GradeCurve curve, double x, double y)
{
    x = std::clamp(x, 0.0, 1.0);
    y = std::clamp(y, 0.0, 1.0);
    if (points.isEmpty() && curveNeutral(curve) >= 0) {
        // Hue / lum-vs-sat: anchor the neighbourhood so only the area around x moves.
        const double n = curveNeutral(curve);
        if (isHueCurve(curve)) {
            const double lo = x - kNeighbourGap - std::floor(x - kNeighbourGap);
            const double hi = x + kNeighbourGap - std::floor(x + kNeighbourGap);
            points.append({lo, n});
            points.append({hi, n});
        } else {
            if (x - kNeighbourGap > 0)
                points.append({x - kNeighbourGap, n});
            if (x + kNeighbourGap < 1)
                points.append({x + kNeighbourGap, n});
        }
    }
    for (int i = 0; i < points.size(); ++i) {
        if (std::abs(points[i].x - x) < kMinPointGap) {
            points[i].y = y;
            return i;
        }
    }
    points.append({x, y});
    std::sort(points.begin(), points.end(),
        [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
    for (int i = 0; i < points.size(); ++i) {
        if (points[i].x == x)
            return i;
    }
    return int(points.size()) - 1;
}

CurveTables CurveTables::bake(const GradeCurves& curves)
{
    CurveTables t;
    for (int c = 0; c < kGradeCurveCount; ++c) {
        const GradeCurve kind = GradeCurve(c);
        t.identity[size_t(c)] = curves.isIdentity(kind);
        t.allIdentity = t.allIdentity && t.identity[size_t(c)];
        const bool neutral = t.identity[size_t(c)];
        for (int i = 0; i < kCurveTableSize; ++i) {
            const double x = double(i) / (kCurveTableSize - 1);
            const double y
                = neutral ? (isChannelCurve(kind) ? x : 0.5) : evaluateCurve(curves[kind], kind, x);
            float v = float(y);
            if (kind == GradeCurve::HueHue)
                v = float(y - 0.5);
            else if (!isChannelCurve(kind))
                v = float(2.0 * y);
            t.table[size_t(c)][size_t(i)] = v;
        }
    }
    return t;
}

int CurveTables::mask() const
{
    int m = 0;
    for (int c = 0; c < kGradeCurveCount; ++c) {
        if (!identity[size_t(c)])
            m |= 1 << c;
    }
    return m;
}

float CurveTables::sample(GradeCurve c, float x) const
{
    const float pos = std::clamp(x, 0.0f, 1.0f) * float(kCurveTableSize - 1);
    const int i = std::min(int(pos), kCurveTableSize - 2);
    const float f = pos - float(i);
    const auto& row = table[size_t(c)];
    return row[size_t(i)] + (row[size_t(i) + 1] - row[size_t(i)]) * f;
}

void applyCurves(const CurveTables& t, float c[3])
{
    if (t.allIdentity)
        return;
    auto active = [&t](GradeCurve k) { return !t.identity[size_t(k)]; };
    for (int ch = 0; ch < 3; ++ch) {
        float v = std::clamp(c[ch], 0.0f, 1.0f);
        if (active(GradeCurve::Luma))
            v = t.sample(GradeCurve::Luma, v);
        if (active(GradeCurve(ch + 1)))
            v = t.sample(GradeCurve(ch + 1), v);
        c[ch] = v;
    }
    const bool hueHue = active(GradeCurve::HueHue);
    const bool hueSat = active(GradeCurve::HueSat);
    const bool hueLum = active(GradeCurve::HueLum);
    const bool lumSat = active(GradeCurve::LumSat);
    const bool satSat = active(GradeCurve::SatSat);
    const bool satLum = active(GradeCurve::SatLum);
    if (!hueHue && !hueSat && !hueLum && !lumSat && !satSat && !satLum)
        return;
    float hsl[3];
    rgbToHsl(c, hsl);
    const float h = hsl[0];
    const float s0 = hsl[1];
    const float y = std::clamp(luma(c), 0.0f, 1.0f);
    float factor = 1.0f;
    if (hueSat)
        factor *= t.sample(GradeCurve::HueSat, h);
    if (lumSat)
        factor *= t.sample(GradeCurve::LumSat, y);
    if (satSat)
        factor *= t.sample(GradeCurve::SatSat, hsl[1]);
    if (hueHue) {
        const float shifted = h + t.sample(GradeCurve::HueHue, h);
        hsl[0] = shifted - std::floor(shifted);
    }
    hsl[1] = std::clamp(hsl[1] * factor, 0.0f, 1.0f);
    hslToRgb(hsl, c);
    if (hueLum || satLum) {
        // Luminance of the selected hues / saturations: the luma moves, the colour
        // differences stay.
        float lumFactor = 1.0f;
        if (hueLum)
            lumFactor *= t.sample(GradeCurve::HueLum, h);
        if (satLum)
            lumFactor *= t.sample(GradeCurve::SatLum, s0);
        const float y2 = luma(c);
        const float delta = y2 * (lumFactor - 1.0f);
        for (int ch = 0; ch < 3; ++ch)
            c[ch] += delta;
    }
}

SoftClipValues SoftClipValues::at(const GradeNode& node, int64_t f)
{
    namespace gk = gradekeys;
    SoftClipValues v;
    v.low = float(std::clamp(node.value(gk::SoftClipLow, f), 0.0, 1.0));
    v.lowSoft = float(std::clamp(node.value(gk::SoftClipLowSoft, f), 0.0, 1.0));
    v.high = float(std::clamp(node.value(gk::SoftClipHigh, f), 0.0, 1.0));
    v.highSoft = float(std::clamp(node.value(gk::SoftClipHighSoft, f), 0.0, 1.0));
    v.high = std::max(v.high, v.low);
    return v;
}

bool SoftClipValues::isIdentity() const
{
    return low <= 0.0f && lowSoft <= 0.0f && high >= 1.0f && highSoft <= 0.0f;
}

float softClip(const SoftClipValues& v, float x)
{
    const float wl = v.lowSoft * 0.5f;
    if (wl <= 0.0f) {
        x = std::max(x, v.low);
    } else if (x < v.low + wl) {
        if (x <= v.low - wl) {
            x = v.low;
        } else {
            const float t = (v.low + wl) - x;
            x = (v.low + wl) - t + t * t / (4.0f * wl);
        }
    }
    const float wh = v.highSoft * 0.5f;
    if (wh <= 0.0f) {
        x = std::min(x, v.high);
    } else if (x > v.high - wh) {
        if (x >= v.high + wh) {
            x = v.high;
        } else {
            const float t = x - (v.high - wh);
            x = (v.high - wh) + t - t * t / (4.0f * wh);
        }
    }
    return x;
}

void applySoftClip(const SoftClipValues& v, float rgb[3])
{
    for (int ch = 0; ch < 3; ++ch)
        rgb[ch] = softClip(v, rgb[ch]);
}

void addCurvePickPoints(CurvePoints& points, GradeCurve curve, const float rgb[3])
{
    float c[3] = {std::clamp(rgb[0], 0.0f, 1.0f), std::clamp(rgb[1], 0.0f, 1.0f),
        std::clamp(rgb[2], 0.0f, 1.0f)};
    float hsl[3];
    rgbToHsl(c, hsl);
    double x = 0.0;
    switch (curve) {
    case GradeCurve::Luma:
        x = luma(c);
        break;
    case GradeCurve::Red:
        x = c[0];
        break;
    case GradeCurve::Green:
        x = c[1];
        break;
    case GradeCurve::Blue:
        x = c[2];
        break;
    case GradeCurve::HueHue:
    case GradeCurve::HueSat:
    case GradeCurve::HueLum:
        x = hsl[0];
        break;
    case GradeCurve::LumSat:
        x = luma(c);
        break;
    case GradeCurve::SatSat:
    case GradeCurve::SatLum:
        x = hsl[1];
        break;
    }
    x = std::clamp(x, 0.0, 1.0);
    const bool channel = isChannelCurve(curve);
    std::vector<double> xs {x};
    if (!channel) {
        for (const double d : {-kCurvePickSpread, kCurvePickSpread}) {
            double nx = x + d;
            if (isHueCurve(curve))
                nx -= std::floor(nx);
            else if (nx <= 0.0 || nx >= 1.0)
                continue;
            xs.push_back(nx);
        }
    }
    for (const double px : xs) {
        const bool exists = std::any_of(points.cbegin(), points.cend(),
            [&](const CurvePoint& p) { return std::abs(p.x - px) < kCurvePickSpread / 4.0; });
        if (exists)
            continue;
        // Where the curve is now at px: a pick must not move the picture by itself.
        const double y = points.isEmpty() ? (channel ? px : 0.5)
                                          : evaluateCurve(points, curve, px);
        points.append({px, y});
    }
    std::sort(points.begin(), points.end(),
        [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
}

void rgbToHsl(const float rgb[3], float hsl[3])
{
    const float r = rgb[0], g = rgb[1], b = rgb[2];
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});
    const float l = (mx + mn) * 0.5f;
    const float d = mx - mn;
    float h = 0.0f;
    float s = 0.0f;
    if (d > 1e-6f) {
        s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
        if (mx == r)
            h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        else if (mx == g)
            h = (b - r) / d + 2.0f;
        else
            h = (r - g) / d + 4.0f;
        h /= 6.0f;
    }
    hsl[0] = h - std::floor(h);
    hsl[1] = s;
    hsl[2] = l;
}

namespace {
float hueToChannel(float p, float q, float t)
{
    t -= std::floor(t);
    if (t < 1.0f / 6.0f)
        return p + (q - p) * 6.0f * t;
    if (t < 0.5f)
        return q;
    if (t < 2.0f / 3.0f)
        return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}
} // namespace

void hslToRgb(const float hsl[3], float rgb[3])
{
    const float h = hsl[0], s = hsl[1], l = hsl[2];
    if (s <= 0.0f) {
        rgb[0] = rgb[1] = rgb[2] = l;
        return;
    }
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    rgb[0] = hueToChannel(p, q, h + 1.0f / 3.0f);
    rgb[1] = hueToChannel(p, q, h);
    rgb[2] = hueToChannel(p, q, h - 1.0f / 3.0f);
}

float smoothStep(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

namespace {
/// A soft range key of @p v: 1 inside [lo, hi], a smoothstep of width @p softness outside.
float rangeKey(float v, double lo, double hi, double softness)
{
    const float soft = std::max(float(softness), kMinSoftness);
    float k = 1.0f;
    if (lo > 0.0)
        k *= smoothStep(float(lo) - soft, float(lo), v);
    if (hi < 1.0)
        k *= 1.0f - smoothStep(float(hi), float(hi) + soft, v);
    return k;
}

/// 3D mode: how much @p c belongs to the neighbourhood (RGB distance) of one picked colour.
float colourNear(const GradeQualifierColor& p, const float c[3], float range, float soft)
{
    const float dr = c[0] - p.r, dg = c[1] - p.g, db = c[2] - p.b;
    const float dist = std::sqrt(dr * dr + dg * dg + db * db);
    return 1.0f - smoothStep(range, range + std::max(soft, kMinSoftness), dist);
}
} // namespace

float qualifierKey(const GradeQualifier& q, const float rgb[3])
{
    if (!q.enabled)
        return 1.0f;
    const float c[3] = {std::clamp(rgb[0], 0.0f, 1.0f), std::clamp(rgb[1], 0.0f, 1.0f),
        std::clamp(rgb[2], 0.0f, 1.0f)};
    float key = 1.0f;
    switch (q.mode) {
    case QualifierMode::Hsl: {
        float hsl[3];
        rgbToHsl(c, hsl);
        if (q.useHue) {
            // Circular distance to the centre hue, in turns (0..0.5).
            const float rel = hsl[0] - float(q.hueCenter) + 0.5f;
            const float dist = std::abs(rel - std::floor(rel) - 0.5f);
            const float half = float(q.hueWidth) * 0.5f;
            const float soft = std::max(float(q.hueSoft), kMinSoftness);
            key *= 1.0f - smoothStep(half, half + soft, dist);
        }
        if (q.useSat)
            key *= rangeKey(hsl[1], q.satLow, q.satHigh, q.satSoft);
        if (q.useLum)
            key *= rangeKey(hsl[2], q.lumLow, q.lumHigh, q.lumSoft);
        break;
    }
    case QualifierMode::Luma:
        key = rangeKey(
            luma(c), q.lumLow, q.lumHigh, q.lumSoft);
        break;
    case QualifierMode::Rgb:
        if (q.useRed)
            key *= rangeKey(c[0], q.redLow, q.redHigh, q.redSoft);
        if (q.useGreen)
            key *= rangeKey(c[1], q.greenLow, q.greenHigh, q.greenSoft);
        if (q.useBlue)
            key *= rangeKey(c[2], q.blueLow, q.blueHigh, q.blueSoft);
        break;
    case QualifierMode::ThreeD: {
        // The union of the picked colours' neighbourhoods, minus the subtracted ones'.
        // (The idea of a soft ellipsoid / box selection in colour space follows the frei0r
        // select0r plugin, GPL.)
        float in = 0.0f, out = 0.0f;
        const int count = std::min(int(q.colors.size()), kMaxQualifierColors);
        for (int i = 0; i < count; ++i) {
            const float k = colourNear(q.colors[i], c, float(q.colorRange), float(q.colorSoft));
            float& best = q.colors[i].subtract ? out : in;
            best = std::max(best, k);
        }
        key = in * (1.0f - out);
        break;
    }
    }
    return q.invert ? 1.0f - key : key;
}

} // namespace kk::grademath

namespace kk::grademath {

WindowGeometry trackedWindowGeometry(
    const GradeNode& node, const GradeWindow& w, int64_t localFrame, double aspect)
{
    namespace wk = gradewindowkeys;
    WindowGeometry r;
    r.cx = w.value(wk::CenterX, localFrame);
    r.cy = w.value(wk::CenterY, localFrame);
    r.width = w.value(wk::Width, localFrame);
    r.height = w.value(wk::Height, localFrame);
    r.rotation = w.value(wk::Rotation, localFrame);
    if (!node.track.hasData())
        return r;
    const GradeTrackSample s = node.track.applied(localFrame);
    const double a = s.rot * std::numbers::pi / 180.0;
    const double cs = std::cos(a), sn = std::sin(a);
    // Offset from the pivot in units of the picture height, scaled, turned, moved.
    const double vx = (r.cx - node.track.pivot.x()) * aspect * s.sx;
    const double vy = (r.cy - node.track.pivot.y()) * s.sy;
    r.cx = node.track.pivot.x() + (cs * vx - sn * vy) / aspect + s.dx;
    r.cy = node.track.pivot.y() + sn * vx + cs * vy + s.dy;
    r.width *= s.sx;
    r.height *= s.sy;
    r.rotation += s.rot;
    return r;
}

WindowGeometry untrackedWindowGeometry(
    const GradeTrack& track, const WindowGeometry& shown, int64_t localFrame, double aspect)
{
    if (!track.hasData())
        return shown;
    const GradeTrackSample s = track.applied(localFrame);
    const double a = s.rot * std::numbers::pi / 180.0;
    const double cs = std::cos(a), sn = std::sin(a);
    const double px = (shown.cx - track.pivot.x() - s.dx) * aspect;
    const double py = shown.cy - track.pivot.y() - s.dy;
    const double ux = cs * px + sn * py; // turn back
    const double uy = -sn * px + cs * py;
    WindowGeometry r = shown;
    r.cx = track.pivot.x() + ux / std::max(1e-6, s.sx) / aspect;
    r.cy = track.pivot.y() + uy / std::max(1e-6, s.sy);
    r.width = shown.width / std::max(1e-6, s.sx);
    r.height = shown.height / std::max(1e-6, s.sy);
    r.rotation = shown.rotation - s.rot;
    return r;
}

GradeWindow trackedWindow(
    const GradeNode& node, const GradeWindow& w, int64_t localFrame, double aspect)
{
    if (!node.track.hasData())
        return w;
    namespace wk = gradewindowkeys;
    const WindowGeometry g = trackedWindowGeometry(node, w, localFrame, aspect);
    GradeWindow out = w;
    const std::pair<QString, double> values[] = {{wk::CenterX, g.cx}, {wk::CenterY, g.cy},
        {wk::Width, g.width}, {wk::Height, g.height}, {wk::Rotation, g.rotation}};
    for (const auto& [key, value] : values) {
        out.params.values.remove(key);
        out.params.setBase(key, value);
    }
    return out;
}

namespace {
constexpr double kCurveFlattenTolerance = 0.1; ///< chord error of a Curve outline, pixels
} // namespace

std::vector<WindowValues> evaluateWindows(
    const GradeNode& node, int64_t localFrame, const PictureGeometry& g)
{
    namespace wk = gradewindowkeys;
    std::vector<WindowValues> out;
    for (const GradeWindow& w : node.windows) {
        if (!w.isRenderable() || int(out.size()) >= kMaxGradeWindows)
            continue;
        WindowValues v;
        v.shape = int(w.shape);
        v.invert = w.invert;
        v.opacity = float(std::clamp(w.value(wk::Opacity, localFrame), 0.0, 1.0));
        const WindowGeometry geo
            = trackedWindowGeometry(node, w, localFrame, g.width / std::max(1e-9, g.height));
        v.cx = float(g.originX + geo.cx * g.width);
        v.cy = float(g.originY + geo.cy * g.height);
        const double angle = geo.rotation * std::numbers::pi / 180.0;
        v.cosR = float(std::cos(angle));
        v.sinR = float(std::sin(angle));
        const double width = std::abs(geo.width) * g.width;
        const double height = std::abs(geo.height) * g.height;
        v.halfW = float(width * 0.5);
        v.halfH = float(height * 0.5);
        // Softness in picture heights; at least half a pixel (the antialiasing).
        v.softIn = float(std::max(0.5, std::max(0.0, w.value(wk::SoftIn, localFrame)) * g.height));
        v.softOut
            = float(std::max(0.5, std::max(0.0, w.value(wk::SoftOut, localFrame)) * g.height));
        if (w.shape == GradeWindowShape::Polygon || w.shape == GradeWindowShape::Curve) {
            const GradeWindowShapePoints pts = w.shapeAt(localFrame);
            const int n = std::min(int(pts.size()), kMaxWindowPoints);
            if (w.shape == GradeWindowShape::Polygon) {
                v.points.reserve(size_t(n) * 2);
                for (int i = 0; i < n; ++i) {
                    v.points.push_back(float(pts[i].pos.x() * width));
                    v.points.push_back(float(pts[i].pos.y() * height));
                }
            } else {
                // A Curve is flattened here (window pixels, error well under a pixel), so the
                // coverage code and the GPU treat it as a polygon.
                MaskPath path;
                path.reserve(n);
                for (int i = 0; i < n; ++i) {
                    MaskPoint m;
                    m.x = pts[i].pos.x() * width;
                    m.y = pts[i].pos.y() * height;
                    m.inX = pts[i].inHandle.x() * width;
                    m.inY = pts[i].inHandle.y() * height;
                    m.outX = pts[i].outHandle.x() * width;
                    m.outY = pts[i].outHandle.y() * height;
                    path.append(m);
                }
                QVector<QPointF> flat;
                flattenPath(path, true, kCurveFlattenTolerance, [](QPointF p) { return p; }, flat);
                const int count = std::min(int(flat.size()), kMaxWindowOutlinePoints);
                v.points.reserve(size_t(count) * 2);
                for (int i = 0; i < count; ++i) {
                    v.points.push_back(float(flat[i].x()));
                    v.points.push_back(float(flat[i].y()));
                }
            }
            v.shape = int(GradeWindowShape::Polygon);
        }
        out.push_back(std::move(v));
    }
    return out;
}

float windowCoverage(const WindowValues& w, float px, float py)
{
    // Window-local pixel position: undo the centre, then the clockwise rotation.
    const float dx = px - w.cx;
    const float dy = py - w.cy;
    const float lx = w.cosR * dx + w.sinR * dy;
    const float ly = -w.sinR * dx + w.cosR * dy;
    float d = 0.0f; // signed distance in pixels, < 0 inside
    switch (GradeWindowShape(w.shape)) {
    case GradeWindowShape::Circle: {
        const float rx = std::max(w.halfW, 1e-3f);
        const float ry = std::max(w.halfH, 1e-3f);
        const float k = std::sqrt((lx / rx) * (lx / rx) + (ly / ry) * (ly / ry));
        d = (k - 1.0f) * std::min(rx, ry);
        break;
    }
    case GradeWindowShape::Linear: {
        const float qx = std::abs(lx) - w.halfW;
        const float qy = std::abs(ly) - w.halfH;
        const float ox = std::max(qx, 0.0f);
        const float oy = std::max(qy, 0.0f);
        d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f);
        break;
    }
    case GradeWindowShape::Gradient:
        d = ly;
        break;
    case GradeWindowShape::Polygon: {
        const int n = int(w.points.size() / 2);
        float best = 1e30f;
        bool inside = false;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            const float ax = w.points[size_t(j) * 2], ay = w.points[size_t(j) * 2 + 1];
            const float bx = w.points[size_t(i) * 2], by = w.points[size_t(i) * 2 + 1];
            const float ex = bx - ax, ey = by - ay;
            const float wx = lx - ax, wy = ly - ay;
            const float len2 = ex * ex + ey * ey;
            const float t = len2 > 0.0f ? std::clamp((wx * ex + wy * ey) / len2, 0.0f, 1.0f) : 0.0f;
            const float qx = wx - ex * t, qy = wy - ey * t;
            best = std::min(best, qx * qx + qy * qy);
            // Even-odd crossing test of a ray towards +x.
            if ((ay > ly) != (by > ly) && lx < ax + (ly - ay) * ex / ey)
                inside = !inside;
        }
        d = std::sqrt(best) * (inside ? -1.0f : 1.0f);
        break;
    }
    }
    return 1.0f - smoothStep(-w.softIn, w.softOut, d);
}

float windowsMatte(const std::vector<WindowValues>& windows, float px, float py)
{
    if (windows.empty())
        return 1.0f;
    float m = 0.0f;
    for (const WindowValues& w : windows) {
        float c = windowCoverage(w, px, py);
        if (w.invert)
            c = 1.0f - c;
        c *= w.opacity;
        m = m + c - m * c;
    }
    return m;
}

void layerOver(float result[3], const float in[3], const float o[3], float alpha)
{
    for (int ch = 0; ch < 3; ++ch)
        result[ch] = (1.0f - alpha) * result[ch] + alpha * in[ch] + (o[ch] - in[ch]);
}

} // namespace kk::grademath

namespace kk::grademath {

bool BlurStage::active() const
{
    for (int ch = 0; ch < 3; ++ch) {
        if ((sigmaH[ch] > 0.0f || sigmaV[ch] > 0.0f) && (mix[ch] > 0.0f || sharp[ch] > 0.0f))
            return true;
    }
    return false;
}

bool BlurStage::uniformSigma() const
{
    for (int ch = 1; ch < 3; ++ch) {
        if (sigmaH[ch] != sigmaH[0] || sigmaV[ch] != sigmaV[0])
            return false;
    }
    return true;
}

bool BlurValues::active() const
{
    return std::any_of(stages.begin(), stages.end(), [](const BlurStage& s) { return s.active(); });
}

namespace {

/// Writes the signed radius @p radius (0.5 = none) of channel @p ch into @p s: positive blurs
/// (@p blurWhenHigh) or sharpens.
void setChannel(BlurStage& s, int ch, float amount, bool highBlurs, float blurSigma,
    float sharpSigma, float sharpStrength, float hFactor, float vFactor)
{
    float sigma = 0.0f;
    float mix = 0.0f;
    float sharp = 0.0f;
    if (amount != 0.0f) {
        if ((amount > 0.0f) == highBlurs) {
            sigma = std::abs(amount) * blurSigma;
            mix = 1.0f;
        } else {
            sigma = sharpSigma;
            sharp = std::abs(amount) * sharpStrength;
        }
    }
    s.sigmaH[ch] = sigma * hFactor >= kMinBlurSigma ? sigma * hFactor : 0.0f;
    s.sigmaV[ch] = sigma * vFactor >= kMinBlurSigma ? sigma * vFactor : 0.0f;
    s.mix[ch] = mix;
    s.sharp[ch] = sharp;
    if (s.sigmaH[ch] == 0.0f && s.sigmaV[ch] == 0.0f)
        s.mix[ch] = s.sharp[ch] = 0.0f;
}

} // namespace

BlurValues BlurValues::at(const GradeNode& node, int64_t f, double pictureHeight)
{
    namespace gk = gradekeys;
    auto v = [&](const QString& key) { return float(node.value(key, f)); };
    auto unit = [&](const QString& key) { return std::clamp(v(key), 0.0f, 1.0f); };
    const float maxSigma = kBlurMaxSigma * float(pictureHeight);
    const float sharpSigma = kSharpSigma * float(pictureHeight);
    BlurValues out;
    auto axes = [&](const QString& ratioKey, const QString& scalingKey, float& h, float& vv,
                    float& scale) {
        const float ratio = unit(ratioKey);
        h = std::min(1.0f, 2.0f * (1.0f - ratio));
        vv = std::min(1.0f, 2.0f * ratio);
        scale = 2.0f * unit(scalingKey);
    };
    float h = 1, vv = 1, scale = 1;
    // Blur: radius above 0.5 blurs, below sharpens.
    {
        axes(gk::BlurRatio, gk::BlurScaling, h, vv, scale);
        const QString* keys[3] = {&gk::BlurRadiusR, &gk::BlurRadiusG, &gk::BlurRadiusB};
        for (int ch = 0; ch < 3; ++ch) {
            const float amount = std::clamp((v(*keys[ch]) - 0.5f) * 2.0f, -1.0f, 1.0f);
            setChannel(out.stages[0], ch, amount, true, maxSigma * scale, sharpSigma * scale,
                kSharpGain, h, vv);
        }
    }
    // Sharpen: radius above 0.5 sharpens (strength by Level), below blurs.
    {
        axes(gk::SharpRatio, gk::SharpScaling, h, vv, scale);
        const QString* keys[3] = {&gk::SharpRadiusR, &gk::SharpRadiusG, &gk::SharpRadiusB};
        const float level = unit(gk::SharpLevel) * 2.0f;
        for (int ch = 0; ch < 3; ++ch) {
            const float amount = std::clamp((v(*keys[ch]) - 0.5f) * 2.0f, -1.0f, 1.0f);
            setChannel(out.stages[1], ch, amount, false, maxSigma * scale, sharpSigma * scale,
                kSharpGain * level, h, vv);
        }
        out.stages[1].coring = unit(gk::SharpCoring);
    }
    // Mist: a blur mixed over the picture.
    {
        axes(gk::MistRatio, gk::MistScaling, h, vv, scale);
        const float radius = unit(gk::MistRadius);
        const float mix = unit(gk::MistMix);
        for (int ch = 0; ch < 3; ++ch) {
            setChannel(out.stages[2], ch, radius, true, maxSigma * scale, 0.0f, 0.0f, h, vv);
            out.stages[2].mix[ch] *= mix;
        }
    }
    // Midtone Detail: a wide blur put back as an unsharp mask (+) or a blend towards it (-),
    // weighted to the midtones.
    {
        const float md = std::clamp(v(gk::MidtoneDetail) / 100.0f, -1.0f, 1.0f);
        BlurStage& s = out.stages[3];
        const float sigma = kMidDetailSigma * float(pictureHeight);
        if (md != 0.0f && sigma >= kMinBlurSigma) {
            s.midWeight = true;
            for (int ch = 0; ch < 3; ++ch) {
                s.sigmaH[ch] = s.sigmaV[ch] = sigma;
                s.sharp[ch] = md > 0.0f ? md * kMidDetailGain : 0.0f;
                s.mix[ch] = md < 0.0f ? -md : 0.0f;
            }
        }
    }
    return out;
}

int blurTapRadius(float sigma)
{
    if (!(sigma >= kMinBlurSigma))
        return 0;
    return std::min(int(std::ceil(3.0f * sigma)), kMaxBlurTaps);
}

std::vector<float> blurKernel(float sigma)
{
    const int r = blurTapRadius(sigma);
    std::vector<float> w(size_t(2 * r + 1), 1.0f);
    if (r == 0)
        return w;
    float sum = 0.0f;
    for (int k = -r; k <= r; ++k) {
        w[size_t(k + r)] = std::exp(-float(k * k) / (2.0f * sigma * sigma));
        sum += w[size_t(k + r)];
    }
    for (float& x : w)
        x /= sum;
    return w;
}

float coringFactor(float detail, float coring)
{
    if (coring <= 0.0f)
        return 1.0f;
    return smoothStep(0.0f, coring * kCoringRange, std::abs(detail));
}

float midtoneWeight(float y)
{
    y = std::clamp(y, 0.0f, 1.0f);
    return 4.0f * y * (1.0f - y);
}

float blurCombine(float c, float b, float mix, float sharp, float coring, float weight)
{
    const float d = c - b;
    return c + ((b - c) * mix + d * sharp * coringFactor(d, coring)) * weight;
}

} // namespace kk::grademath

// ---- Matte Finesse ----
// Cleaning a matte picture: grayscale morphology (a flat square structuring element, run as
// two 1D passes) for Clean Black / Clean White and Shrink / Grow, a median for Denoise and a
// Gaussian for Blur Radius. These are the standard operators image tools use for matte
// clean-up: Natron's Erode / Dilate / Median / Blur nodes and GIMP's / GEGL's value-propagate,
// median-blur and gaussian-blur filters (both GPL-compatible); the code here is written
// from the operators' definitions.
namespace kk::grademath {

namespace {

int radiusFor(double value, double height)
{
    const double v = std::clamp(std::abs(value), 0.0, 1.0);
    if (v <= 0.0)
        return 0;
    return std::clamp(int(std::lround(v * kMatteMaxRadius * height)), 1, kMatteMaxTaps);
}

/// Min (@p dilate false) or max over a 2 radius + 1 window along x (@p horizontal) or y.
void morphLine(const std::vector<float>& src, std::vector<float>& dst, int w, int h, int radius,
    bool dilate, bool horizontal)
{
    dst.resize(src.size());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float best = dilate ? 0.0f : 1.0f;
            for (int k = -radius; k <= radius; ++k) {
                const int sx = horizontal ? std::clamp(x + k, 0, w - 1) : x;
                const int sy = horizontal ? y : std::clamp(y + k, 0, h - 1);
                const float v = src[size_t(sy) * size_t(w) + size_t(sx)];
                best = dilate ? std::max(best, v) : std::min(best, v);
            }
            dst[size_t(y) * size_t(w) + size_t(x)] = best;
        }
    }
}

void morph(std::vector<float>& m, int w, int h, int radius, bool dilate)
{
    if (radius <= 0)
        return;
    std::vector<float> tmp;
    morphLine(m, tmp, w, h, radius, dilate, true);
    morphLine(tmp, m, w, h, radius, dilate, false);
}

} // namespace

bool MatteFinesse::active() const
{
    return cleanBlack > 0 || cleanWhite > 0 || shrinkGrow != 0 || denoise > 0 || blurSigma > 0
        || ratio != 0.0f;
}

MatteFinesse MatteFinesse::at(const GradeQualifier& q, double pictureHeight)
{
    MatteFinesse f;
    f.cleanBlack = radiusFor(q.cleanBlack, pictureHeight);
    f.cleanWhite = radiusFor(q.cleanWhite, pictureHeight);
    f.shrinkGrow = radiusFor(q.shrinkGrow, pictureHeight) * (q.shrinkGrow < 0.0 ? -1 : 1);
    f.denoise = int(std::lround(std::clamp(q.denoise, 0.0, 1.0) * kMatteMaxDenoise));
    f.blurSigma
        = float(std::clamp(q.blurRadius, 0.0, 1.0) * kMatteMaxSigma * pictureHeight);
    if (f.blurSigma < kMinBlurSigma)
        f.blurSigma = 0.0f;
    f.ratio = float(std::clamp(q.inOutRatio, -1.0, 1.0));
    return f;
}

float matteRatio(float blurred, float ratio)
{
    // Positive: the soft edge moves inside the original matte (the blurred matte is
    // reached later); negative: outside (it is reached earlier).
    if (ratio > 0.0f) {
        const float t = 0.5f * ratio;
        return std::clamp((blurred - t) / (1.0f - t), 0.0f, 1.0f);
    }
    if (ratio < 0.0f) {
        const float t = -0.5f * ratio;
        return std::clamp(blurred / (1.0f - t), 0.0f, 1.0f);
    }
    return blurred;
}

void matteMedian(const std::vector<float>& src, std::vector<float>& dst, int w, int h, int radius)
{
    dst = src;
    if (radius <= 0)
        return;
    std::vector<float> window;
    window.reserve(size_t((2 * radius + 1) * (2 * radius + 1)));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            window.clear();
            for (int j = -radius; j <= radius; ++j) {
                for (int i = -radius; i <= radius; ++i) {
                    window.push_back(src[size_t(std::clamp(y + j, 0, h - 1)) * size_t(w)
                        + size_t(std::clamp(x + i, 0, w - 1))]);
                }
            }
            std::nth_element(window.begin(), window.begin() + window.size() / 2, window.end());
            dst[size_t(y) * size_t(w) + size_t(x)] = window[window.size() / 2];
        }
    }
}

void applyMatteFinesse(const MatteFinesse& f, std::vector<float>& m, int w, int h)
{
    if (!f.active() || w <= 0 || h <= 0)
        return;
    // Clean Black: opening (an erosion then a dilation) removes white specks from the black.
    morph(m, w, h, f.cleanBlack, false);
    morph(m, w, h, f.cleanBlack, true);
    // Clean White: closing removes black specks from the white.
    morph(m, w, h, f.cleanWhite, true);
    morph(m, w, h, f.cleanWhite, false);
    if (f.denoise > 0) {
        std::vector<float> out;
        matteMedian(m, out, w, h, f.denoise);
        m.swap(out);
    }
    morph(m, w, h, std::abs(f.shrinkGrow), f.shrinkGrow > 0);
    if (f.blurSigma > 0.0f) {
        const std::vector<float> kernel = blurKernel(f.blurSigma);
        const int r = int(kernel.size() / 2);
        std::vector<float> tmp(m.size());
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float acc = 0.0f;
                for (int k = -r; k <= r; ++k)
                    acc += kernel[size_t(k + r)] * m[size_t(y) * size_t(w) + size_t(std::clamp(x + k, 0, w - 1))];
                tmp[size_t(y) * size_t(w) + size_t(x)] = acc;
            }
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float acc = 0.0f;
                for (int k = -r; k <= r; ++k)
                    acc += kernel[size_t(k + r)] * tmp[size_t(std::clamp(y + k, 0, h - 1)) * size_t(w) + size_t(x)];
                m[size_t(y) * size_t(w) + size_t(x)] = acc;
            }
        }
    }
    if (f.ratio != 0.0f) {
        for (float& v : m)
            v = matteRatio(v, f.ratio);
    }
}


// ---- HDR palette ----

HdrValues HdrValues::at(const GradeNode& node, int64_t f)
{
    namespace gk = gradekeys;
    static const gk::Wheel zoneWheels[kHdrZones] = {gk::Wheel::HdrGlobal, gk::Wheel::HdrShadow,
        gk::Wheel::HdrLight, gk::Wheel::HdrHighlight, gk::Wheel::HdrSpecular};
    const QString* const sats[kHdrZones] = {&gk::HdrGlobalSat, &gk::HdrShadowSat,
        &gk::HdrLightSat, &gk::HdrHighlightSat, &gk::HdrSpecularSat};
    HdrValues v;
    for (int z = 0; z < kHdrZones; ++z) {
        const float master = float(node.value(gk::wheelKey(zoneWheels[z], 3), f));
        for (int ch = 0; ch < 3; ++ch)
            v.stops[z][ch] = master
                + kHdrTintStops * float(node.value(gk::wheelKey(zoneWheels[z], ch), f));
        v.sat[z] = float(node.value(*sats[z], f));
    }
    const QString* const edges[3] = {&gk::HdrShadowRange, &gk::HdrLightRange, &gk::HdrHighlightRange};
    const QString* const falls[3]
        = {&gk::HdrShadowFalloff, &gk::HdrLightFalloff, &gk::HdrHighlightFalloff};
    for (int k = 0; k < 3; ++k) {
        v.edge[k] = float(node.value(*edges[k], f));
        if (k > 0)
            v.edge[k] = std::max(v.edge[k], v.edge[k - 1]); // the zones stay in order
        v.falloff[k] = std::max(float(node.value(*falls[k], f)), 0.0f);
    }
    const gk::Wheel black = gk::Wheel::HdrBlack;
    const float master = float(node.value(gk::wheelKey(black, 3), f));
    for (int ch = 0; ch < 3; ++ch)
        v.black[ch] = kHdrBlackScale * (master + float(node.value(gk::wheelKey(black, ch), f)));
    return v;
}

bool HdrValues::isIdentity() const
{
    for (int z = 0; z < kHdrZones; ++z) {
        if (sat[z] != 1.0f)
            return false;
        for (int ch = 0; ch < 3; ++ch) {
            if (stops[z][ch] != 0.0f)
                return false;
        }
    }
    return black[0] == 0.0f && black[1] == 0.0f && black[2] == 0.0f;
}

namespace {
float smoothEdge(float e0, float e1, float x)
{
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace

void applyHdr(const HdrValues& v, float c[3])
{
    float lin[3];
    for (int i = 0; i < 3; ++i)
        lin[i] = std::copysign(std::pow(std::fabs(c[i]), kHdrGamma), c[i]);
    const float e = std::log2(std::max(luma(lin), 1e-6f) / kHdrMidGrey);
    float s[3];
    for (int k = 0; k < 3; ++k) {
        const float half = 0.5f * std::max(v.falloff[k], 0.01f);
        s[k] = smoothEdge(v.edge[k] - half, v.edge[k] + half, e);
    }
    const float w[kHdrZones] = {
        1.0f, 1.0f - s[0], std::max(s[0] - s[1], 0.0f), std::max(s[1] - s[2], 0.0f), s[2]};
    float sat = 1.0f;
    for (int z = 1; z < kHdrZones; ++z)
        sat += w[z] * (v.sat[z] - 1.0f);
    sat *= v.sat[0];
    for (int ch = 0; ch < 3; ++ch) {
        float st = v.stops[0][ch];
        for (int z = 1; z < kHdrZones; ++z)
            st += w[z] * v.stops[z][ch];
        lin[ch] *= std::exp2(st);
    }
    const float y = luma(lin);
    for (int ch = 0; ch < 3; ++ch) {
        float o = y + sat * (lin[ch] - y);
        o += v.black[ch] * (1.0f - std::clamp(o, 0.0f, 1.0f));
        c[ch] = std::copysign(std::pow(std::fabs(o), 1.0f / kHdrGamma), o);
    }
}

// ---- Color Warper ----
//
// The mesh warp is our own: a displacement field over the Hue - Sat and the Chroma - Luma planes,
// held on a regular mesh of pins and interpolated with smoothstep-weighted bilinear blending
// (zero slope at every pin, so a pin's colour lands exactly on the pin and the field is C1
// between them). The idea of moving colours with pins on a grid follows the zone based tools
// of darktable (color zones) and the hue / saturation warps of Natron's colour correct nodes;
// no code of either was used.

namespace {

void sampleMesh(const std::vector<float>& t, int rowBase, int cols, int rows, bool wrap, float fx,
    float fy, float out[2])
{
    int x0 = 0, x1 = 0;
    float tx = 0.0f;
    if (wrap) {
        fx -= std::floor(fx);
        const float gx = fx * float(cols);
        x0 = std::min(int(gx), cols - 1);
        tx = gx - float(x0);
        x1 = (x0 + 1) % cols;
    } else {
        const float gx = std::clamp(fx, 0.0f, 1.0f) * float(cols - 1);
        x0 = std::min(int(gx), cols - 2);
        tx = gx - float(x0);
        x1 = x0 + 1;
    }
    const float gy = std::clamp(fy, 0.0f, 1.0f) * float(rows - 1);
    const int y0 = std::min(int(gy), rows - 2);
    const float ty = gy - float(y0);
    tx = tx * tx * (3.0f - 2.0f * tx);
    const float uy = ty * ty * (3.0f - 2.0f * ty);
    const auto at = [&](int x, int y) { return &t[(size_t(rowBase + y) * kWarpTexWidth + size_t(x)) * 2]; };
    for (int k = 0; k < 2; ++k) {
        const float a = at(x0, y0)[k] + (at(x1, y0)[k] - at(x0, y0)[k]) * tx;
        const float b = at(x0, y0 + 1)[k] + (at(x1, y0 + 1)[k] - at(x0, y0 + 1)[k]) * tx;
        out[k] = a + (b - a) * uy;
    }
}

} // namespace

WarpTables WarpTables::bake(const GradeWarp& warp)
{
    WarpTables t;
    t.texels.assign(size_t(kWarpTexWidth) * kWarpTexHeight * 2, 0.0f);
    std::tie(t.hsCols, t.hsRows) = warpGridSize(WarpGrid::HueSat, warp.resolution);
    std::tie(t.clCols, t.clRows) = warpGridSize(WarpGrid::ChromaLuma, warp.resolution);
    const auto put = [&](int x, int y, float dx, float dy) {
        float* p = &t.texels[(size_t(y) * kWarpTexWidth + size_t(x)) * 2];
        p[0] = dx;
        p[1] = dy;
    };
    for (int row = 1; row < t.hsRows; ++row) { // row 0 is the grey centre: it stays
        for (int col = 0; col < t.hsCols; ++col) {
            const auto [dx, dy] = warp.pin(WarpGrid::HueSat, col, row);
            put(col, row, dx, dy);
            t.hs = t.hs || dx != 0.0f || dy != 0.0f;
        }
    }
    for (int row = 0; row < t.clRows; ++row) {
        for (int col = 0; col < t.clCols; ++col) {
            auto [dx, dy] = warp.pin(WarpGrid::ChromaLuma, col, row);
            if (col == 0)
                dx = 0.0f; // grey stays grey
            put(col, kWarpMaxRows + row, dx, dy);
            t.cl = t.cl || dx != 0.0f || dy != 0.0f;
        }
    }
    return t;
}

void WarpTables::sampleHueSat(float hue, float sat, float out[2]) const
{
    sampleMesh(texels, 0, hsCols, hsRows, true, hue, sat, out);
}

void WarpTables::sampleChromaLuma(float sat, float lum, float out[2]) const
{
    sampleMesh(texels, kWarpMaxRows, clCols, clRows, false, sat, lum, out);
}

void applyWarp(const WarpTables& t, float c[3])
{
    constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
    const float y = luma(c);
    float cb = (c[2] - y) / kWarpCbScale;
    float cr = (c[0] - y) / kWarpCrScale;
    const float chroma = std::sqrt(cb * cb + cr * cr);
    float s = chroma / kWarpChromaMax;
    if (t.hs && chroma > 1e-6f) {
        float h = std::atan2(cr, cb) / kTwoPi;
        h -= std::floor(h);
        float d[2];
        t.sampleHueSat(h, s, d);
        h += d[0];
        s = std::max(s + d[1], 0.0f);
        const float ch = s * kWarpChromaMax;
        cb = ch * std::cos(kTwoPi * h);
        cr = ch * std::sin(kTwoPi * h);
    }
    float yo = y;
    if (t.cl) {
        float d[2];
        t.sampleChromaLuma(s, y, d);
        const float s2 = std::max(s + d[0], 0.0f);
        if (s > 1e-6f) {
            cb *= s2 / s;
            cr *= s2 / s;
        }
        yo = y + d[1];
    }
    c[0] = yo + kWarpCrScale * cr;
    c[2] = yo + kWarpCbScale * cb;
    c[1] = (yo - kLumaR * c[0] - kLumaB * c[2]) / kLumaG;
}

GradeWarp resampleWarp(const GradeWarp& warp, int resolution)
{
    resolution = std::clamp(resolution, 0, kWarpResolutions - 1);
    if (resolution == warp.resolution)
        return warp;
    const WarpTables old = WarpTables::bake(warp);
    GradeWarp out;
    out.resolution = quint8(resolution);
    if (old.hs) {
        const auto [cols, rows] = warpGridSize(WarpGrid::HueSat, resolution);
        for (int row = 1; row < rows; ++row) {
            for (int col = 0; col < cols; ++col) {
                float d[2];
                old.sampleHueSat(float(col) / float(cols), float(row) / float(rows - 1), d);
                out.setPin(WarpGrid::HueSat, col, row, d[0], d[1]);
            }
        }
    }
    if (old.cl) {
        const auto [cols, rows] = warpGridSize(WarpGrid::ChromaLuma, resolution);
        for (int row = 0; row < rows; ++row) {
            for (int col = 0; col < cols; ++col) {
                float d[2];
                old.sampleChromaLuma(float(col) / float(cols - 1), float(row) / float(rows - 1), d);
                out.setPin(WarpGrid::ChromaLuma, col, row, d[0], d[1]);
            }
        }
    }
    return out;
}

} // namespace kk::grademath
