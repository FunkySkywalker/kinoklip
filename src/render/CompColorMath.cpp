// SPDX-License-Identifier: GPL-3.0-or-later
// CompColorMath implementation, see the header. The Brightness Contrast chain, the Channel
// Mapper matrix and the Custom Tool follow the controls of the public Fusion reference manual in
// outline; the curve nodes reuse the Color page curve tables (core/GradeMath), so both pages
// bend a picture the same way.
#include "render/CompColorMath.h"

#include "core/CurveParam.h"
#include "core/NodeRegistry.h"

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

} // namespace

// ---- Brightness Contrast ----

bool BrightnessContrastParams::isIdentity() const
{
    return gain == 1 && lift == 0 && gamma == 1 && contrast == 0 && brightness == 0
        && saturation == 1 && low == 0 && high == 1;
}

BrightnessContrastParams brightnessContrastParams(const ParamSet& p, int64_t frame)
{
    BrightnessContrastParams b;
    b.channel[0] = p.staticValue(k::BcRed, true).toBool();
    b.channel[1] = p.staticValue(k::BcGreen, true).toBool();
    b.channel[2] = p.staticValue(k::BcBlue, true).toBool();
    b.channel[3] = p.staticValue(k::BcAlpha, false).toBool();
    b.gain = float(std::clamp(num(p, k::BcGain, frame, 1.0), 0.0, 10.0));
    b.lift = float(std::clamp(num(p, k::BcLift, frame, 0.0), -1.0, 1.0));
    b.gamma = float(std::clamp(num(p, k::BcGamma, frame, 1.0), 0.1, 10.0));
    b.contrast = float(std::clamp(num(p, k::BcContrast, frame, 0.0), -1.0, 4.0));
    b.brightness = float(std::clamp(num(p, k::BcBrightness, frame, 0.0), -1.0, 1.0));
    b.saturation = float(std::clamp(num(p, k::Saturation, frame, 1.0), 0.0, 4.0));
    b.low = float(std::clamp(num(p, k::BcLow, frame, 0.0), 0.0, 1.0));
    b.high = float(std::clamp(num(p, k::BcHigh, frame, 1.0), 0.0, 1.0));
    return b;
}

float brightnessContrastChannel(const BrightnessContrastParams& b, float v)
{
    const float range = std::max(b.high - b.low, 0.001f);
    v = (v - b.low) / range;
    v *= b.gain;
    v += b.lift * (1.0f - v);
    v = std::pow(std::max(v, 0.0f), 1.0f / b.gamma);
    v = (v - 0.5f) * (1.0f + b.contrast) + 0.5f;
    return v + b.brightness;
}

void applyBrightnessContrast(const BrightnessContrastParams& b, float rgba[4])
{
    for (int i = 0; i < 3; ++i) {
        if (b.channel[i])
            rgba[i] = brightnessContrastChannel(b, rgba[i]);
    }
    if (b.saturation != 1.0f) {
        const float y = 0.2126f * rgba[0] + 0.7152f * rgba[1] + 0.0722f * rgba[2];
        for (int i = 0; i < 3; ++i)
            rgba[i] = y + (rgba[i] - y) * b.saturation;
    }
    if (b.channel[3])
        rgba[3] = brightnessContrastChannel(b, rgba[3]);
    rgba[3] = clamp01(rgba[3]);
}

// ---- Alpha Multiply / Divide ----

void alphaMultiply(float rgba[4])
{
    for (int i = 0; i < 3; ++i)
        rgba[i] *= rgba[3];
}

void alphaDivide(float rgba[4])
{
    if (rgba[3] <= 0.0f)
        return;
    for (int i = 0; i < 3; ++i)
        rgba[i] = std::min(rgba[i] / rgba[3], 1.0f);
}

// ---- Channel Mapper ----

bool ChannelMapperParams::isIdentity() const
{
    for (int o = 0; o < 4; ++o) {
        if (offset[o] != 0.0f)
            return false;
        for (int i = 0; i < 4; ++i) {
            if (m[o][i] != (o == i ? 1.0f : 0.0f))
                return false;
        }
    }
    return true;
}

