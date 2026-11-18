// SPDX-License-Identifier: GPL-3.0-or-later
// CompFilterMath implementation. Blur kernels, glow and light wrap: standard methods written
// for Kinoklip (the Light Wrap controls follow the open-source Natron LightWrap plugin's
// parameter set; see attribution.md).
#include "render/CompFilterMath.h"

#include "core/NodeRegistry.h"

#include <QColor>

#include <algorithm>
#include <cmath>

namespace kk::render::compmath {

namespace {

namespace k = nodekeys;

double num(const ParamSet& p, const QString& key, int64_t frame, double fallback)
{
    return p.valueAtTime(key, double(frame), fallback);
}

/// One separable pass along x (@p horizontal) or y with the symmetric kernel w[0..r].
FImage convolve(const FImage& in, const std::vector<double>& w, bool horizontal)
{
    const int r = int(w.size()) - 1;
    FImage out(in.w, in.h);
    for (int y = 0; y < in.h; ++y) {
        for (int x = 0; x < in.w; ++x) {
            double acc[4] = {0, 0, 0, 0};
            for (int i = -r; i <= r; ++i) {
                const float* p = horizontal ? in.clamped(x + i, y) : in.clamped(x, y + i);
                const double wt = w[size_t(std::abs(i))];
                for (int c = 0; c < 4; ++c)
                    acc[c] += wt * p[c];
            }
            float* o = out.at(x, y);
            for (int c = 0; c < 4; ++c)
                o[c] = float(acc[c]);
        }
    }
    return out;
}

void normalise(std::vector<double>& w)
{
    double sum = 0;
    for (size_t i = 0; i < w.size(); ++i)
        sum += i == 0 ? w[i] : 2.0 * w[i];
    for (double& v : w)
        v /= sum;
}

} // namespace

FImage toFloat(const QImage& image)
{
    FImage out(image.width(), image.height());
    for (int y = 0; y < image.height(); ++y) {
        const QRgb* p = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            float* o = out.at(x, y);
            o[0] = qRed(p[x]) / 255.0f;
            o[1] = qGreen(p[x]) / 255.0f;
            o[2] = qBlue(p[x]) / 255.0f;
            o[3] = qAlpha(p[x]) / 255.0f;
        }
    }
    return out;
}

QImage toImage(const FImage& image)
{
    QImage out(image.w, image.h, QImage::Format_ARGB32_Premultiplied);
    const auto q = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    for (int y = 0; y < image.h; ++y) {
        QRgb* p = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < image.w; ++x) {
            const float* c = image.at(x, y);
            const int a = q(c[3]);
            p[x] = qRgba(std::min(q(c[0]), a), std::min(q(c[1]), a), std::min(q(c[2]), a), a);
        }
    }
    return out;
}

FImage gaussian(const FImage& in, double sigmaX, double sigmaY)
{
    FImage out = in;
    for (int axis = 0; axis < 2; ++axis) {
        const double sigma = axis == 0 ? sigmaX : sigmaY;
        if (sigma < 0.25)
            continue;
        const int r = int(std::ceil(3.0 * sigma));
        std::vector<double> w(size_t(r) + 1);
        for (int i = 0; i <= r; ++i)
            w[size_t(i)] = std::exp(-0.5 * i * i / (sigma * sigma));
        normalise(w);
        out = convolve(out, w, axis == 0);
    }
    return out;
}

FImage box(const FImage& in, double rx, double ry)
{
    FImage out = in;
    for (int axis = 0; axis < 2; ++axis) {
        const double radius = axis == 0 ? rx : ry;
        if (radius < 0.25)
            continue;
        const int whole = int(std::floor(radius));
        std::vector<double> w(size_t(whole) + 1, 1.0);
        if (radius - whole > 1e-3)
            w.push_back(radius - whole);
        normalise(w);
        out = convolve(out, w, axis == 0);
    }
    return out;
}

FImage morphology(const FImage& in, double radius, bool dilate)
{
    if (radius < 0.01)
        return in;
    FImage cur = in;
    for (int axis = 0; axis < 2; ++axis) {
        double left = radius;
        while (left > 0.01) {
            const double chunk = std::min(left, 16.0);
            left -= chunk;
            const int taps = int(std::ceil(chunk - 1e-6));
            const float frac = float(chunk - (taps - 1));
            FImage next(cur.w, cur.h);
            for (int y = 0; y < cur.h; ++y) {
                for (int x = 0; x < cur.w; ++x) {
                    float r[4];
                    std::copy_n(cur.at(x, y), 4, r);
                    for (int i = 1; i <= taps; ++i) {
                        const float* a = axis == 0 ? cur.clamped(x + i, y) : cur.clamped(x, y + i);
                        const float* b = axis == 0 ? cur.clamped(x - i, y) : cur.clamped(x, y - i);
                        const float t = i == taps ? frac : 1.0f;
                        for (int c = 0; c < 4; ++c) {
                            const float e = dilate ? std::max(r[c], std::max(a[c], b[c]))
                                                   : std::min(r[c], std::min(a[c], b[c]));
                            r[c] += (e - r[c]) * t;
                        }
                    }
                    std::copy_n(r, 4, next.at(x, y));
                }
            }
            cur = std::move(next);
        }
    }
    return cur;
}

