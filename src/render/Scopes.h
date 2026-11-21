// SPDX-License-Identifier: GPL-3.0-or-later
// Scopes — video scope data of a picture (Color page, Edit page workspace window): waveform
// (luma or RGB overlaid), RGB parade, vectorscope and histogram, computed on the CPU from a
// small (downscaled) frame. Pure functions of an image; the widgets in src/ui/scopes draw
// them. Values are the gamma-encoded Rec. 709 RGB of the rendered frame ("premultiplied over
// black", as delivered), luma with the Rec. 709 weights.
#pragma once

#include <QImage>
#include <QPointF>

#include <array>
#include <cstdint>
#include <vector>

namespace kk::scopes {

/// Which scope a view shows.
enum class ScopeType : quint8 {
    WaveformLuma, ///< luma level per column
    WaveformRgb, ///< R, G and B levels per column, overlaid
    Parade, ///< R, G and B waveforms side by side
    Vectorscope, ///< Cb / Cr scatter (colour hue and saturation)
    Histogram, ///< R, G, B and luma value counts
    CieChromaticity ///< CIE 1931 xy chromaticity of every pixel on the spectral locus
};

/// The numbers beside the waveforms' and the histogram's levels (the scope settings' Scale).
enum class ScopeScale : quint8 {
    TenBit, ///< 0 - 1023 (the default)
    EightBit, ///< 0 - 255
    Percent, ///< 0 - 100
    Float ///< 0.0 - 1.0
};
/// Stable ids ("10bit", "8bit", "percent", "float").
[[nodiscard]] const char* scopeScaleId(ScopeScale scale);
[[nodiscard]] ScopeScale scopeScaleFromId(const QString& id);
/// The label of graticule line @p step (0 .. 4, black to white in quarters) on @p scale.
[[nodiscard]] QString scaleLabel(ScopeScale scale, int step);
/// Range of the scope settings' Gain (a multiplier of the trace brightness) and its default.
inline constexpr double kMinScopeGain = 0.25;
inline constexpr double kMaxScopeGain = 4.0;
inline constexpr double kDefaultScopeGain = 1.0;

/// Stable string ids ("waveform", "waveformRgb", "parade", "vectorscope", "histogram").
[[nodiscard]] const char* scopeTypeId(ScopeType type);
[[nodiscard]] ScopeType scopeTypeFromId(const QString& id);

/// Waveform counts: for each channel plane (1 for luma, 3 for RGB: R, G, B) a grid of
/// `columns` x `levels` counters, row-major by level (level 0 = black). Pixel column x of an
/// image of width W lands in column x * columns / W; value v (0..1) in level
/// round(v * (levels - 1)).
struct WaveformData
{
    int columns = 0;
    int levels = 0;
    std::vector<std::vector<uint32_t>> planes;

    /// Count of plane @p plane at (@p column, @p level).
    [[nodiscard]] uint32_t at(int plane, int column, int level) const
    {
        return planes[size_t(plane)][size_t(level) * size_t(columns) + size_t(column)];
    }
};

/// Waveform of @p image: luma only (@p rgb false) or the three channels. @p columns <= 0 uses
/// the image width.
[[nodiscard]] WaveformData waveform(
    const QImage& image, bool rgb, int columns = 0, int levels = 256);

/// Vectorscope counts on a size x size grid, row-major, row 0 at the top: Cb to the right,
/// Cr up, the centre is neutral grey, the edge of the grid is |Cb| or |Cr| = 0.5.
struct VectorscopeData
{
    int size = 0;
    std::vector<uint32_t> counts;
    [[nodiscard]] uint32_t at(int x, int y) const
    {
        return counts[size_t(y) * size_t(size) + size_t(x)];
    }
};

/// Cb / Cr (Rec. 709, -0.5..0.5) of gamma-encoded RGB (0..1).
[[nodiscard]] QPointF chroma(double r, double g, double b);
/// Grid cell (fractional, 0..size - 1) of chroma @p cbCr on a vectorscope of @p size.
[[nodiscard]] QPointF vectorscopeCell(QPointF cbCr, int size);
/// Vectorscope of @p image on a @p size grid.
[[nodiscard]] VectorscopeData vectorscope(const QImage& image, int size = 256);

/// Largest x and y of the CIE scope's grid (the plot shows 0 .. kCieMax on both axes).
inline constexpr double kCieMax = 0.85;
/// CIE 1931 chromaticity counts on a size x size grid, row 0 at the top (y up).
struct CieData
{
    int size = 0;
    std::vector<uint32_t> counts;
    [[nodiscard]] uint32_t at(int x, int y) const
    {
        return counts[size_t(y) * size_t(size) + size_t(x)];
    }
};
/// CIE 1931 xy chromaticity of gamma-encoded Rec. 709 RGB (0..1): decoded with the BT.1886
/// display gamma 2.4, mapped to XYZ with the Rec. 709 primaries (D65). False in @p valid (and
/// the white point) for black, which has no chromaticity.
[[nodiscard]] QPointF xyChromaticity(double r, double g, double b, bool* valid = nullptr);
/// Grid cell (fractional, 0..size - 1) of chromaticity @p xy on a CIE scope of @p size.
[[nodiscard]] QPointF cieCell(QPointF xy, int size);
/// CIE chromaticity scope of @p image on a @p size grid (black pixels are skipped).
[[nodiscard]] CieData cieChromaticity(const QImage& image, int size = 256);
/// The spectral locus of the CIE 1931 2 degree observer, 380 to 700 nm in steps of 10 nm (the
/// closing purple line joins the last point to the first).
[[nodiscard]] const std::vector<QPointF>& cieSpectralLocus();
/// Red, green and blue primaries of a gamut the scope can outline: Rec. 709, DCI-P3 (D65) and
/// Rec. 2020.
[[nodiscard]] std::array<QPointF, 3> gamutPrimaries(int gamut);
inline constexpr int kGamutCount = 3;
/// D65 white point.
inline constexpr double kD65x = 0.3127;
inline constexpr double kD65y = 0.3290;

/// Histogram: `bins` counters per channel (R, G, B, luma), value v in bin round(v (bins - 1)).
struct HistogramData
{
    int bins = 0;
    std::array<std::vector<uint32_t>, 4> channels; ///< R, G, B, luma
};
[[nodiscard]] HistogramData histogram(const QImage& image, int bins = 256);

/// The picture a scope is computed from: @p image reduced to at most @p maxWidth pixels wide
/// (aspect kept, smooth), ARGB32_Premultiplied. Scopes of a 1080p frame from a 480 wide copy
/// cost about a millisecond.
[[nodiscard]] QImage scopeInput(const QImage& image, int maxWidth = 480);

} // namespace kk::scopes