ChannelMapperParams channelMapperParams(const ParamSet& p, int64_t frame)
{
    ChannelMapperParams c;
    for (int o = 0; o < 4; ++o) {
        c.offset[o] = float(std::clamp(num(p, k::mapperOffsetKey(o), frame, 0.0), -1.0, 1.0));
        for (int i = 0; i < 4; ++i) {
            c.m[o][i] = float(std::clamp(
                num(p, k::mapperKey(o, i), frame, o == i ? 1.0 : 0.0), -4.0, 4.0));
        }
    }
    return c;
}

void applyChannelMapper(const ChannelMapperParams& c, float rgba[4])
{
    const float in[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};
    for (int o = 0; o < 4; ++o) {
        float v = c.offset[o];
        for (int i = 0; i < 4; ++i)
            v += c.m[o][i] * in[i];
        rgba[o] = v;
    }
    rgba[3] = clamp01(rgba[3]);
}

// ---- Color Curves and Hue Curves ----

namespace {

CurvePoints gradePoints(const ParamSet& p, const QString& key, bool flat = false)
{
    CurvePoints out;
    // A node saved before the curve existed has no string: the control's default.
    const QVariant v = p.staticValue(key);
    const QString text = v.isValid() ? v.toString()
                                     : (flat ? QStringLiteral("0,0.5 1,0.5") : curve::identityText());
    for (const curve::Point& pt : curve::parse(text))
        out.append(CurvePoint {pt.x, pt.y});
    return out;
}

} // namespace

CurvesNodeParams colorCurvesParams(const ParamSet& p)
{
    GradeCurves g;
    g[GradeCurve::Luma] = gradePoints(p, k::CurveLuma);
    g[GradeCurve::Red] = gradePoints(p, k::CurveRed);
    g[GradeCurve::Green] = gradePoints(p, k::CurveGreen);
    g[GradeCurve::Blue] = gradePoints(p, k::CurveBlue);
    CurvesNodeParams c;
    c.tables = grademath::CurveTables::bake(g);
    const curve::Points alpha = curve::parse(p.staticValue(k::CurveAlpha).toString());
    c.alphaOn = !curve::isIdentity(alpha);
    curve::sample(alpha, c.alpha.data(), grademath::kCurveTableSize);
    if (!c.alphaOn) {
        for (int i = 0; i < grademath::kCurveTableSize; ++i)
            c.alpha[size_t(i)] = float(i) / float(grademath::kCurveTableSize - 1);
    }
    return c;
}

CurvesNodeParams hueCurvesParams(const ParamSet& p)
{
    GradeCurves g;
    g[GradeCurve::HueHue] = gradePoints(p, k::CurveHueHue, true);
    g[GradeCurve::HueSat] = gradePoints(p, k::CurveHueSat, true);
    g[GradeCurve::HueLum] = gradePoints(p, k::CurveHueLum, true);
    g[GradeCurve::LumSat] = gradePoints(p, k::CurveLumSat, true);
    g[GradeCurve::SatSat] = gradePoints(p, k::CurveSatSat, true);
    CurvesNodeParams c;
    c.tables = grademath::CurveTables::bake(g);
    for (int i = 0; i < grademath::kCurveTableSize; ++i)
        c.alpha[size_t(i)] = float(i) / float(grademath::kCurveTableSize - 1);
    return c;
}

void applyCurvesNode(const CurvesNodeParams& c, float rgba[4])
{
    grademath::applyCurves(c.tables, rgba);
    if (c.alphaOn) {
        const float pos = clamp01(rgba[3]) * float(grademath::kCurveTableSize - 1);
        const int i = std::min(int(pos), grademath::kCurveTableSize - 2);
        const float f = pos - float(i);
        rgba[3] = c.alpha[size_t(i)] + (c.alpha[size_t(i) + 1] - c.alpha[size_t(i)]) * f;
    }
}

// ---- Custom Tool ----

const QStringList& customToolVariables()
{
    static const QStringList names = [] {
        QStringList n {QStringLiteral("time"), QStringLiteral("x"), QStringLiteral("y"),
            QStringLiteral("w"), QStringLiteral("h")};
        for (int i = 1; i <= kCustomNumbers; ++i)
            n.append(QStringLiteral("n%1").arg(i));
        for (int img = 1; img <= 2; ++img) {
            for (const char* c : {"r", "g", "b", "a"})
                n.append(QStringLiteral("%1%2").arg(QLatin1String(c)).arg(img));
        }
        for (int i = 1; i <= kCustomInters; ++i)
            n.append(QStringLiteral("i%1").arg(i));
        return n;
    }();
    return names;
}