FImage blurByKind(const FImage& in, int kind, double sizeX, double sizeY, int passes)
{
    if (kind == GaussianBlur)
        return gaussian(in, sizeX * 0.5, sizeY * 0.5);
    int n = 1;
    if (kind == BartlettBlur)
        n = 2;
    else if (kind == MultiBoxBlur)
        n = std::clamp(passes, 1, 10);
    FImage out = in;
    for (int i = 0; i < n; ++i)
        out = box(out, sizeX / n, sizeY / n);
    return out;
}

// ---- Soft Glow ----

SoftGlowParams softGlowParams(const ParamSet& p, int64_t frame)
{
    SoftGlowParams g;
    g.filter = blurfilters::index(p.staticValue(k::BlurFilter, blurfilters::Gaussian).toString());
    g.passes = std::clamp(int(num(p, k::NumPasses, frame, 4)), 1, 10);
    g.threshold = float(std::clamp(num(p, k::GlowThreshold, frame, 0.5), 0.0, 4.0));
    g.gain = float(std::clamp(num(p, k::Gain, frame, 1.0), 0.0, 10.0));
    g.sizeX = std::max(0.0, num(p, k::GlowSize, frame, 20));
    g.sizeY = p.staticValue(k::LockXY, true).toBool()
        ? g.sizeX
        : std::max(0.0, num(p, k::GlowSizeY, frame, 20));
    const QString scaleKeys[4] = {k::GlowScaleR, k::GlowScaleG, k::GlowScaleB, k::GlowScaleA};
    const QString chanKeys[4] = {k::GlowRed, k::GlowGreen, k::GlowBlue, k::GlowAlpha};
    for (int i = 0; i < 4; ++i) {
        g.scale[i] = float(std::clamp(num(p, scaleKeys[i], frame, 1.0), 0.0, 4.0));
        g.channel[i] = p.staticValue(chanKeys[i], true).toBool();
    }
    return g;
}

void softGlowHighlight(const SoftGlowParams& g, const float c[4], float h[4])
{
    const float a = c[3];
    float top = 0;
    for (int i = 0; i < 3; ++i) {
        const float s = a > 0 ? c[i] / a : 0.0f;
        const float over = std::max(s - g.threshold, 0.0f);
        h[i] = a * over;
        top = std::max(top, over);
    }
    h[3] = a * std::clamp(top / std::max(1.0f - g.threshold, 1e-3f), 0.0f, 1.0f);
}

void softGlowCombine(const SoftGlowParams& g, const float c[4], const float glow[4], float o[4])
{
    float add[4];
    for (int i = 0; i < 4; ++i)
        add[i] = g.channel[i] ? glow[i] * g.gain * g.scale[i] : 0.0f;
    const float a = c[3] + (1.0f - c[3]) * std::min(add[3], 1.0f);
    for (int i = 0; i < 3; ++i)
        o[i] = std::min(c[i] + add[i], a);
    o[3] = a;
}

FImage softGlow(const FImage& in, const SoftGlowParams& g, double scale)
{
    FImage hl(in.w, in.h);
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x)
            softGlowHighlight(g, in.at(x, y), hl.at(x, y));
    const FImage blurred = blurByKind(hl, g.filter, g.sizeX * scale, g.sizeY * scale, g.passes);
    FImage out(in.w, in.h);
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x)
            softGlowCombine(g, in.at(x, y), blurred.at(x, y), out.at(x, y));
    return out;
}

// ---- Light Wrap ----

