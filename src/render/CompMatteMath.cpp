// SPDX-License-Identifier: GPL-3.0-or-later
// CompMatteMath implementation. A colour-difference key (the screen channel against the others),
// the morphological and blur matte filters, and a fringe restore from the matte before the
// threshold: standard methods written for Kinoklip, using the control sets of the public Fusion
// manual (Delta Keyer, Ultra Keyer, Matte Control). Spill methods and the fringe weighting are
// our own choices (spec section 115).
#include "render/CompMatteMath.h"

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

float clamp01(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

void colorOf(const ParamSet& p, const QString& key, QColor fallback, float out[3])
{
    const QColor c = p.staticValue(key, fallback).value<QColor>();
    const QColor use = c.isValid() ? c : fallback;
    out[0] = float(use.redF());
    out[1] = float(use.greenF());
    out[2] = float(use.blueF());
}

FImage grey(int w, int h)
{
    return FImage(w, h);
}

void setGrey(float* p, float v)
{
    p[0] = p[1] = p[2] = p[3] = v;
}

/// Largest component index of a screen colour.
int dominant(const float bg[3])
{
    int d = 0;
    for (int i = 1; i < 3; ++i)
        if (bg[i] > bg[d])
            d = i;
    return d;
}

/// The value the screen channel is measured against.
float others(const KeyerParams& k, const float v[3], int d)
{
    const int o1 = (d + 1) % 3, o2 = (d + 2) % 3;
    if (k.kind == UltraKind)
        return std::max(v[o1] * k.levels[o1], v[o2] * k.levels[o2]);
    return k.balance * std::max(v[o1], v[o2]) + (1.0f - k.balance) * std::min(v[o1], v[o2]);
}

} // namespace

MatteFilterParams matteFilterParams(const ParamSet& p, int64_t frame)
{
    MatteFilterParams m;
    m.filter = blurfilters::index(p.staticValue(k::BlurFilter, blurfilters::Gaussian).toString());
    m.passes = std::clamp(int(num(p, k::NumPasses, frame, 4)), 1, 10);
    m.erode = num(p, k::MatteErode, frame, 0);
    m.blur = std::max(0.0, num(p, k::MatteBlur, frame, 0));
    m.gamma = float(std::clamp(num(p, k::MatteGamma, frame, 1.0), 0.1, 10.0));
    m.cleanBg = float(std::clamp(num(p, k::CleanBackground, frame, 0.0), 0.0, 1.0));
    m.cleanFg = float(std::clamp(num(p, k::CleanForeground, frame, 0.0), 0.0, 1.0));
    m.low = float(std::clamp(num(p, k::MatteLow, frame, 0.0), 0.0, 1.0));
    m.high = float(std::clamp(num(p, k::MatteHigh, frame, 1.0), 0.0, 1.0));
    m.restoreFringe = float(std::clamp(num(p, k::RestoreFringe, frame, 0.0), 0.0, 1.0));
    m.invert = p.staticValue(k::Invert, false).toBool();
    return m;
}

float matteShape(const MatteFilterParams& m, float v)
{
    v = clamp01(v);
    if (m.gamma != 1.0f)
        v = std::pow(v, 1.0f / m.gamma);
    v = clamp01((v - m.cleanBg) / std::max(1.0f - m.cleanBg - m.cleanFg, 1e-3f));
    return clamp01((v - m.low) / std::max(m.high - m.low, 1e-3f));
}

float matteRestore(const MatteFilterParams& m, float filtered, float shaped, float near)
{
    const float fringe = std::max(filtered - shaped, 0.0f);
    return clamp01(shaped + m.restoreFringe * fringe * clamp01(near * 3.0f));
}

