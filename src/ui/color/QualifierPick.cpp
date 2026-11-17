// SPDX-License-Identifier: GPL-3.0-or-later
// QualifierPick — HSL ranges from picked colours.
#include "ui/color/QualifierPick.h"

#include "core/GradeMath.h"

#include <algorithm>
#include <cmath>

namespace kk::qualifierpick {

namespace {

double wrap01(double h)
{
    return h - std::floor(h);
}

/// Signed hue difference a - b in (-0.5, 0.5].
double hueDelta(double a, double b)
{
    double d = wrap01(a - b + 0.5) - 0.5;
    return d;
}

/// Edits one range after picking @p values (0..1): Pick sets it around them, Add widens it,
/// Remove moves the nearer end past each value that is inside.
void pickRange(double& low, double& high, double soft, const QVector<double>& values, Mode mode)
{
    double lo = 1, hi = 0;
    for (const double v : values) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    switch (mode) {
    case Mode::Pick:
        low = std::max(0.0, lo - kRangeMargin);
        high = std::min(1.0, hi + kRangeMargin);
        break;
    case Mode::Add:
        low = std::max(0.0, std::min(low, lo - kRangeMargin));
        high = std::min(1.0, std::max(high, hi + kRangeMargin));
        break;
    case Mode::Remove:
        for (const double v : values) {
            if (v < low || v > high)
                continue;
            const double gap = kRangeMargin / 2 + soft;
            if (v - low < high - v)
                low = std::min(high, v + gap);
            else
                high = std::max(low, v - gap);
        }
        break;
    }
}

} // namespace

Hsl toHsl(const float rgb[3])
{
    const float c[3] = {std::clamp(rgb[0], 0.0f, 1.0f), std::clamp(rgb[1], 0.0f, 1.0f),
        std::clamp(rgb[2], 0.0f, 1.0f)};
    Hsl hsl {};
    grademath::rgbToHsl(c, hsl.data());
    return hsl;
}

std::pair<double, double> hueArc(QVector<double> hues)
{
    if (hues.isEmpty())
        return {0.0, 0.0};
    for (double& h : hues)
        h = wrap01(h);
    std::sort(hues.begin(), hues.end());
    // The arc is the complement of the largest gap between neighbouring hues (circularly).
    double bestGap = -1.0;
    int bestAfter = 0; // the arc starts at hues[bestAfter]
    for (int i = 0; i < hues.size(); ++i) {
        const double next = i + 1 < hues.size() ? hues[i + 1] : hues[0] + 1.0;
        const double gap = next - hues[i];
        if (gap > bestGap) {
            bestGap = gap;
            bestAfter = (i + 1) % int(hues.size());
        }
    }
    const double start = hues[bestAfter];
    const double width = std::max(0.0, 1.0 - bestGap);
    return {wrap01(start + width / 2), width};
}

GradeQualifier apply(const GradeQualifier& current, const QVector<Hsl>& samples, Mode mode)
{
    if (samples.isEmpty())
        return current;
    GradeQualifier q = current;
    q.enabled = true;
    double sLo = 1, sHi = 0, lLo = 1, lHi = 0;
    QVector<double> hues;
    for (const Hsl& s : samples) {
        hues.append(s[0]);
        sLo = std::min(sLo, double(s[1]));
        sHi = std::max(sHi, double(s[1]));
        lLo = std::min(lLo, double(s[2]));
        lHi = std::max(lHi, double(s[2]));
    }
    switch (mode) {
    case Mode::Pick: {
        const auto [centre, width] = hueArc(hues);
        q.hueCenter = centre;
        q.hueWidth = std::min(1.0, width + 2 * kHueMargin);
        q.satLow = std::max(0.0, sLo - kRangeMargin);
        q.satHigh = std::min(1.0, sHi + kRangeMargin);
        q.lumLow = std::max(0.0, lLo - kRangeMargin);
        q.lumHigh = std::min(1.0, lHi + kRangeMargin);
        break;
    }
    case Mode::Add: {
        // The current arc's two ends join the samples; inner hues keep it from flipping.
        const double half = current.hueWidth / 2;
        QVector<double> all = hues;
        for (int i = 0; i <= 4; ++i)
            all.append(current.hueCenter - half + current.hueWidth * i / 4.0);
        const auto [centre, width] = hueArc(all);
        // Hues outside the old arc get the pick margin.
        q.hueCenter = centre;
        q.hueWidth = std::min(1.0, width + kHueMargin);
        q.satLow = std::max(0.0, std::min(current.satLow, sLo - kRangeMargin));
        q.satHigh = std::min(1.0, std::max(current.satHigh, sHi + kRangeMargin));
        q.lumLow = std::max(0.0, std::min(current.lumLow, lLo - kRangeMargin));
        q.lumHigh = std::min(1.0, std::max(current.lumHigh, lHi + kRangeMargin));
        break;
    }
    case Mode::Remove: {
        for (const Hsl& s : samples) {
            // How far inside each enabled range the sample is (relative to its width); the
            // range it is nearest to leaving is the one that gives way.
            struct Cut
            {
                int channel;
                double depth;
            };
            QVector<Cut> cuts;
            const double hd = hueDelta(s[0], q.hueCenter);
            const double half = q.hueWidth / 2;
            if (q.useHue && std::abs(hd) <= half && q.hueWidth > 0)
                cuts.append({0, (half - std::abs(hd)) / q.hueWidth});
            if (q.useSat && s[1] >= q.satLow && s[1] <= q.satHigh && q.satHigh > q.satLow)
                cuts.append(
                    {1, std::min(s[1] - q.satLow, q.satHigh - s[1]) / (q.satHigh - q.satLow)});
            if (q.useLum && s[2] >= q.lumLow && s[2] <= q.lumHigh && q.lumHigh > q.lumLow)
                cuts.append(
                    {2, std::min(s[2] - q.lumLow, q.lumHigh - s[2]) / (q.lumHigh - q.lumLow)});
            if (cuts.isEmpty())
                continue; // already outside
            const Cut cut = *std::min_element(cuts.begin(), cuts.end(),
                [](const Cut& a, const Cut& b) { return a.depth < b.depth; });
            if (cut.channel == 0) {
                // Move the nearer hue edge just past the sample.
                const double lo = q.hueCenter - half;
                const double hi = q.hueCenter + half;
                double newLo = lo, newHi = hi;
                const double at = q.hueCenter + hd;
                // Past the sample by the margin and the soft edge, so it ends up unselected.
                const double gap = kHueMargin + q.hueSoft;
                if (hd < 0)
                    newLo = at + gap;
                else
                    newHi = at - gap;
                if (newHi < newLo)
                    newHi = newLo = (newLo + newHi) / 2;
                q.hueCenter = wrap01((newLo + newHi) / 2);
                q.hueWidth = std::max(0.0, newHi - newLo);
            } else {
                double& low = cut.channel == 1 ? q.satLow : q.lumLow;
                double& high = cut.channel == 1 ? q.satHigh : q.lumHigh;
                const double v = s[size_t(cut.channel)];
                const double gap = kRangeMargin / 2 + (cut.channel == 1 ? q.satSoft : q.lumSoft);
                if (v - low < high - v)
                    low = std::min(high, v + gap);
                else
                    high = std::max(low, v - gap);
            }
        }
        break;
    }
    }
    return q;
}

GradeQualifier applyRgb(const GradeQualifier& current, const QVector<Rgb>& samples, Mode mode)
{
    if (samples.isEmpty())
        return current;
    GradeQualifier q = current;
    q.enabled = true;
    QVector<double> v[3];
    for (const Rgb& s : samples) {
        for (int c = 0; c < 3; ++c)
            v[c].append(std::clamp(s[size_t(c)], 0.0f, 1.0f));
    }
    pickRange(q.redLow, q.redHigh, q.redSoft, v[0], mode);
    pickRange(q.greenLow, q.greenHigh, q.greenSoft, v[1], mode);
    pickRange(q.blueLow, q.blueHigh, q.blueSoft, v[2], mode);
    return q;
}

GradeQualifier applyLuma(const GradeQualifier& current, const QVector<Rgb>& samples, Mode mode)
{
    if (samples.isEmpty())
        return current;
    GradeQualifier q = current;
    q.enabled = true;
    QVector<double> luma;
    for (const Rgb& s : samples)
        luma.append(grademath::luma(s.data()));
    pickRange(q.lumLow, q.lumHigh, q.lumSoft, luma, mode);
    return q;
}

GradeQualifier apply3d(const GradeQualifier& current, const QVector<Rgb>& samples, Mode mode)
{
    if (samples.isEmpty())
        return current;
    GradeQualifier q = current;
    q.enabled = true;
    if (mode == Mode::Pick)
        q.colors.clear();
    constexpr float same = 0.04f; // RGB distance under which two picks are one colour
    for (const Rgb& s : samples) {
        GradeQualifierColor c {std::clamp(s[0], 0.0f, 1.0f), std::clamp(s[1], 0.0f, 1.0f),
            std::clamp(s[2], 0.0f, 1.0f), mode == Mode::Remove};
        const bool known = std::any_of(q.colors.cbegin(), q.colors.cend(),
            [&](const GradeQualifierColor& o) {
                return o.subtract == c.subtract
                    && std::hypot(o.r - c.r, o.g - c.g, o.b - c.b) < same;
            });
        if (!known)
            q.colors.append(c);
    }
    while (q.colors.size() > kMaxQualifierColors)
        q.colors.removeFirst();
    return q;
}

GradeQualifier applyForMode(const GradeQualifier& current, const QVector<Rgb>& samples, Mode mode)
{
    switch (current.mode) {
    case QualifierMode::Rgb:
        return applyRgb(current, samples, mode);
    case QualifierMode::Luma:
        return applyLuma(current, samples, mode);
    case QualifierMode::ThreeD:
        return apply3d(current, samples, mode);
    case QualifierMode::Hsl:
        break;
    }
    QVector<Hsl> hsl;
    for (const Rgb& s : samples)
        hsl.append(toHsl(s.data()));
    return apply(current, hsl, mode);
}

} // namespace kk::qualifierpick
