// SPDX-License-Identifier: GPL-3.0-or-later
// Scopes — waveform, parade, vectorscope and histogram counts of an image.
#include "render/Scopes.h"

#include "core/GradeMath.h"

#include <QString>

#include <algorithm>
#include <cmath>

namespace kk::scopes {

namespace {

/// Rec. 709 chroma scale factors: Cb = (B - Y) / 1.8556, Cr = (R - Y) / 1.5748.
constexpr double kCbScale = 1.8556;
constexpr double kCrScale = 1.5748;

/// Visits every pixel of @p image (converted to ARGB32_Premultiplied) with its column and
/// RGB in 0..1 (premultiplied over black).
template<typename F>
void forEachPixel(const QImage& image, F&& fn)
{
    const QImage img = image.format() == QImage::Format_ARGB32_Premultiplied
        ? image
        : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < img.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = line[x];
            fn(x, qRed(p) / 255.0f, qGreen(p) / 255.0f, qBlue(p) / 255.0f);
        }
    }
}

int levelOf(float v, int levels)
{
    return std::clamp(int(std::lround(v * float(levels - 1))), 0, levels - 1);
}

} // namespace

const char* scopeTypeId(ScopeType type)
{
    switch (type) {
    case ScopeType::WaveformLuma:
        return "waveform";
    case ScopeType::WaveformRgb:
        return "waveformRgb";
    case ScopeType::Parade:
        return "parade";
    case ScopeType::Vectorscope:
        return "vectorscope";
    case ScopeType::Histogram:
        return "histogram";
    case ScopeType::CieChromaticity:
        return "cie";
    }
    return "waveform";
}

const char* scopeScaleId(ScopeScale scale)
{
    switch (scale) {
    case ScopeScale::TenBit:
        return "10bit";
    case ScopeScale::EightBit:
        return "8bit";
    case ScopeScale::Percent:
        return "percent";
    case ScopeScale::Float:
        return "float";
    }
    return "10bit";
}

ScopeScale scopeScaleFromId(const QString& id)
{
    for (const ScopeScale s :
        {ScopeScale::TenBit, ScopeScale::EightBit, ScopeScale::Percent, ScopeScale::Float}) {
        if (id == QLatin1String(scopeScaleId(s)))
            return s;
    }
    return ScopeScale::TenBit;
}

QString scaleLabel(ScopeScale scale, int step)
{
    step = std::clamp(step, 0, 4);
    switch (scale) {
    case ScopeScale::TenBit:
        return QString::number(step == 4 ? 1023 : step * 256);
    case ScopeScale::EightBit:
        return QString::number(step == 4 ? 255 : step * 64);
    case ScopeScale::Percent:
        return QString::number(step * 25);
    case ScopeScale::Float:
        return QString::number(step * 0.25, 'f', 2);
    }
    return {};
}

ScopeType scopeTypeFromId(const QString& id)
{
    for (const ScopeType t : {ScopeType::WaveformLuma, ScopeType::WaveformRgb, ScopeType::Parade,
             ScopeType::Vectorscope, ScopeType::Histogram, ScopeType::CieChromaticity}) {
        if (id == QLatin1String(scopeTypeId(t)))
            return t;
    }
    return ScopeType::WaveformLuma;
}

WaveformData waveform(const QImage& image, bool rgb, int columns, int levels)
{
    WaveformData d;
    if (image.isNull() || levels < 2)
        return d;
    d.columns = columns > 0 ? columns : image.width();
    d.levels = levels;
    d.planes.assign(rgb ? 3 : 1, std::vector<uint32_t>(size_t(d.columns) * size_t(levels), 0));
    const int w = image.width();
    forEachPixel(image, [&](int x, float r, float g, float b) {
        const size_t column = size_t(int(int64_t(x) * d.columns / w));
        if (rgb) {
            const float c[3] = {r, g, b};
            for (int ch = 0; ch < 3; ++ch)
                ++d.planes[size_t(ch)][size_t(levelOf(c[ch], levels)) * size_t(d.columns) + column];
        } else {
            const float c[3] = {r, g, b};
            ++d.planes[0][size_t(levelOf(grademath::luma(c), levels)) * size_t(d.columns) + column];
        }
    });
    return d;
}

QPointF chroma(double r, double g, double b)
{
    const double y = grademath::kLumaR * r + grademath::kLumaG * g + grademath::kLumaB * b;
    return {(b - y) / kCbScale, (r - y) / kCrScale};
}

QPointF vectorscopeCell(QPointF cbCr, int size)
{
    const double n = size - 1;
    return {(cbCr.x() + 0.5) * n, (0.5 - cbCr.y()) * n};
}

VectorscopeData vectorscope(const QImage& image, int size)
{
    VectorscopeData d;
    if (image.isNull() || size < 2)
        return d;
    d.size = size;
    d.counts.assign(size_t(size) * size_t(size), 0);
    forEachPixel(image, [&](int, float r, float g, float b) {
        const QPointF cell = vectorscopeCell(chroma(r, g, b), size);
        const int x = std::clamp(int(std::lround(cell.x())), 0, size - 1);
        const int y = std::clamp(int(std::lround(cell.y())), 0, size - 1);
        ++d.counts[size_t(y) * size_t(size) + size_t(x)];
    });
    return d;
}