FImage matteFilter(const FImage& matte, const MatteFilterParams& m, double scale)
{
    FImage cur = matte;
    if (m.erode != 0.0)
        cur = morphology(cur, std::abs(m.erode) * scale, m.erode > 0);
    if (m.blur > 0.0)
        cur = blurByKind(cur, m.filter, m.blur * scale, m.blur * scale, m.passes);
    FImage shaped(cur.w, cur.h);
    for (int y = 0; y < cur.h; ++y)
        for (int x = 0; x < cur.w; ++x)
            setGrey(shaped.at(x, y), matteShape(m, cur.at(x, y)[3]));
    FImage near;
    if (m.restoreFringe > 0.0f)
        near = gaussian(shaped, 3.0 * scale, 3.0 * scale);
    FImage out(cur.w, cur.h);
    for (int y = 0; y < cur.h; ++y) {
        for (int x = 0; x < cur.w; ++x) {
            float v = shaped.at(x, y)[3];
            if (m.restoreFringe > 0.0f)
                v = matteRestore(m, clamp01(cur.at(x, y)[3]), v, near.at(x, y)[3]);
            setGrey(out.at(x, y), m.invert ? 1.0f - v : v);
        }
    }
    return out;
}

// ---- Keyers ----

KeyerParams keyerParams(const ParamSet& p, int64_t frame, KeyerKind kind)
{
    KeyerParams k2;
    k2.kind = kind;
    colorOf(p, k::Color, QColor(0, 200, 0), k2.bg);
    k2.gain = float(std::clamp(num(p, k::Gain, frame, 1.0), 0.0, 4.0));
    k2.balance = float(std::clamp(num(p, k::KeyBalance, frame, 0.5), 0.0, 1.0));
    k2.soft = float(std::clamp(num(p, k::PreSoft, frame, 0.0), 0.0, 1.0));
    k2.levels[0] = float(std::clamp(num(p, k::UkRedLevel, frame, 1.0), 0.0, 2.0));
    k2.levels[1] = float(std::clamp(num(p, k::UkGreenLevel, frame, 1.0), 0.0, 2.0));
    k2.levels[2] = float(std::clamp(num(p, k::UkBlueLevel, frame, 1.0), 0.0, 2.0));
    k2.separation = float(std::clamp(num(p, k::MatteSeparation, frame, 0.0), 0.0, 1.0));
    k2.preBlur = std::max(0.0, num(p, k::PreBlur, frame, 0.0));
    k2.closeSize = std::max(0.0, num(p, k::PreMatteSize, frame, 0.0));
    k2.pre.erode = num(p, k::PreMatteErode, frame, 0.0);
    k2.pre.blur = std::max(0.0, num(p, k::PreMatteBlur, frame, 0.0));
    k2.matte = matteFilterParams(p, frame);
    k2.spill = spillmethods::index(p.staticValue(k::SpillMethod, spillmethods::Medium).toString());
    k2.despill = float(std::clamp(num(p, k::Despill, frame, 1.0), 0.0, 1.0));
    k2.fringeGamma = float(std::clamp(num(p, k::FringeGamma, frame, 1.0), 0.1, 10.0));
    k2.fringeSize = float(std::clamp(num(p, k::FringeSize, frame, 0.0), -1.0, 1.0));
    k2.fringeShape = float(std::clamp(num(p, k::FringeShape, frame, 0.0), -1.0, 1.0));
    k2.fringeColor[0] = float(std::clamp(num(p, k::FringeCyanRed, frame, 0.0), -1.0, 1.0));
    k2.fringeColor[1] = float(std::clamp(num(p, k::FringeMagentaGreen, frame, 0.0), -1.0, 1.0));
    k2.fringeColor[2] = float(std::clamp(num(p, k::FringeYellowBlue, frame, 0.0), -1.0, 1.0));
    k2.replace = replacemodes::index(p.staticValue(k::ReplaceMode, replacemodes::None).toString());
    colorOf(p, k::ReplaceColor, QColor(128, 128, 128), k2.replaceColor);
    k2.subtractBackground = p.staticValue(k::SubtractBackground, false).toBool();
    k2.solidInvert = p.staticValue(k::SolidInvert, false).toBool();
    k2.garbageInvert = p.staticValue(k::GarbageInvert, false).toBool();
    k2.view = viewmodes::index(p.staticValue(k::ViewMode, viewmodes::Final).toString());
    return k2;
}

