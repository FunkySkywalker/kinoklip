// SPDX-License-Identifier: GPL-3.0-or-later
// Grade — key defaults, node helpers and the graph operations of the Color page node editor.
#include "core/Grade.h"

#include "core/GradeMath.h"

#include <QCoreApplication>
#include <QHash>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

/// Points closer than this to the neutral line count as neutral (floating point noise of
/// dragged points).
constexpr double kNeutralEpsilon = 1e-6;
/// Horizontal / vertical spacing of automatically placed nodes (node editor units).
constexpr double kNodeSpacingX = 140.0;
constexpr double kNodeSpacingY = 90.0;

} // namespace

bool GradeCurves::isIdentity(GradeCurve c) const
{
    const double neutral = curveNeutral(c);
    for (const CurvePoint& p : points[size_t(c)]) {
        const double want = neutral < 0 ? p.x : neutral;
        if (std::abs(p.y - want) > kNeutralEpsilon)
            return false;
    }
    return true;
}

bool GradeCurves::isIdentity() const
{
    for (int i = 0; i < kGradeCurveCount; ++i) {
        if (!isIdentity(GradeCurve(i)))
            return false;
    }
    return true;
}

namespace gradewindowkeys {

double defaultValue(QStringView key)
{
    if (key == CenterX || key == CenterY || key == Width || key == Height)
        return 0.5;
    if (key == SoftOut)
        return 0.05;
    if (key == Opacity)
        return 1.0;
    return 0.0;
}

const QVector<QString>& allKeys()
{
    static const QVector<QString> keys
        = {CenterX, CenterY, Width, Height, Rotation, SoftIn, SoftOut, Opacity};
    return keys;
}

QString label(const QString& key)
{
    const std::pair<const QString*, const char*> named[] = {
        {&CenterX, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Center X")},
        {&CenterY, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Center Y")},
        {&Width, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Width")},
        {&Height, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Height")},
        {&Rotation, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Rotation")},
        {&SoftIn, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Inside Softness")},
        {&SoftOut, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Outside Softness")},
        {&Opacity, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Opacity")},
    };
    for (const auto& [k, text] : named) {
        if (key == *k)
            return QCoreApplication::translate("kk::gradewindowkeys", text);
    }
    return key;
}

} // namespace gradewindowkeys

bool GradeWindow::isRenderable() const
{
    const bool pointShape = shape == GradeWindowShape::Polygon || shape == GradeWindowShape::Curve;
    return enabled && (!pointShape || points.size() >= 3);
}

GradeWindowShapePoints GradeWindow::shapeAt(int64_t localFrame) const
{
    if (shapeKeys.isEmpty())
        return points;
    auto after = shapeKeys.lowerBound(localFrame); // first key >= localFrame
    if (after == shapeKeys.cbegin() || (after != shapeKeys.cend() && after.key() == localFrame))
        return after.value();
    if (after == shapeKeys.cend())
        return std::prev(after).value();
    const auto before = std::prev(after);
    const GradeWindowShapePoints& a = before.value();
    const GradeWindowShapePoints& b = after.value();
    if (a.size() != b.size())
        return a;
    const double t = double(localFrame - before.key()) / double(after.key() - before.key());
    GradeWindowShapePoints out = a;
    for (int i = 0; i < out.size(); ++i) {
        out[i].pos = a[i].pos + (b[i].pos - a[i].pos) * t;
        out[i].inHandle = a[i].inHandle + (b[i].inHandle - a[i].inHandle) * t;
        out[i].outHandle = a[i].outHandle + (b[i].outHandle - a[i].outHandle) * t;
    }
    return out;
}

GradeWindowPoint smoothedPoint(const GradeWindowShapePoints& points, int index)
{
    GradeWindowPoint p = points.value(index);
    const int n = int(points.size());
    if (n < 2)
        return p;
    const QPointF t = (points[(index + 1) % n].pos - points[(index + n - 1) % n].pos) / 6.0;
    p.outHandle = t;
    p.inHandle = -t;
    p.smooth = true;
    return p;
}

bool insertWindowPoint(GradeWindowShapePoints& points, int index, double t)
{
    const int n = int(points.size());
    if (index < 0 || index >= n || n >= kMaxWindowPoints)
        return false;
    t = std::clamp(t, 0.0, 1.0);
    GradeWindowPoint& a = points[index];
    GradeWindowPoint& b = points[(index + 1) % n];
    auto lerp = [](QPointF x, QPointF y, double k) { return x + (y - x) * k; };
    GradeWindowPoint mid;
    if (!a.hasHandles() && !b.hasHandles()) {
        mid.pos = lerp(a.pos, b.pos, t);
    } else {
        const QPointF p0 = a.pos, c0 = a.pos + a.outHandle, c1 = b.pos + b.inHandle, p1 = b.pos;
        const QPointF q0 = lerp(p0, c0, t), q1 = lerp(c0, c1, t), q2 = lerp(c1, p1, t);
        const QPointF r0 = lerp(q0, q1, t), r1 = lerp(q1, q2, t);
        const QPointF s = lerp(r0, r1, t);
        a.outHandle = q0 - p0;
        b.inHandle = q2 - p1;
        mid = {s, r0 - s, r1 - s, true};
    }
    points.insert(index + 1, mid);
    return true;
}

QPointF windowSpanPoint(const GradeWindowShapePoints& points, int index, double t)
{
    const int n = int(points.size());
    if (n == 0)
        return {};
    const GradeWindowPoint& a = points[((index % n) + n) % n];
    const GradeWindowPoint& b = points[(index + 1 + n) % n];
    const double u = 1.0 - t;
    return u * u * u * a.pos + 3 * u * u * t * (a.pos + a.outHandle)
        + 3 * u * t * t * (b.pos + b.inHandle) + t * t * t * b.pos;
}

QString gradeWindowShapeToString(GradeWindowShape shape)
{
    switch (shape) {
    case GradeWindowShape::Circle:
        return QStringLiteral("circle");
    case GradeWindowShape::Linear:
        return QStringLiteral("linear");
    case GradeWindowShape::Polygon:
        return QStringLiteral("polygon");
    case GradeWindowShape::Gradient:
        return QStringLiteral("gradient");
    case GradeWindowShape::Curve:
        return QStringLiteral("curve");
    }
    return QStringLiteral("circle");
}

GradeWindowShape gradeWindowShapeFromString(const QString& text)
{
    if (text == QLatin1String("linear"))
        return GradeWindowShape::Linear;
    if (text == QLatin1String("polygon"))
        return GradeWindowShape::Polygon;
    if (text == QLatin1String("gradient"))
        return GradeWindowShape::Gradient;
    if (text == QLatin1String("curve"))
        return GradeWindowShape::Curve;
    return GradeWindowShape::Circle;
}

std::pair<int, int> warpGridSize(WarpGrid grid, int resolution)
{
    static const std::pair<int, int> hueSat[kWarpResolutions] = {{8, 4}, {12, 5}, {24, 8}};
    static const std::pair<int, int> chromaLuma[kWarpResolutions] = {{4, 4}, {5, 5}, {8, 8}};
    const int r = std::clamp(resolution, 0, kWarpResolutions - 1);
    return grid == WarpGrid::HueSat ? hueSat[r] : chromaLuma[r];
}

bool GradeWarp::isIdentity(WarpGrid grid) const
{
    for (const float v : pins(grid)) {
        if (v != 0.0f)
            return false;
    }
    return true;
}

bool GradeWarp::isIdentity() const
{
    return isIdentity(WarpGrid::HueSat) && isIdentity(WarpGrid::ChromaLuma);
}

std::pair<float, float> GradeWarp::pin(WarpGrid grid, int col, int row) const
{
    const auto [cols, rows] = warpGridSize(grid, resolution);
    const QVector<float>& v = pins(grid);
    const int i = (row * cols + col) * 2;
    if (col < 0 || row < 0 || col >= cols || row >= rows || i + 1 >= v.size())
        return {0.0f, 0.0f};
    return {v[i], v[i + 1]};
}

void GradeWarp::setPin(WarpGrid grid, int col, int row, float dx, float dy)
{
    const auto [cols, rows] = warpGridSize(grid, resolution);
    if (col < 0 || row < 0 || col >= cols || row >= rows)
        return;
    QVector<float>& v = pins(grid);
    if (v.size() != cols * rows * 2)
        v = QVector<float>(cols * rows * 2, 0.0f);
    v[(row * cols + col) * 2] = dx;
    v[(row * cols + col) * 2 + 1] = dy;
    if (isIdentity(grid))
        v.clear();
}

namespace gradekeys {

const QString& wheelKey(Wheel wheel, int channel)
{
    static const QString* const table[kWheelCount][4] = {
        {&LiftR, &LiftG, &LiftB, &LiftY},
        {&GammaR, &GammaG, &GammaB, &GammaY},
        {&GainR, &GainG, &GainB, &GainY},
        {&OffsetR, &OffsetG, &OffsetB, &OffsetY},
        {&LogShadowR, &LogShadowG, &LogShadowB, &LogShadowY},
        {&LogMidR, &LogMidG, &LogMidB, &LogMidY},
        {&LogHighR, &LogHighG, &LogHighB, &LogHighY},
        {&LogOffsetR, &LogOffsetG, &LogOffsetB, &LogOffsetY},
        {&HdrGlobalR, &HdrGlobalG, &HdrGlobalB, &HdrGlobalY},
        {&HdrShadowR, &HdrShadowG, &HdrShadowB, &HdrShadowY},
        {&HdrLightR, &HdrLightG, &HdrLightB, &HdrLightY},
        {&HdrHighlightR, &HdrHighlightG, &HdrHighlightB, &HdrHighlightY},
        {&HdrSpecularR, &HdrSpecularG, &HdrSpecularB, &HdrSpecularY},
        {&HdrBlackR, &HdrBlackG, &HdrBlackB, &HdrBlackY},
    };
    return *table[std::clamp(int(wheel), 0, kWheelCount - 1)][std::clamp(channel, 0, 3)];
}

double defaultValue(QStringView key)
{
    if (key.startsWith(u"gain"))
        return 1.0;
    if (key == Contrast || key == Saturation || key == LumMix || key == Mix)
        return 1.0;
    if (key == Pivot)
        return 0.435; // Resolve's default contrast pivot (log-ish mid grey)
    if (key == SoftClipHigh)
        return 1.0;
    if (key == LogLowRange)
        return 0.33; // Resolve's default Low Range of the Log palette
    if (key == LogHighRange)
        return 0.55; // and High Range
    if (key.startsWith(u"agx"))
        return 0.0;
    if (key.startsWith(u"hdr")) {
        if (key.endsWith(u"Sat"))
            return 1.0;
        if (key == HdrShadowRange)
            return -1.5;
        if (key == HdrLightRange)
            return 1.0;
        if (key == HdrHighlightRange)
            return 3.0;
        if (key.endsWith(u"Falloff"))
            return 1.5;
        return 0.0;
    }
    if (key.startsWith(u"mixer")) {
        if (key == MixerRR || key == MixerGG || key == MixerBB)
            return 1.0;
        if (key == MixerMonoR)
            return 0.3;
        if (key == MixerMonoG)
            return 0.59;
        if (key == MixerMonoB)
            return 0.11;
        return 0.0;
    }
    if (key.startsWith(u"blur") || key.startsWith(u"sharp") || key.startsWith(u"mist")) {
        if (key == SharpCoring || key == MistRadius)
            return 0.0;
        return 0.5; // radii, ratios, scalings, sharpen level, mist mix
    }
    return 0.0;
}

bool isLogKey(QStringView key)
{
    return key.startsWith(u"log");
}

const QVector<QString>& hdrKeys()
{
    static const QVector<QString> keys = {HdrGlobalR, HdrGlobalG, HdrGlobalB, HdrGlobalY,
        HdrShadowR, HdrShadowG, HdrShadowB, HdrShadowY, HdrLightR, HdrLightG, HdrLightB,
        HdrLightY, HdrHighlightR, HdrHighlightG, HdrHighlightB, HdrHighlightY, HdrSpecularR,
        HdrSpecularG, HdrSpecularB, HdrSpecularY, HdrBlackR, HdrBlackG, HdrBlackB, HdrBlackY,
        HdrGlobalSat, HdrShadowSat, HdrLightSat, HdrHighlightSat, HdrSpecularSat,
        HdrShadowRange, HdrShadowFalloff, HdrLightRange, HdrLightFalloff, HdrHighlightRange,
        HdrHighlightFalloff};
    return keys;
}

bool isHdrKey(QStringView key)
{
    return key.startsWith(u"hdr");
}

bool isHdrZoneKey(QStringView key)
{
    return key.endsWith(u"Range") || key.endsWith(u"Falloff");
}

const QVector<QString>& blurKeys()
{
    static const QVector<QString> keys = {BlurRadiusR, BlurRadiusG, BlurRadiusB, BlurRatio,
        BlurScaling, SharpRadiusR, SharpRadiusG, SharpRadiusB, SharpRatio, SharpScaling,
        SharpCoring, SharpLevel, MistRadius, MistRatio, MistScaling, MistMix};
    return keys;
}

const QVector<QString>& rgbMixerKeys()
{
    static const QVector<QString> keys = {MixerRR, MixerRG, MixerRB, MixerGR, MixerGG, MixerGB,
        MixerBR, MixerBG, MixerBB, MixerMonoR, MixerMonoG, MixerMonoB, MixerMono, MixerPreserve};
    return keys;
}

const QVector<QString>& agxKeys()
{
    static const QVector<QString> keys = {AgxOn, AgxInput, AgxExposure, AgxLook, AgxOutput};
    return keys;
}

bool isAgxKey(QStringView key)
{
    return key.startsWith(u"agx");
}

bool isRgbMixerKey(QStringView key)
{
    return key.startsWith(u"mixer");
}

bool isBlurKey(QStringView key)
{
    return key.startsWith(u"blur") || key.startsWith(u"sharp") || key.startsWith(u"mist");
}

const QVector<QString>& allKeys()
{
    static const QVector<QString> keys = {LiftR, LiftG, LiftB, LiftY, GammaR, GammaG, GammaB,
        GammaY, GainR, GainG, GainB, GainY, OffsetR, OffsetG, OffsetB, OffsetY, Contrast, Pivot,
        Saturation, Hue, Temperature, Tint, LumMix, Mix, ColorBoost, Shadows, Highlights, MidtoneDetail, SoftClipLow, SoftClipLowSoft,
        SoftClipHigh, SoftClipHighSoft,
        LogShadowR, LogShadowG, LogShadowB, LogShadowY, LogMidR, LogMidG, LogMidB, LogMidY,
        LogHighR, LogHighG, LogHighB, LogHighY, LogOffsetR, LogOffsetG, LogOffsetB, LogOffsetY,
        LogLowRange, LogHighRange, BlurRadiusR, BlurRadiusG, BlurRadiusB,
        BlurRatio, BlurScaling, SharpRadiusR, SharpRadiusG, SharpRadiusB, SharpRatio, SharpScaling,
        SharpCoring, SharpLevel, MistRadius, MistRatio, MistScaling, MistMix, MixerRR, MixerRG, MixerRB,
        MixerGR, MixerGG, MixerGB, MixerBR, MixerBG, MixerBB, MixerMonoR, MixerMonoG, MixerMonoB,
        MixerMono, MixerPreserve, AgxOn, AgxInput, AgxExposure, AgxLook, AgxOutput,
        HdrGlobalR, HdrGlobalG, HdrGlobalB, HdrGlobalY, HdrShadowR, HdrShadowG, HdrShadowB,
        HdrShadowY, HdrLightR, HdrLightG, HdrLightB, HdrLightY, HdrHighlightR, HdrHighlightG,
        HdrHighlightB, HdrHighlightY, HdrSpecularR, HdrSpecularG, HdrSpecularB, HdrSpecularY,
        HdrBlackR, HdrBlackG, HdrBlackB, HdrBlackY, HdrGlobalSat, HdrShadowSat, HdrLightSat,
        HdrHighlightSat, HdrSpecularSat, HdrShadowRange, HdrShadowFalloff, HdrLightRange,
        HdrLightFalloff, HdrHighlightRange, HdrHighlightFalloff};
    return keys;
}

QString label(const QString& key)
{
    static const char* const wheelNames[kWheelCount] = {QT_TRANSLATE_NOOP("kk::gradekeys", "Lift"),
        QT_TRANSLATE_NOOP("kk::gradekeys", "Gamma"), QT_TRANSLATE_NOOP("kk::gradekeys", "Gain"),
        QT_TRANSLATE_NOOP("kk::gradekeys", "Offset"),
        QT_TRANSLATE_NOOP("kk::gradekeys", "Log Shadow"),
        QT_TRANSLATE_NOOP("kk::gradekeys", "Log Midtone"),
        QT_TRANSLATE_NOOP("kk::gradekeys", "Log Highlight"),
        QT_TRANSLATE_NOOP("kk::gradekeys", "Log Offset"),
        QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Global"),
        QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Shadow"),
        QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Light"),
        QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Highlight"),
        QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Specular"),
        QT_TRANSLATE_NOOP("kk::gradekeys", "Black Offset")};
    static const char* const channels[4] = {"R", "G", "B", "Y"};
    for (int w = 0; w < kWheelCount; ++w) {
        for (int ch = 0; ch < 4; ++ch) {
            if (key == wheelKey(Wheel(w), ch))
                return QCoreApplication::translate("kk::gradekeys", wheelNames[w])
                    + QLatin1Char(' ') + QLatin1String(channels[ch]);
        }
    }
    const std::pair<const QString*, const char*> named[] = {
        {&BlurRadiusR, QT_TRANSLATE_NOOP("kk::gradekeys", "Blur Radius R")},
        {&BlurRadiusG, QT_TRANSLATE_NOOP("kk::gradekeys", "Blur Radius G")},
        {&BlurRadiusB, QT_TRANSLATE_NOOP("kk::gradekeys", "Blur Radius B")},
        {&BlurRatio, QT_TRANSLATE_NOOP("kk::gradekeys", "Blur H/V Ratio")},
        {&BlurScaling, QT_TRANSLATE_NOOP("kk::gradekeys", "Blur Scaling")},
        {&SharpRadiusR, QT_TRANSLATE_NOOP("kk::gradekeys", "Sharpen Radius R")},
        {&SharpRadiusG, QT_TRANSLATE_NOOP("kk::gradekeys", "Sharpen Radius G")},
        {&SharpRadiusB, QT_TRANSLATE_NOOP("kk::gradekeys", "Sharpen Radius B")},
        {&SharpRatio, QT_TRANSLATE_NOOP("kk::gradekeys", "Sharpen H/V Ratio")},
        {&SharpScaling, QT_TRANSLATE_NOOP("kk::gradekeys", "Sharpen Scaling")},
        {&SharpCoring, QT_TRANSLATE_NOOP("kk::gradekeys", "Coring Softness")},
        {&SharpLevel, QT_TRANSLATE_NOOP("kk::gradekeys", "Sharpen Level")},
        {&MistRadius, QT_TRANSLATE_NOOP("kk::gradekeys", "Mist Radius")},
        {&MistRatio, QT_TRANSLATE_NOOP("kk::gradekeys", "Mist H/V Ratio")},
        {&MistScaling, QT_TRANSLATE_NOOP("kk::gradekeys", "Mist Scaling")},
        {&MistMix, QT_TRANSLATE_NOOP("kk::gradekeys", "Mist Mix")},
        {&MixerRR, QT_TRANSLATE_NOOP("kk::gradekeys", "Red Output: Red")},
        {&MixerRG, QT_TRANSLATE_NOOP("kk::gradekeys", "Red Output: Green")},
        {&MixerRB, QT_TRANSLATE_NOOP("kk::gradekeys", "Red Output: Blue")},
        {&MixerGR, QT_TRANSLATE_NOOP("kk::gradekeys", "Green Output: Red")},
        {&MixerGG, QT_TRANSLATE_NOOP("kk::gradekeys", "Green Output: Green")},
        {&MixerGB, QT_TRANSLATE_NOOP("kk::gradekeys", "Green Output: Blue")},
        {&MixerBR, QT_TRANSLATE_NOOP("kk::gradekeys", "Blue Output: Red")},
        {&MixerBG, QT_TRANSLATE_NOOP("kk::gradekeys", "Blue Output: Green")},
        {&MixerBB, QT_TRANSLATE_NOOP("kk::gradekeys", "Blue Output: Blue")},
        {&MixerMonoR, QT_TRANSLATE_NOOP("kk::gradekeys", "Monochrome Red")},
        {&MixerMonoG, QT_TRANSLATE_NOOP("kk::gradekeys", "Monochrome Green")},
        {&MixerMonoB, QT_TRANSLATE_NOOP("kk::gradekeys", "Monochrome Blue")},
        {&MixerMono, QT_TRANSLATE_NOOP("kk::gradekeys", "Monochrome")},
        {&MixerPreserve, QT_TRANSLATE_NOOP("kk::gradekeys", "Preserve Luminance")},
        {&AgxOn, QT_TRANSLATE_NOOP("kk::gradekeys", "AgX")},
        {&AgxInput, QT_TRANSLATE_NOOP("kk::gradekeys", "AgX Input")},
        {&AgxExposure, QT_TRANSLATE_NOOP("kk::gradekeys", "AgX Exposure")},
        {&AgxLook, QT_TRANSLATE_NOOP("kk::gradekeys", "AgX Look")},
        {&AgxOutput, QT_TRANSLATE_NOOP("kk::gradekeys", "AgX Output")},
        {&HdrGlobalSat, QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Global Saturation")},
        {&HdrShadowSat, QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Shadow Saturation")},
        {&HdrLightSat, QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Light Saturation")},
        {&HdrHighlightSat, QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Highlight Saturation")},
        {&HdrSpecularSat, QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Specular Saturation")},
        {&HdrShadowRange, QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Shadow Range")},
        {&HdrShadowFalloff, QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Shadow Falloff")},
        {&HdrLightRange, QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Light Range")},
        {&HdrLightFalloff, QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Light Falloff")},
        {&HdrHighlightRange, QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Highlight Range")},
        {&HdrHighlightFalloff, QT_TRANSLATE_NOOP("kk::gradekeys", "HDR Highlight Falloff")},
        {&Contrast, QT_TRANSLATE_NOOP("kk::gradekeys", "Contrast")},
        {&Pivot, QT_TRANSLATE_NOOP("kk::gradekeys", "Pivot")},
        {&Saturation, QT_TRANSLATE_NOOP("kk::gradekeys", "Saturation")},
        {&Hue, QT_TRANSLATE_NOOP("kk::gradekeys", "Hue")},
        {&Temperature, QT_TRANSLATE_NOOP("kk::gradekeys", "Temperature")},
        {&Tint, QT_TRANSLATE_NOOP("kk::gradekeys", "Tint")},
        {&LumMix, QT_TRANSLATE_NOOP("kk::gradekeys", "Lum Mix")},
        {&Mix, QT_TRANSLATE_NOOP("kk::gradekeys", "Key Output Mix")},
        {&ColorBoost, QT_TRANSLATE_NOOP("kk::gradekeys", "Color Boost")},
        {&Shadows, QT_TRANSLATE_NOOP("kk::gradekeys", "Shadows")},
        {&Highlights, QT_TRANSLATE_NOOP("kk::gradekeys", "Highlights")},
        {&MidtoneDetail, QT_TRANSLATE_NOOP("kk::gradekeys", "Midtone Detail")},
        {&SoftClipLow, QT_TRANSLATE_NOOP("kk::gradekeys", "Soft Clip Low")},
        {&SoftClipLowSoft, QT_TRANSLATE_NOOP("kk::gradekeys", "Soft Clip Low Soft")},
        {&SoftClipHigh, QT_TRANSLATE_NOOP("kk::gradekeys", "Soft Clip High")},
        {&SoftClipHighSoft, QT_TRANSLATE_NOOP("kk::gradekeys", "Soft Clip High Soft")},
        {&LogLowRange, QT_TRANSLATE_NOOP("kk::gradekeys", "Low Range")},
        {&LogHighRange, QT_TRANSLATE_NOOP("kk::gradekeys", "High Range")},
    };
    for (const auto& [k, text] : named) {
        if (key == *k)
            return QCoreApplication::translate("kk::gradekeys", text);
    }
    return key;
}

std::pair<double, double> range(const QString& key)
{
    if (isHdrKey(key)) {
        if (key.endsWith(QLatin1String("Y")))
            return {-4.0, 4.0}; // exposure in stops
        if (key.endsWith(QLatin1String("Sat")))
            return {0.0, 4.0};
        if (key.endsWith(QLatin1String("Range")))
            return {-8.0, 8.0};
        if (key.endsWith(QLatin1String("Falloff")))
            return {0.0, 8.0};
        return {-1.0, 1.0};
    }
    if (key.startsWith(QLatin1String("gain")))
        return {0.0, 4.0};
    if (key.startsWith(QLatin1String("lift")) || key.startsWith(QLatin1String("gamma"))
        || key.startsWith(QLatin1String("offset")) || isLogKey(key))
        return key == LogLowRange || key == LogHighRange ? std::pair {0.0, 1.0}
                                                         : std::pair {-1.0, 1.0};
    if (key == ColorBoost || key == Shadows || key == Highlights || key == MidtoneDetail)
        return {-100.0, 100.0};
    if (key == Contrast || key == Saturation)
        return {0.0, 4.0};
    if (key == Pivot || key == LumMix || key == Mix || isBlurKey(key) || key.startsWith(QLatin1String("softClip")))
        return {0.0, 1.0};
    if (key == MixerMono || key == MixerPreserve || key == AgxOn)
        return {0.0, 1.0};
    if (key == AgxInput)
        return {0.0, double(int(AgxInput::Count) - 1)};
    if (key == AgxLook)
        return {0.0, double(int(AgxLook::Count) - 1)};
    if (key == AgxOutput)
        return {0.0, double(int(AgxOutput::Count) - 1)};
    if (key == AgxExposure)
        return {-8.0, 8.0};
    if (isRgbMixerKey(key))
        return {-2.0, 2.0};
    if (key == Hue)
        return {-180.0, 180.0};
    return {-1.0, 1.0}; // temperature, tint
}

} // namespace gradekeys

bool GradeNode::hasKey() const
{
    if (isMixer())
        return false;
    if (qualifier.enabled)
        return true;
    return std::any_of(
        windows.cbegin(), windows.cend(), [](const GradeWindow& w) { return w.isRenderable(); });
}

GradeTrackSample GradeTrack::at(int64_t frame) const
{
    if (samples.isEmpty())
        return {};
    const int64_t i = std::clamp<int64_t>(frame - firstFrame, 0, samples.size() - 1);
    return samples[int(i)];
}

GradeTrackSample GradeTrack::applied(int64_t frame) const
{
    GradeTrackSample s = at(frame);
    if (!pan)
        s.dx = 0.0;
    if (!tilt)
        s.dy = 0.0;
    if (!zoom) {
        s.sx = s.sy = 1.0;
    } else if (!perspective) {
        s.sx = s.sy = std::sqrt(std::max(1e-6, s.sx * s.sy));
    }
    if (!rotate)
        s.rot = 0.0;
    return s;
}

bool GradeNode::isAnimated() const
{
    return track.hasData() || params.isAnimated()
        || std::any_of(windows.cbegin(), windows.cend(),
            [](const GradeWindow& w) { return w.params.isAnimated() || !w.shapeKeys.isEmpty(); });
}

void GradeNode::shiftKeys(int64_t deltaFrames)
{
    params.shiftKeys(deltaFrames);
    track.firstFrame += deltaFrames;
    for (GradeWindow& w : windows) {
        w.params.shiftKeys(deltaFrames);
        if (!w.shapeKeys.isEmpty()) {
            QMap<int64_t, GradeWindowShapePoints> shifted;
            for (auto it = w.shapeKeys.cbegin(); it != w.shapeKeys.cend(); ++it)
                shifted.insert(it.key() + deltaFrames, it.value());
            w.shapeKeys = shifted;
        }
    }
}

quint64 ClipGrade::allocateId() const
{
    quint64 id = 0;
    for (const GradeNode& n : nodes) {
        id = std::max(id, n.id);
        for (const GradeWindow& w : n.windows)
            id = std::max(id, w.id);
    }
    return id + 1;
}

const GradeWindow* ClipGrade::window(quint64 id, const GradeNode** owner) const
{
    for (const GradeNode& n : nodes) {
        for (const GradeWindow& w : n.windows) {
            if (w.id == id) {
                if (owner)
                    *owner = &n;
                return &w;
            }
        }
    }
    return nullptr;
}

GradeWindow* ClipGrade::window(quint64 id, GradeNode** owner)
{
    for (GradeNode& n : nodes) {
        for (GradeWindow& w : n.windows) {
            if (w.id == id) {
                if (owner)
                    *owner = &n;
                return &w;
            }
        }
    }
    return nullptr;
}

bool ClipGrade::isAnimated() const
{
    return std::any_of(
        nodes.cbegin(), nodes.cend(), [](const GradeNode& n) { return n.isAnimated(); });
}

void ClipGrade::shiftKeys(int64_t deltaFrames)
{
    for (GradeNode& n : nodes)
        n.shiftKeys(deltaFrames);
}

int ClipGrade::indexOf(quint64 id) const
{
    for (int i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id == id)
            return i;
    }
    return -1;
}

const GradeNode* ClipGrade::node(quint64 id) const
{
    const int i = indexOf(id);
    return i < 0 ? nullptr : &nodes[i];
}

GradeNode* ClipGrade::node(quint64 id)
{
    const int i = indexOf(id);
    return i < 0 ? nullptr : &nodes[i];
}

QString gradeNodeName(const ClipGrade& grade, int index)
{
    if (index < 0 || index >= grade.nodes.size())
        return {};
    const GradeNode& node = grade.nodes[index];
    if (!node.label.isEmpty())
        return node.label;
    if (node.isMixer())
        return {};
    // As in Resolve only correctors are numbered: mixers do not use up a number.
    int number = 0;
    for (int i = 0; i <= index; ++i) {
        if (!grade.nodes[i].isMixer())
            ++number;
    }
    return QStringLiteral("%1").arg(number, 2, 10, QLatin1Char('0'));
}

QString gradeNodeTypeToString(GradeNodeType type)
{
    switch (type) {
    case GradeNodeType::Corrector:
        return QStringLiteral("corrector");
    case GradeNodeType::Parallel:
        return QStringLiteral("parallel");
    case GradeNodeType::Layer:
        return QStringLiteral("layer");
    case GradeNodeType::KeyMixer:
        return QStringLiteral("keymixer");
    case GradeNodeType::Splitter:
        return QStringLiteral("splitter");
    case GradeNodeType::Combiner:
        return QStringLiteral("combiner");
    case GradeNodeType::Compound:
        return QStringLiteral("compound");
    case GradeNodeType::AlphaOutput:
        return QStringLiteral("alphaoutput");
    }
    return QStringLiteral("corrector");
}

GradeNodeType gradeNodeTypeFromString(const QString& text)
{
    static const std::pair<const char*, GradeNodeType> table[] = {
        {"parallel", GradeNodeType::Parallel}, {"layer", GradeNodeType::Layer},
        {"keymixer", GradeNodeType::KeyMixer}, {"splitter", GradeNodeType::Splitter},
        {"combiner", GradeNodeType::Combiner}, {"compound", GradeNodeType::Compound},
        {"alphaoutput", GradeNodeType::AlphaOutput}};
    for (const auto& [name, type] : table) {
        if (text == QLatin1String(name))
            return type;
    }
    return GradeNodeType::Corrector;
}

namespace gradeops {

namespace {

/// The nodes @p n reads: its inputs, its key input; a Compound node reads its inner output
/// (its input while bypassed or empty).
QVector<quint64> dependencies(const GradeNode& n)
{
    QVector<quint64> out;
    if (n.type == GradeNodeType::Compound) {
        if (n.enabled && n.innerOutput != 0)
            out.append(n.innerOutput);
        else if (n.inputs.value(0, 0) != 0)
            out.append(n.inputs[0]);
        return out;
    }
    for (const quint64 in : n.inputs) {
        if (in != 0)
            out.append(in);
    }
    if (n.keyInput != 0)
        out.append(n.keyInput);
    return out;
}

/// Post-order DFS from node @p index (inputs first). @p state: 0 unvisited, 1 on the stack,
/// 2 done; a node met while on the stack closes a cycle and is skipped. Compound nodes are
/// walked through but not listed (they are aliases of their inner output).
void visit(const ClipGrade& g, int index, std::vector<int>& state, std::vector<int>& out)
{
    if (index < 0 || state[size_t(index)] != 0)
        return;
    state[size_t(index)] = 1;
    for (const quint64 in : dependencies(g.nodes[index]))
        visit(g, g.indexOf(in), state, out);
    state[size_t(index)] = 2;
    if (g.nodes[index].type != GradeNodeType::Compound)
        out.push_back(index);
}

/// Normalises `ports` of @p n: as long as inputs, or empty when every port is 0.
void normalizePorts(GradeNode& n)
{
    if (n.ports.isEmpty())
        return;
    n.ports.resize(n.inputs.size());
    if (std::all_of(n.ports.cbegin(), n.ports.cend(), [](quint8 p) { return p == 0; }))
        n.ports.clear();
}

void setPort(GradeNode& n, int k, int port)
{
    if (n.ports.size() < n.inputs.size())
        n.ports.resize(n.inputs.size());
    if (k >= 0 && k < n.ports.size())
        n.ports[k] = quint8(port);
    normalizePorts(n);
}

/// Removes input @p k of @p n with its port and Key Mixer settings.
void removeInputAt(GradeNode& n, int k)
{
    n.inputs.remove(k);
    if (k < n.ports.size())
        n.ports.remove(k);
    if (k < n.keyGains.size())
        n.keyGains.remove(k);
    if (k < n.keyInverts.size())
        n.keyInverts.remove(k);
    normalizePorts(n);
}

/// Makes every picture reference to node @p oldId (consumer inputs and the output) refer to
/// output @p newPort of @p newId, except in node @p except.
void redirect(ClipGrade& g, quint64 oldId, quint64 newId, quint64 except, int newPort = 0)
{
    for (GradeNode& n : g.nodes) {
        if (n.id == except || n.type == GradeNodeType::KeyMixer)
            continue;
        for (int k = 0; k < n.inputs.size(); ++k) {
            if (n.inputs[k] == oldId) {
                n.inputs[k] = newId;
                if (newPort != 0 || k < n.ports.size())
                    setPort(n, k, newPort);
            }
        }
    }
    if (g.output == oldId)
        g.output = newId;
}

/// The node new nodes go after when @p id has no picture output (or is unknown): the last node
/// of the output chain, else the last node.
GradeNode* pictureNode(ClipGrade& g, quint64 id)
{
    GradeNode* base = g.node(id);
    if (base && base->hasPictureOutput())
        return base;
    const std::vector<int> order = evaluationOrder(g);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (g.nodes[*it].hasPictureOutput())
            return &g.nodes[*it];
    }
    for (int i = int(g.nodes.size()) - 1; i >= 0; --i) {
        if (g.nodes[i].hasPictureOutput() && g.nodes[i].type != GradeNodeType::Compound)
            return &g.nodes[i];
    }
    return nullptr;
}

/// Adds a corrector parallel to / layered over @p nodeId through a mixer of @p type.
quint64 addMixed(ClipGrade& g, quint64 nodeId, GradeNodeType type)
{
    if (g.isEmpty())
        initialize(g);
    GradeNode* base = pictureNode(g, nodeId);
    if (!base)
        base = &g.nodes.back();
    const quint64 baseId = base->id;
    const quint64 source = base->inputs.value(0, 0);
    // An existing mixer of this type directly after the node takes one more input.
    for (GradeNode& n : g.nodes) {
        if (n.type == type && n.inputs.contains(baseId)) {
            GradeNode extra;
            extra.id = g.allocateId();
            extra.inputs = {source};
            const quint64 mixerId = n.id;
            g.nodes.append(extra);
            g.node(mixerId)->inputs.append(extra.id);
            placeNode(g, extra.id);
            g.nodes.last().pos.ry() += kNodeSpacingY * (g.node(mixerId)->inputs.size() - 1);
            return extra.id;
        }
    }
    GradeNode extra;
    extra.id = g.allocateId();
    extra.inputs = {source};
    GradeNode mixer;
    mixer.id = extra.id + 1;
    mixer.type = type;
    mixer.inputs = {baseId, extra.id};
    const QPointF basePos = base->pos;
    redirect(g, baseId, mixer.id, 0);
    extra.pos = basePos + QPointF(0, kNodeSpacingY);
    mixer.pos = basePos + QPointF(kNodeSpacingX, kNodeSpacingY * 0.5);
    g.nodes.append(extra);
    g.nodes.append(mixer);
    return extra.id;
}

} // namespace

std::vector<int> evaluationOrder(const ClipGrade& grade)
{
    std::vector<int> out;
    if (grade.output == 0)
        return out;
    std::vector<int> state(size_t(grade.nodes.size()), 0);
    visit(grade, grade.indexOf(grade.output), state, out);
    return out;
}

bool isNodeIdentity(const GradeNode& node, int64_t localFrame)
{
    if (node.isMixer())
        return false;
    if (!node.enabled || node.value(gradekeys::Mix, localFrame) <= 0.0)
        return true;
    if (!node.lutPath.isEmpty() || !node.curves.isIdentity() || !node.warp.isIdentity())
        return false;
    for (const QString& key : gradekeys::allKeys()) {
        if (key == gradekeys::Pivot || key == gradekeys::LumMix || key == gradekeys::Mix
            || key == gradekeys::LogLowRange || key == gradekeys::LogHighRange
            || gradekeys::isBlurKey(key) || gradekeys::isRgbMixerKey(key)
            || gradekeys::isAgxKey(key) || gradekeys::isHdrZoneKey(key))
            continue; // only matter together with another value (blur, mixer, AgX: below)
        if (node.value(key, localFrame) != gradekeys::defaultValue(key))
            return false;
    }
    return !grademath::BlurValues::at(node, localFrame, 1080.0).active()
        && grademath::MixerValues::at(node, localFrame).isIdentity()
        && !grademath::AgxValues::at(node, localFrame).on;
}

bool isIdentity(const ClipGrade& grade, int64_t localFrame)
{
    if (alphaOutput(grade))
        return false;
    const std::vector<int> order = evaluationOrder(grade);
    if (order.empty())
        return true;
    // Mixers of identity inputs average / layer identical pictures: identity as well.
    return std::all_of(order.begin(), order.end(), [&](int i) {
        const GradeNode& n = grade.nodes[i];
        return n.isMixer() || isNodeIdentity(n, localFrame);
    });
}

quint64 initialize(ClipGrade& grade)
{
    grade.nodes.clear();
    GradeNode n;
    n.id = 1;
    n.inputs = {0};
    grade.nodes.append(n);
    grade.output = n.id;
    return n.id;
}

ClipGrade toneMapGrade(const ToneMap& map)
{
    ClipGrade grade;
    if (!map.agx)
        return grade;
    const quint64 id = initialize(grade);
    GradeNode* n = grade.node(id);
    n->params.setBase(gradekeys::AgxOn, 1.0);
    n->params.setBase(gradekeys::AgxLook, double(int(map.look)));
    return grade;
}

quint64 addSerialNode(ClipGrade& grade, quint64 after)
{
    if (grade.isEmpty())
        return initialize(grade);
    GradeNode* prev = pictureNode(grade, after);
    if (!prev)
        prev = &grade.nodes.back();
    GradeNode n;
    n.id = grade.allocateId();
    n.inputs = {prev->id};
    n.pos = prev->pos + QPointF(kNodeSpacingX, 0);
    const quint64 prevId = prev->id;
    redirect(grade, prevId, n.id, 0);
    grade.nodes.append(n);
    // Nodes downstream that sat where the new one goes move one step right.
    for (GradeNode& other : grade.nodes) {
        if (other.id != n.id && other.pos.x() >= n.pos.x() - 1e-9 && reaches(grade, other.id, n.id))
            other.pos.rx() += kNodeSpacingX;
    }
    return n.id;
}

quint64 addParallelNode(ClipGrade& grade, quint64 node)
{
    return addMixed(grade, node, GradeNodeType::Parallel);
}

quint64 addLayerNode(ClipGrade& grade, quint64 node)
{
    return addMixed(grade, node, GradeNodeType::Layer);
}

QVector<quint64> compoundMembers(const ClipGrade& grade, quint64 id)
{
    QVector<quint64> out;
    for (const GradeNode& n : grade.nodes) {
        if (n.compound == id && id != 0)
            out.append(n.id);
    }
    return out;
}

namespace {

/// The members of Compound @p id and of the Compounds in it, all levels.
QVector<quint64> descendants(const ClipGrade& grade, quint64 id)
{
    QVector<quint64> out;
    QVector<quint64> todo {id};
    while (!todo.isEmpty()) {
        const quint64 cur = todo.takeLast();
        for (const quint64 m : compoundMembers(grade, cur)) {
            if (!out.contains(m)) {
                out.append(m);
                todo.append(m);
            }
        }
    }
    return out;
}

bool isPictureMixer(const GradeNode& n)
{
    return n.type == GradeNodeType::Parallel || n.type == GradeNodeType::Layer;
}

} // namespace

bool removeNode(ClipGrade& grade, quint64 id)
{
    const int index = grade.indexOf(id);
    if (index < 0)
        return false;
    const GradeNode removed = grade.nodes[index];
    const quint64 replacement = removed.inputs.value(0, 0);
    const int replacementPort = removed.portOf(0);
    QVector<quint64> doomed {id};
    if (removed.type == GradeNodeType::Compound)
        doomed += descendants(grade, id);
    for (int i = int(grade.nodes.size()) - 1; i >= 0; --i) {
        if (doomed.contains(grade.nodes[i].id))
            grade.nodes.remove(i);
    }
    // Key links into the deleted nodes end; a Key Mixer slot goes (the last one stays empty).
    for (GradeNode& n : grade.nodes) {
        if (doomed.contains(n.keyInput))
            n.keyInput = 0;
        if (n.type == GradeNodeType::KeyMixer) {
            for (int k = int(n.inputs.size()) - 1; k >= 0; --k) {
                if (!doomed.contains(n.inputs[k]))
                    continue;
                if (n.inputs.size() > 1)
                    removeInputAt(n, k);
                else
                    n.inputs[k] = 0;
            }
        }
        if (n.type == GradeNodeType::Compound && doomed.contains(n.innerOutput)) {
            const GradeNode* r = grade.node(replacement);
            n.innerOutput = r && r->compound == n.id ? replacement : 0;
        }
    }
    // A mixer branch whose input another branch of the same mixer already carries (a deleted
    // parallel / layer node) leaves the mixer instead of feeding it the ungraded picture.
    for (GradeNode& n : grade.nodes) {
        if (!isPictureMixer(n) || !n.inputs.contains(id))
            continue;
        const bool shared = std::any_of(n.inputs.begin(), n.inputs.end(),
            [&](quint64 in) { return in != id && reaches(grade, in, replacement); });
        if (shared) {
            for (int k = int(n.inputs.size()) - 1; k >= 0; --k) {
                if (n.inputs[k] == id)
                    removeInputAt(n, k);
            }
        }
    }
    redirect(grade, id, replacement, 0, replacementPort);
    // Tidy the mixers: duplicate inputs merge, a mixer with a single input dissolves.
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < grade.nodes.size(); ++i) {
            GradeNode& n = grade.nodes[i];
            if (!isPictureMixer(n))
                continue;
            QVector<std::pair<quint64, int>> seen;
            for (int k = 0; k < n.inputs.size();) {
                const std::pair<quint64, int> key {n.inputs[k], n.portOf(k)};
                if (seen.contains(key)) {
                    removeInputAt(n, k);
                } else {
                    seen.append(key);
                    ++k;
                }
            }
            if (n.inputs.size() <= 1) {
                const quint64 mixerId = n.id;
                const quint64 only = n.inputs.value(0, 0);
                const int onlyPort = n.portOf(0);
                grade.nodes.remove(i);
                redirect(grade, mixerId, only, 0, onlyPort);
                changed = true;
                break;
            }
        }
    }
    if (grade.output != 0 && grade.indexOf(grade.output) < 0)
        grade.output = 0;
    return true;
}

bool reaches(const ClipGrade& grade, quint64 of, quint64 id)
{
    if (of == 0)
        return id == 0;
    QVector<quint64> stack {of};
    QVector<quint64> seen;
    while (!stack.isEmpty()) {
        const quint64 cur = stack.takeLast();
        if (cur == id)
            return true;
        if (cur == 0 || seen.contains(cur))
            continue;
        seen.append(cur);
        if (const GradeNode* n = grade.node(cur)) {
            // The picture (id 0) is an input too; dependencies() leaves it out.
            if (id == 0 && n->type != GradeNodeType::Compound && n->inputs.contains(0))
                return true;
            stack += dependencies(*n);
        }
    }
    return false;
}

bool connect(ClipGrade& grade, quint64 from, quint64 to, int inputIndex, int fromPort)
{
    const GradeNode* source = from != 0 ? grade.node(from) : nullptr;
    if (from != 0
        && (!source || !source->hasPictureOutput() || fromPort < 0
            || fromPort >= std::max(1, source->outputPortCount())))
        return false;
    if (to == 0) {
        grade.output = from;
        return true;
    }
    GradeNode* target = grade.node(to);
    if (!target || !target->hasPictureInput() || from == to
        || (from != 0 && reaches(grade, from, to)))
        return false; // to is upstream of from: the edge would close a cycle
    if (target->type == GradeNodeType::Compound) {
        if (descendants(grade, to).contains(from))
            return false;
        // The members that read the old input now read the new one.
        const quint64 old = target->inputs.value(0, 0);
        const int oldPort = target->portOf(0);
        if (old != from || oldPort != fromPort) {
            for (const quint64 m : descendants(grade, to)) {
                GradeNode* member = grade.node(m);
                if (member->type == GradeNodeType::Compound)
                    continue; // a nested compound's input is one of its own links below
                for (int k = 0; k < member->inputs.size(); ++k) {
                    if (member->inputs[k] == old && member->portOf(k) == oldPort
                        && member->type != GradeNodeType::KeyMixer) {
                        member->inputs[k] = from;
                        setPort(*member, k, fromPort);
                    }
                }
                if (member->keyInput == old && old != 0)
                    member->keyInput = from;
            }
            for (const quint64 m : descendants(grade, to)) {
                GradeNode* member = grade.node(m);
                if (member->type == GradeNodeType::Compound && member->inputs.value(0, 0) == old
                    && member->portOf(0) == oldPort) {
                    member->inputs = {from};
                    member->ports.clear();
                    setPort(*member, 0, fromPort);
                }
            }
        }
        target->inputs = {from};
        target->ports.clear();
        setPort(*target, 0, fromPort);
        return true;
    }
    if (target->type == GradeNodeType::Combiner) {
        while (target->inputs.size() < 3)
            target->inputs.append(0);
        if (inputIndex < 0 || inputIndex > 2)
            return false;
        target->inputs[inputIndex] = from;
        setPort(*target, inputIndex, fromPort);
        return true;
    }
    if (!target->hasVariableInputs()) {
        target->inputs = {from};
        target->ports.clear();
        setPort(*target, 0, fromPort);
        return true;
    }
    if (target->type == GradeNodeType::KeyMixer)
        return false; // its inputs are keys: connectKey()
    if (inputIndex >= 0 && inputIndex < target->inputs.size()) {
        target->inputs[inputIndex] = from;
        setPort(*target, inputIndex, fromPort);
    } else {
        for (int k = 0; k < target->inputs.size(); ++k) {
            if (target->inputs[k] == from && target->portOf(k) == fromPort)
                return false;
        }
        target->inputs.append(from);
        setPort(*target, int(target->inputs.size()) - 1, fromPort);
    }
    return true;
}

bool disconnect(ClipGrade& grade, quint64 to, int inputIndex)
{
    if (to == 0) {
        if (grade.output == 0)
            return false;
        grade.output = 0;
        return true;
    }
    GradeNode* target = grade.node(to);
    if (!target || !target->hasPictureInput() || inputIndex < 0
        || inputIndex >= target->inputs.size())
        return false;
    if (target->type == GradeNodeType::KeyMixer)
        return false;
    if (target->type == GradeNodeType::Compound) {
        if (target->inputs[0] == 0)
            return false;
        return connect(grade, 0, to, 0);
    }
    if (target->type == GradeNodeType::Combiner) {
        if (target->inputs[inputIndex] == 0 && target->portOf(inputIndex) == 0)
            return false;
        target->inputs[inputIndex] = 0;
        setPort(*target, inputIndex, 0);
        return true;
    }
    if (!target->hasVariableInputs()) {
        if (target->inputs[0] == 0)
            return false;
        target->inputs = {0};
        target->ports.clear();
        return true;
    }
    if (target->inputs.size() <= 1)
        return false;
    removeInputAt(*target, inputIndex);
    return true;
}

bool connectKey(ClipGrade& grade, quint64 from, quint64 to, int inputIndex, bool invert)
{
    const GradeNode* source = grade.node(from);
    GradeNode* target = grade.node(to);
    if (!source || !target || !source->hasKeyOutput() || from == to || reaches(grade, from, to))
        return false;
    if (target->type == GradeNodeType::Compound && descendants(grade, to).contains(from))
        return false;
    if (target->hasKeyInput()) {
        target->keyInput = from;
        target->keyInputInvert = invert;
        return true;
    }
    if (target->type != GradeNodeType::KeyMixer)
        return false;
    if (inputIndex >= 0 && inputIndex < target->inputs.size()) {
        target->inputs[inputIndex] = from;
    } else {
        if (target->inputs.contains(from) || target->inputs.size() >= kMaxMixerInputs)
            return false;
        if (target->inputs.size() == 1 && target->inputs[0] == 0)
            target->inputs[0] = from; // the empty first slot
        else
            target->inputs.append(from);
        inputIndex = int(target->inputs.indexOf(from));
    }
    // Settings are kept per slot; a new slot starts at gain 1, not inverted.
    while (target->keyGains.size() < target->inputs.size())
        target->keyGains.append(1.0);
    while (target->keyInverts.size() < target->inputs.size())
        target->keyInverts.append(false);
    target->keyInverts[inputIndex] = invert;
    return true;
}

bool disconnectKey(ClipGrade& grade, quint64 to, int inputIndex)
{
    GradeNode* target = grade.node(to);
    if (!target)
        return false;
    if (target->hasKeyInput()) {
        if (target->keyInput == 0)
            return false;
        target->keyInput = 0;
        target->keyInputInvert = false;
        return true;
    }
    if (target->type != GradeNodeType::KeyMixer || inputIndex < 0
        || inputIndex >= target->inputs.size() || target->inputs[inputIndex] == 0)
        return false;
    if (target->inputs.size() > 1)
        removeInputAt(*target, inputIndex);
    else
        target->inputs[0] = 0;
    return true;
}

quint64 resolve(const ClipGrade& grade, quint64 id)
{
    for (int guard = 0; guard < 64 && id != 0; ++guard) {
        const GradeNode* n = grade.node(id);
        if (!n || n->type != GradeNodeType::Compound)
            return id;
        id = n->enabled && n->innerOutput != 0 ? n->innerOutput : n->inputs.value(0, 0);
    }
    return id;
}

const GradeNode* alphaOutput(const ClipGrade& grade)
{
    for (const GradeNode& n : grade.nodes) {
        if (n.type == GradeNodeType::AlphaOutput && n.enabled)
            return &n;
    }
    return nullptr;
}

std::vector<int> evaluationOrderWithAlpha(const ClipGrade& grade)
{
    std::vector<int> out;
    std::vector<int> state(size_t(grade.nodes.size()), 0);
    if (grade.output != 0)
        visit(grade, grade.indexOf(grade.output), state, out);
    if (const GradeNode* a = alphaOutput(grade))
        visit(grade, grade.indexOf(a->id), state, out);
    return out;
}

bool resetNode(ClipGrade& grade, quint64 id)
{
    GradeNode* n = grade.node(id);
    if (!n)
        return false;
    n->params = ParamSet {};
    n->curves = GradeCurves {};
    n->warp = GradeWarp {};
    n->qualifier = GradeQualifier {};
    n->windows.clear();
    n->track = GradeTrack {};
    n->lutPath.clear();
    n->lutInterpolation = LutInterpolation::Tetrahedral;
    return true;
}

bool setNodeEnabled(ClipGrade& grade, quint64 id, bool enabled)
{
    GradeNode* n = grade.node(id);
    if (!n || n->enabled == enabled)
        return false;
    n->enabled = enabled;
    return true;
}

quint64 addSerialNodeBefore(ClipGrade& grade, quint64 before)
{
    GradeNode* next = grade.node(before);
    if (grade.isEmpty() || !next || !next->hasPictureInput())
        return addSerialNode(grade, before);
    GradeNode n;
    n.id = grade.allocateId();
    n.inputs = {next->inputs.value(0, 0)};
    if (next->portOf(0) != 0)
        n.ports = {quint8(next->portOf(0))};
    n.pos = next->pos;
    n.compound = next->compound;
    const quint64 nextId = next->id;
    const bool compound = next->type == GradeNodeType::Compound;
    if (!compound) {
        next->inputs[0] = n.id;
        setPort(*next, 0, 0);
    }
    grade.nodes.append(n);
    if (compound) // the members reading the compound's input now read the new node
        connect(grade, n.id, nextId, 0);
    // The node it feeds and everything after it move one step right.
    for (GradeNode& other : grade.nodes) {
        if (other.id != n.id && reaches(grade, other.id, nextId))
            other.pos.rx() += kNodeSpacingX;
    }
    return n.id;
}

quint64 addOutsideNode(ClipGrade& grade, quint64 node)
{
    if (grade.isEmpty())
        initialize(grade);
    const GradeNode* prev = pictureNode(grade, node);
    if (!prev)
        return 0;
    // The Outside node reads the picture and the key of the node it follows.
    const bool keyed = prev->hasKeyOutput();
    const quint64 prevId = prev->id;
    const quint64 id = addSerialNode(grade, prevId);
    GradeNode* n = grade.node(id);
    if (keyed) {
        n->keyInput = prevId;
        n->keyInputInvert = true;
    }
    return id;
}

quint64 addKeyMixer(ClipGrade& grade, quint64 node)
{
    if (grade.isEmpty())
        initialize(grade);
    const GradeNode* base = grade.node(node);
    if (!base)
        base = pictureNode(grade, node);
    GradeNode mixer;
    mixer.id = grade.allocateId();
    mixer.type = GradeNodeType::KeyMixer;
    mixer.inputs = {base && base->hasKeyOutput() ? base->id : 0};
    mixer.keyGains = {1.0};
    mixer.keyInverts = {false};
    mixer.pos = base ? base->pos + QPointF(kNodeSpacingX, kNodeSpacingY) : QPointF(0, 0);
    mixer.compound = base ? base->compound : 0;
    grade.nodes.append(mixer);
    return mixer.id;
}

quint64 addSplitterCombiner(ClipGrade& grade, quint64 node)
{
    if (grade.isEmpty())
        initialize(grade);
    GradeNode* prev = pictureNode(grade, node);
    if (!prev)
        return 0;
    const quint64 prevId = prev->id;
    GradeNode splitter;
    splitter.id = grade.allocateId();
    splitter.type = GradeNodeType::Splitter;
    splitter.inputs = {prevId};
    splitter.pos = prev->pos + QPointF(kNodeSpacingX, 0);
    splitter.compound = prev->compound;
    GradeNode combiner;
    combiner.id = splitter.id + 1;
    combiner.type = GradeNodeType::Combiner;
    combiner.inputs = {splitter.id, splitter.id, splitter.id};
    combiner.ports = {0, 1, 2};
    combiner.pos = splitter.pos + QPointF(kNodeSpacingX, 0);
    combiner.compound = prev->compound;
    // Everything that read the node now reads the combiner.
    redirect(grade, prevId, combiner.id, 0);
    for (GradeNode& other : grade.nodes) {
        if (other.pos.x() >= splitter.pos.x() - 1e-9 && reaches(grade, other.id, combiner.id))
            other.pos.rx() += 2 * kNodeSpacingX;
    }
    grade.nodes.append(splitter);
    grade.nodes.append(combiner);
    return splitter.id;
}

quint64 addAlphaOutput(ClipGrade& grade, quint64 node)
{
    if (grade.isEmpty())
        initialize(grade);
    const GradeNode* base = grade.node(node);
    if (!base || !base->hasKeyOutput())
        base = pictureNode(grade, node);
    const quint64 keyId = base && base->hasKeyOutput() ? base->id : 0;
    for (GradeNode& n : grade.nodes) {
        if (n.type == GradeNodeType::AlphaOutput) {
            n.keyInput = keyId;
            n.enabled = true;
            return n.id;
        }
    }
    GradeNode a;
    a.id = grade.allocateId();
    a.type = GradeNodeType::AlphaOutput;
    a.keyInput = keyId;
    a.pos = base ? base->pos + QPointF(kNodeSpacingX, kNodeSpacingY) : QPointF(0, 0);
    a.compound = base && base->type != GradeNodeType::Compound ? base->compound : 0;
    grade.nodes.append(a);
    return a.id;
}

quint64 groupIntoCompound(ClipGrade& grade, const QVector<quint64>& ids)
{
    if (ids.isEmpty())
        return 0;
    quint64 level = 0;
    bool first = true;
    for (const quint64 id : ids) {
        const GradeNode* n = grade.node(id);
        if (!n || n->type == GradeNodeType::AlphaOutput)
            return 0;
        if (first)
            level = n->compound;
        else if (n->compound != level)
            return 0;
        first = false;
    }
    // Everything inside: the chosen nodes and the members of the compounds among them.
    QVector<quint64> members = ids;
    for (const quint64 id : ids) {
        if (grade.node(id)->type == GradeNodeType::Compound)
            members += descendants(grade, id);
    }
    // The picture sources read from outside, and the nodes read from outside.
    QVector<std::pair<quint64, int>> sources;
    quint64 outId = 0;
    int outsideReaders = 0;
    for (const quint64 id : ids) {
        const GradeNode* n = grade.node(id);
        if (n->hasPictureInput()) {
            for (int k = 0; k < n->inputs.size(); ++k) {
                const quint64 in = n->inputs[k];
                if (n->type == GradeNodeType::Compound && k > 0)
                    break;
                if (!members.contains(in) && !sources.contains({in, n->portOf(k)}))
                    sources.append({in, n->portOf(k)});
            }
        }
    }
    if (sources.size() > 1)
        return 0;
    for (const GradeNode& n : grade.nodes) {
        if (members.contains(n.id))
            continue;
        for (const quint64 in : dependencies(n)) {
            if (ids.contains(in) && in != 0) {
                if (outId != 0 && outId != in)
                    return 0; // two members read from outside
                outId = in;
                ++outsideReaders;
            }
        }
    }
    if (ids.contains(grade.output)) {
        if (outId != 0 && outId != grade.output)
            return 0;
        outId = grade.output;
    }
    if (outId == 0) {
        // Nothing reads the selection: its last node (in evaluation order) leaves it.
        for (const int i : evaluationOrder(grade)) {
            if (ids.contains(grade.nodes[i].id))
                outId = grade.nodes[i].id;
        }
        if (outId == 0)
            outId = ids.last();
    }
    GradeNode c;
    c.id = grade.allocateId();
    c.type = GradeNodeType::Compound;
    c.inputs = {sources.isEmpty() ? 0 : sources[0].first};
    if (!sources.isEmpty() && sources[0].second != 0)
        c.ports = {quint8(sources[0].second)};
    c.innerOutput = outId;
    c.compound = level;
    QPointF sum;
    for (const quint64 id : ids)
        sum += grade.node(id)->pos;
    c.pos = sum / double(ids.size());
    // Whatever read the output now reads the compound (it is the same picture and key).
    for (GradeNode& n : grade.nodes) {
        if (members.contains(n.id))
            continue;
        for (int k = 0; k < n.inputs.size(); ++k) {
            if (n.inputs[k] == outId && n.type != GradeNodeType::KeyMixer) {
                n.inputs[k] = c.id;
                setPort(n, k, 0);
            } else if (n.inputs[k] == outId) {
                n.inputs[k] = c.id;
            }
        }
        if (n.keyInput == outId)
            n.keyInput = c.id;
    }
    if (grade.output == outId)
        grade.output = c.id;
    for (const quint64 id : ids)
        grade.node(id)->compound = c.id;
    grade.nodes.append(c);
    return c.id;
}

bool ungroupCompound(ClipGrade& grade, quint64 id)
{
    GradeNode* c = grade.node(id);
    if (!c || c->type != GradeNodeType::Compound)
        return false;
    const quint64 parent = c->compound;
    const quint64 inner = c->enabled && c->innerOutput != 0 ? c->innerOutput : c->inputs.value(0, 0);
    const int innerPort = c->enabled && c->innerOutput != 0 ? 0 : c->portOf(0);
    const quint64 doomed = id;
    // A bypassed compound dissolves into its members but keeps passing its input: members
    // are not reached then, exactly as before.
    for (GradeNode& n : grade.nodes) {
        if (n.compound == doomed)
            n.compound = parent;
    }
    for (GradeNode& n : grade.nodes) {
        if (n.id == doomed)
            continue;
        if (n.keyInput == doomed)
            n.keyInput = inner;
        if (n.type == GradeNodeType::Compound && n.innerOutput == doomed)
            n.innerOutput = inner;
    }
    redirect(grade, doomed, inner, 0, innerPort);
    for (GradeNode& n : grade.nodes) { // Key Mixer slots read the key
        if (n.type == GradeNodeType::KeyMixer) {
            for (quint64& in : n.inputs) {
                if (in == doomed)
                    in = inner;
            }
        }
    }
    grade.nodes.remove(grade.indexOf(doomed));
    return true;
}

quint64 addWindow(ClipGrade& grade, quint64 nodeId, GradeWindowShape shape)
{
    GradeNode* n = grade.node(nodeId);
    if (!n || n->isMixer() || n->windows.size() >= kMaxGradeWindows)
        return 0;
    GradeWindow w;
    w.id = grade.allocateId();
    w.shape = shape;
    if (shape == GradeWindowShape::Polygon)
        w.points = {{QPointF(0, -0.5)}, {QPointF(0.5, 0)}, {QPointF(0, 0.5)}, {QPointF(-0.5, 0)}};
    if (shape == GradeWindowShape::Curve) {
        // A rounded shape: four smooth points on a circle (handle length 0.5523 of the radius).
        constexpr double k = 0.5 * 0.5522847498;
        w.points = {{QPointF(0, -0.5), QPointF(-k, 0), QPointF(k, 0), true},
            {QPointF(0.5, 0), QPointF(0, -k), QPointF(0, k), true},
            {QPointF(0, 0.5), QPointF(k, 0), QPointF(-k, 0), true},
            {QPointF(-0.5, 0), QPointF(0, k), QPointF(0, -k), true}};
    }
    if (shape == GradeWindowShape::Gradient)
        w.params.setBase(gradewindowkeys::SoftOut, 0.25);
    n->windows.append(w);
    return w.id;
}

bool removeWindow(ClipGrade& grade, quint64 windowId)
{
    for (GradeNode& n : grade.nodes) {
        for (int i = 0; i < n.windows.size(); ++i) {
            if (n.windows[i].id == windowId) {
                n.windows.remove(i);
                return true;
            }
        }
    }
    return false;
}

quint64 defaultSelection(const ClipGrade& grade)
{
    if (grade.isEmpty())
        return 0;
    if (grade.node(grade.output))
        return grade.output;
    return grade.nodes.back().id;
}

bool appendGraph(ClipGrade& target, const ClipGrade& source)
{
    if (source.isEmpty())
        return false;
    if (target.isEmpty()) {
        target = source;
        return true;
    }
    // Every id (nodes and windows share one space) moves up by the target's largest id.
    const quint64 shift = target.allocateId() - 1;
    double maxX = 0.0;
    for (const GradeNode& n : std::as_const(target.nodes))
        maxX = std::max(maxX, n.pos.x());
    double minX = source.nodes.first().pos.x();
    for (const GradeNode& n : source.nodes)
        minX = std::min(minX, n.pos.x());
    const quint64 attach = target.output; // 0 = the target's output is disconnected
    for (GradeNode n : source.nodes) {
        n.id += shift;
        for (GradeWindow& w : n.windows)
            w.id += shift;
        for (quint64& in : n.inputs)
            in = in == 0 ? attach : in + shift;
        n.pos.rx() += maxX + kNodeSpacingX - minX;
        target.nodes.append(n);
    }
    target.output = source.output ? source.output + shift : 0;
    return true;
}

void placeNode(ClipGrade& grade, quint64 id)
{
    GradeNode* n = grade.node(id);
    if (!n)
        return;
    const GradeNode* in = grade.node(n->inputs.value(0, 0));
    n->pos = in ? in->pos + QPointF(kNodeSpacingX, 0) : QPointF(0, 0);
}

} // namespace gradeops

} // namespace kk