namespace {

// Slots of customToolVariables().
constexpr int kVarTime = 0, kVarX = 1, kVarY = 2, kVarW = 3, kVarH = 4, kVarN = 5;
constexpr int kVarImg1 = kVarN + kCustomNumbers; // r1 g1 b1 a1
constexpr int kVarImg2 = kVarImg1 + 4;
constexpr int kVarInter = kVarImg2 + 4;
constexpr int kVarCount = kVarInter + kCustomInters;

const QString kChannelDefault[4] = {QStringLiteral("r1"), QStringLiteral("g1"),
    QStringLiteral("b1"), QStringLiteral("a1")};

QString fieldKey(int field)
{
    switch (field) {
    case 0: case 1: case 2: case 3: return k::customInterKey(field);
    case 4: return k::CustomRed;
    case 5: return k::CustomGreen;
    case 6: return k::CustomBlue;
    default: return k::CustomAlpha;
    }
}

} // namespace

bool CustomToolParams::isIdentity() const
{
    return !channel[0] && !channel[1] && !channel[2] && !channel[3];
}

QString customToolText(const ParamSet& p, int field)
{
    const QVariant v = p.staticValue(fieldKey(field));
    if (field >= kCustomInters)
        return v.isValid() ? v.toString() : kChannelDefault[field - kCustomInters];
    return v.toString();
}

CustomToolParams customToolParams(const ParamSet& p, int64_t frame)
{
    CustomToolParams c;
    for (int i = 0; i < kCustomNumbers; ++i)
        c.n[i] = num(p, k::customNumberKey(i), frame, 0.0);
    for (int f = 0; f < kCustomInters + 4; ++f) {
        const QString text = customToolText(p, f).trimmed();
        std::shared_ptr<const expr::Program>& slot
            = f < kCustomInters ? c.inter[f] : c.channel[f - kCustomInters];
        if (text.isEmpty())
            continue; // an empty Intermediate is 0, an empty channel keeps Image 1
        QString why;
        slot = expr::Program::compilePixel(text, customToolVariables(), &why);
        if (!slot)
            c.error[f] = why;
    }
    // A channel that is only "r1" / "g1" / "b1" / "a1" is the input as it is.
    for (int i = 0; i < 4; ++i) {
        if (customToolText(p, kCustomInters + i).trimmed().compare(
                kChannelDefault[i], Qt::CaseInsensitive) == 0)
            c.channel[i].reset();
    }
    return c;
}

void customToolPixel(const CustomToolParams& c, double time, double x, double y, double w,
    double h, const float img1[4], const float img2[4], float out[4])
{
    double v[kVarCount] = {};
    v[kVarTime] = time;
    v[kVarX] = x;
    v[kVarY] = y;
    v[kVarW] = w;
    v[kVarH] = h;
    for (int i = 0; i < kCustomNumbers; ++i)
        v[kVarN + i] = c.n[i];
    for (int i = 0; i < 4; ++i) {
        v[kVarImg1 + i] = img1[i];
        v[kVarImg2 + i] = img2[i];
    }
    for (int i = 0; i < kCustomInters; ++i) {
        if (c.inter[i])
            v[kVarInter + i] = double(float(c.inter[i]->evaluatePixel(time, v)));
    }
    for (int i = 0; i < 4; ++i) {
        out[i] = c.channel[i] ? float(c.channel[i]->evaluatePixel(time, v)) : img1[i];
    }
    out[3] = clamp01(out[3]);
}

QString customToolGlsl(const CustomToolParams& c, const ParamSet& p)
{
    Q_UNUSED(p)
    const QStringList& vars = customToolVariables();
    QString s;
    for (int i = 0; i < kCustomInters; ++i) {
        s += QStringLiteral("    v_i%1 = %2;\n")
                 .arg(i + 1)
                 .arg(c.inter[i] ? c.inter[i]->toGlsl(vars) : QStringLiteral("0.0"));
    }
    const char* names[4] = {"r1", "g1", "b1", "a1"};
    for (int i = 0; i < 4; ++i) {
        s += QStringLiteral("    result[%1] = %2;\n")
                 .arg(i)
                 .arg(c.channel[i] ? c.channel[i]->toGlsl(vars)
                                   : QStringLiteral("v_%1").arg(QLatin1String(names[i])));
    }
    return s;
}

} // namespace kk::render::compmath