float keyMatte(const KeyerParams& k, const float c[3], const float bg[3])
{
    const int d = dominant(bg);
    const float ref = std::max(bg[d] - others(k, bg, d), 1e-3f);
    float screen = (c[d] - others(k, c, d)) / ref;
    if (k.kind == DeltaKind)
        screen *= k.gain;
    float m = 1.0f - clamp01(screen);
    if (k.kind == DeltaKind) {
        m = m + (m * m * (3.0f - 2.0f * m) - m) * k.soft;
    } else {
        m = clamp01((m - 0.5f) * (1.0f + 4.0f * k.separation) + 0.5f);
    }
    return m;
}

void keyColor(const KeyerParams& k, const float src[3], float m, const float bg[3], float out[3])
{
    float c[3] = {src[0], src[1], src[2]};
    if (k.subtractBackground && m < 1.0f && m > 1e-3f) {
        for (int i = 0; i < 3; ++i)
            c[i] = clamp01((c[i] - (1.0f - m) * bg[i]) / m);
    }
    if (k.spill != 0) {
        const int d = dominant(bg);
        const int o1 = (d + 1) % 3, o2 = (d + 2) % 3;
        float limit;
        switch (k.spill) {
        case 1: // Rare
            limit = std::max(c[o1], c[o2]);
            break;
        case 2: // Medium: the key's own balance
            limit = k.kind == UltraKind ? 0.5f * (std::max(c[o1], c[o2]) + std::min(c[o1], c[o2]))
                                        : others(k, c, d);
            break;
        case 3: // Well Done
            limit = std::min(c[o1], c[o2]);
            break;
        default: // Burnt
            limit = std::min(c[o1], c[o2]) * 0.9f;
            break;
        }
        c[d] = c[d] + (std::min(c[d], limit) - c[d]) * k.despill;
    }
    // Fringe: a window over the semi-transparent matte values.
    const float h = std::clamp(0.5f + 0.5f * k.fringeSize, 0.05f, 1.0f);
    const float peak = 0.5f + 0.25f * k.fringeShape;
    const float w = clamp01(1.0f - std::abs(m - peak) / h);
    if (k.fringeGamma != 1.0f) {
        for (float& v : c)
            v += (std::pow(clamp01(v), 1.0f / k.fringeGamma) - v) * w;
    }
    for (int i = 0; i < 3; ++i)
        c[i] += k.fringeColor[i] * 0.5f * w;
    if (k.replace == 1) { // Source
        for (int i = 0; i < 3; ++i)
            c[i] += (src[i] - c[i]) * (1.0f - m);
    } else if (k.replace == 2) { // Hard Color
        const float t = 1.0f - std::clamp((m - 0.98f) / 0.02f, 0.0f, 1.0f);
        for (int i = 0; i < 3; ++i)
            c[i] += (k.replaceColor[i] - c[i]) * t;
    } else if (k.replace == 3) { // Soft Color
        for (int i = 0; i < 3; ++i)
            c[i] += (k.replaceColor[i] - c[i]) * (1.0f - m);
    }
    for (int i = 0; i < 3; ++i)
        out[i] = clamp01(c[i]);
}

float applyMattes(const KeyerParams& k, float m, const float* solidAlpha, const float* garbageAlpha)
{
    if (solidAlpha) {
        const float s = k.solidInvert ? 1.0f - *solidAlpha : *solidAlpha;
        m = std::max(m, s);
    }
    if (garbageAlpha) {
        const float g = k.garbageInvert ? 1.0f - *garbageAlpha : *garbageAlpha;
        m *= 1.0f - g;
    }
    return m;
}