QPointF xyChromaticity(double r, double g, double b, bool* valid)
{
    auto lin = [](double v) { return std::pow(std::clamp(v, 0.0, 1.0), 2.4); };
    const double lr = lin(r), lg = lin(g), lb = lin(b);
    // Rec. 709 / sRGB primaries with D65 white to XYZ.
    const double x = 0.4124564 * lr + 0.3575761 * lg + 0.1804375 * lb;
    const double y = 0.2126729 * lr + 0.7151522 * lg + 0.0721750 * lb;
    const double z = 0.0193339 * lr + 0.1191920 * lg + 0.9503041 * lb;
    const double sum = x + y + z;
    if (sum < 1e-6) {
        if (valid)
            *valid = false;
        return {kD65x, kD65y};
    }
    if (valid)
        *valid = true;
    return {x / sum, y / sum};
}

QPointF cieCell(QPointF xy, int size)
{
    const double n = size - 1;
    return {xy.x() / kCieMax * n, (1.0 - xy.y() / kCieMax) * n};
}

CieData cieChromaticity(const QImage& image, int size)
{
    CieData d;
    if (image.isNull() || size < 2)
        return d;
    d.size = size;
    d.counts.assign(size_t(size) * size_t(size), 0);
    forEachPixel(image, [&](int, float r, float g, float b) {
        bool valid = false;
        const QPointF xy = xyChromaticity(r, g, b, &valid);
        if (!valid)
            return;
        const QPointF cell = cieCell(xy, size);
        const int x = std::clamp(int(std::lround(cell.x())), 0, size - 1);
        const int y = std::clamp(int(std::lround(cell.y())), 0, size - 1);
        ++d.counts[size_t(y) * size_t(size) + size_t(x)];
    });
    return d;
}

const std::vector<QPointF>& cieSpectralLocus()
{
    // CIE 1931 2 degree standard observer, chromaticity of the spectral colours (public data).
    static const std::vector<QPointF> locus = {{0.1741, 0.0050}, {0.1738, 0.0049},
        {0.1733, 0.0048}, {0.1726, 0.0048}, {0.1714, 0.0051}, {0.1689, 0.0069}, {0.1644, 0.0109},
        {0.1566, 0.0177}, {0.1440, 0.0297}, {0.1241, 0.0578}, {0.0913, 0.1327}, {0.0454, 0.2950},
        {0.0082, 0.5384}, {0.0139, 0.7502}, {0.0743, 0.8338}, {0.1547, 0.8059}, {0.2296, 0.7543},
        {0.3016, 0.6923}, {0.3731, 0.6245}, {0.4441, 0.5547}, {0.5125, 0.4866}, {0.5752, 0.4242},
        {0.6270, 0.3725}, {0.6658, 0.3340}, {0.6915, 0.3083}, {0.7079, 0.2920}, {0.7190, 0.2809},
        {0.7260, 0.2740}, {0.7300, 0.2700}, {0.7320, 0.2680}, {0.7334, 0.2666}, {0.7344, 0.2656},
        {0.7347, 0.2653}};
    return locus;
}

std::array<QPointF, 3> gamutPrimaries(int gamut)
{
    switch (gamut) {
    case 0: // Rec. 709
        return {QPointF(0.640, 0.330), QPointF(0.300, 0.600), QPointF(0.150, 0.060)};
    case 1: // DCI-P3 primaries, D65 white
        return {QPointF(0.680, 0.320), QPointF(0.265, 0.690), QPointF(0.150, 0.060)};
    default: // Rec. 2020
        return {QPointF(0.708, 0.292), QPointF(0.170, 0.797), QPointF(0.131, 0.046)};
    }
}

HistogramData histogram(const QImage& image, int bins)
{
    HistogramData d;
    if (image.isNull() || bins < 2)
        return d;
    d.bins = bins;
    for (auto& c : d.channels)
        c.assign(size_t(bins), 0);
    forEachPixel(image, [&](int, float r, float g, float b) {
        const float c[3] = {r, g, b};
        ++d.channels[0][size_t(levelOf(r, bins))];
        ++d.channels[1][size_t(levelOf(g, bins))];
        ++d.channels[2][size_t(levelOf(b, bins))];
        ++d.channels[3][size_t(levelOf(grademath::luma(c), bins))];
    });
    return d;
}

QImage scopeInput(const QImage& image, int maxWidth)
{
    if (image.isNull())
        return {};
    QImage img = image.width() > maxWidth ? image.scaledToWidth(maxWidth, Qt::SmoothTransformation)
                                          : image;
    if (img.format() != QImage::Format_ARGB32_Premultiplied)
        img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    return img;
}

} // namespace kk::scopes