LightWrapParams lightWrapParams(const ParamSet& p, int64_t frame)
{
    LightWrapParams l;
    l.intensity = float(std::clamp(num(p, k::WrapIntensity, frame, 0.5), 0.0, 4.0));
    l.saturation = float(std::clamp(num(p, k::Saturation, frame, 1.0), 0.0, 4.0));
    l.lumaTolerance = float(std::clamp(num(p, k::WrapLumaTolerance, frame, 0.0), 0.0, 1.0));
    l.diffuse = std::max(0.0, num(p, k::WrapDiffuse, frame, 15));
    l.fgBlur = std::max(0.0, num(p, k::WrapFgBlur, frame, 10));
    l.bgBlur = std::max(0.0, num(p, k::WrapBgBlur, frame, 0));
    l.lumaBased = p.staticValue(k::WrapLumaBased, true).toBool();
    l.glow = p.staticValue(k::WrapGlow, false).toBool();
    l.wrapOnly = p.staticValue(k::WrapOnly, false).toBool();
    l.useConstant = p.staticValue(k::WrapUseConstant, false).toBool();
    l.screen = p.staticValue(k::WrapMerge, wrapmerges::Add).toString() == wrapmerges::Screen;
    const QColor c = p.staticValue(k::WrapColor, QColor(255, 255, 255)).value<QColor>();
    if (c.isValid()) {
        l.constant[0] = float(c.redF());
        l.constant[1] = float(c.greenF());
        l.constant[2] = float(c.blueF());
    }
    return l;
}

void lightWrapEdge(const LightWrapParams& l, const float bg[4], float ab, float w[4])
{
    float s[3] = {0, 0, 0};
    if (bg[3] > 0) {
        for (int i = 0; i < 3; ++i)
            s[i] = bg[i] / bg[3];
    }
    const float lum = 0.2126f * s[0] + 0.7152f * s[1] + 0.0722f * s[2];
    float col[3];
    for (int i = 0; i < 3; ++i)
        col[i] = l.useConstant ? l.constant[i] : s[i];
    if (!l.lumaBased) {
        const float top = std::max(col[0], std::max(col[1], col[2]));
        for (float& v : col)
            v /= std::max(top, 1e-3f);
    }
    const float gate = l.lumaTolerance <= 0 ? 1.0f
                                            : std::clamp((lum - l.lumaTolerance) / 0.1f, 0.0f, 1.0f);
    const float cl = 0.2126f * col[0] + 0.7152f * col[1] + 0.0722f * col[2];
    const float edge = std::clamp(1.0f - ab, 0.0f, 1.0f) * gate;
    for (int i = 0; i < 3; ++i)
        w[i] = std::max(cl + (col[i] - cl) * l.saturation, 0.0f) * edge;
    w[3] = edge;
}

void lightWrapCombine(const LightWrapParams& l, const float fg[4], const float wrap[4], float o[4])
{
    const float inside = l.glow ? 1.0f : fg[3];
    float wr[3];
    for (int i = 0; i < 3; ++i)
        wr[i] = wrap[i] * l.intensity * inside;
    const float wa = std::min(wrap[3] * l.intensity, 1.0f) * inside;
    if (l.wrapOnly) {
        for (int i = 0; i < 3; ++i)
            o[i] = std::min(wr[i], wa);
        o[3] = wa;
        return;
    }
    const float a = l.glow ? fg[3] + (1.0f - fg[3]) * wa : fg[3];
    for (int i = 0; i < 3; ++i) {
        const float v = l.screen ? fg[i] + wr[i] * (1.0f - std::min(fg[i], 1.0f)) : fg[i] + wr[i];
        o[i] = std::min(v, a);
    }
    o[3] = a;
}

FImage lightWrap(const FImage& fg, const FImage& bg, const LightWrapParams& l, double scale)
{
    FImage alpha(fg.w, fg.h);
    for (int y = 0; y < fg.h; ++y)
        for (int x = 0; x < fg.w; ++x) {
            float* o = alpha.at(x, y);
            o[0] = o[1] = o[2] = o[3] = fg.at(x, y)[3];
        }
    const FImage ab = gaussian(alpha, l.fgBlur * scale * 0.5, l.fgBlur * scale * 0.5);
    const FImage bb = gaussian(bg, l.bgBlur * scale * 0.5, l.bgBlur * scale * 0.5);
    FImage edge(fg.w, fg.h);
    for (int y = 0; y < fg.h; ++y)
        for (int x = 0; x < fg.w; ++x)
            lightWrapEdge(l, bb.at(x, y), ab.at(x, y)[3], edge.at(x, y));
    const FImage wd = gaussian(edge, l.diffuse * scale * 0.5, l.diffuse * scale * 0.5);
    FImage out(fg.w, fg.h);
    for (int y = 0; y < fg.h; ++y)
        for (int x = 0; x < fg.w; ++x)
            lightWrapCombine(l, fg.at(x, y), wd.at(x, y), out.at(x, y));
    return out;
}

} // namespace kk::render::compmath