FImage keyer(const FImage& src, const FImage* plate, const FImage* garbage, const FImage* solid,
    const KeyerParams& k, double scale)
{
    const FImage blurred = k.preBlur > 0
        ? gaussian(src, k.preBlur * scale * 0.5, k.preBlur * scale * 0.5)
        : FImage();
    const FImage& read = k.preBlur > 0 ? blurred : src;
    const auto bgAt = [&](int x, int y, float bg[3]) {
        std::copy_n(k.bg, 3, bg);
        if (plate) {
            const float* p = plate->at(x, y);
            if (p[3] > 0) {
                for (int i = 0; i < 3; ++i)
                    bg[i] = p[i] / p[3];
            }
        }
    };
    FImage m0 = grey(src.w, src.h);
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            const float* t = read.at(x, y);
            float v = 0;
            if (t[3] > 0) {
                const float c[3] = {t[0] / t[3], t[1] / t[3], t[2] / t[3]};
                float bg[3];
                bgAt(x, y, bg);
                v = keyMatte(k, c, bg);
            }
            setGrey(m0.at(x, y), v);
        }
    }
    FImage pre = matteFilter(m0, k.pre, scale);
    if (k.closeSize > 0) {
        const FImage closed = gaussian(pre, k.closeSize * scale * 0.5, k.closeSize * scale * 0.5);
        for (size_t i = 0; i < pre.d.size(); ++i)
            pre.d[i] = std::max(pre.d[i], closed.d[i]);
    }
    FImage out(src.w, src.h);
    if (k.view == 1) {
        for (int y = 0; y < src.h; ++y)
            for (int x = 0; x < src.w; ++x) {
                float* o = out.at(x, y);
                o[0] = o[1] = o[2] = pre.at(x, y)[3];
                o[3] = 1.0f;
            }
        return out;
    }
    const FImage final = matteFilter(pre, k.matte, scale);
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            const float* s = src.at(x, y);
            float sa = solid ? solid->at(x, y)[3] : 0, ga = garbage ? garbage->at(x, y)[3] : 0;
            const float m = clamp01(applyMattes(
                k, final.at(x, y)[3], solid ? &sa : nullptr, garbage ? &ga : nullptr));
            float* o = out.at(x, y);
            if (k.view == 2) {
                o[0] = o[1] = o[2] = m;
                o[3] = 1.0f;
                continue;
            }
            if (s[3] <= 0)
                continue;
            const float sc[3] = {s[0] / s[3], s[1] / s[3], s[2] / s[3]};
            float bg[3], c[3];
            bgAt(x, y, bg);
            keyColor(k, sc, m, bg, c);
            const float a = s[3] * m;
            o[0] = c[0] * a;
            o[1] = c[1] * a;
            o[2] = c[2] * a;
            o[3] = a;
        }
    }
    return out;
}

// ---- Matte Control ----

MatteControlParams matteControlParams(const ParamSet& p, int64_t frame)
{
    MatteControlParams m;
    m.matte = matteFilterParams(p, frame);
    m.solidInvert = p.staticValue(k::SolidInvert, false).toBool();
    m.garbageInvert = p.staticValue(k::GarbageInvert, false).toBool();
    return m;
}

FImage matteControl(const FImage& src, const FImage* garbage, const FImage* solid,
    const MatteControlParams& m, double scale)
{
    FImage a = grey(src.w, src.h);
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x)
            setGrey(a.at(x, y), src.at(x, y)[3]);
    const FImage f = matteFilter(a, m.matte, scale);
    KeyerParams kp;
    kp.solidInvert = m.solidInvert;
    kp.garbageInvert = m.garbageInvert;
    FImage out(src.w, src.h);
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            const float* s = src.at(x, y);
            float sa = solid ? solid->at(x, y)[3] : 0, ga = garbage ? garbage->at(x, y)[3] : 0;
            const float v = clamp01(applyMattes(
                kp, f.at(x, y)[3], solid ? &sa : nullptr, garbage ? &ga : nullptr));
            float* o = out.at(x, y);
            if (s[3] > 0) {
                for (int i = 0; i < 3; ++i)
                    o[i] = s[i] / s[3] * v;
            }
            o[3] = v;
        }
    }
    return out;
}

} // namespace kk::render::compmath
