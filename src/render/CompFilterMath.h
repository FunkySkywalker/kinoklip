// SPDX-License-Identifier: GPL-3.0-or-later
// CompFilterMath — the float reference maths of the Compositing page nodes that look at
// neighbouring pixels (Soft Glow, Light Wrap, the matte filters of the keyers, Grid Warp,
// Displace, Vector Distort). The CPU evaluation (CpuComposition) runs these directly; the GPU
// passes (GlCompFilters) are written to give the same result, and the render tests compare the two.
// Images are premultiplied RGBA floats. Sizes are image pixels unless a name says timeline pixels.
#pragma once

#include "core/Animation.h"

#include <QImage>

#include <vector>

namespace kk::render::compmath {

/// A premultiplied RGBA float picture.
struct FImage
{
    int w = 0, h = 0;
    std::vector<float> d; ///< 4 floats per pixel, row 0 at the top

    FImage() = default;
    FImage(int width, int height)
        : w(width)
        , h(height)
        , d(size_t(width) * size_t(height) * 4, 0.0f)
    { }
    [[nodiscard]] bool isNull() const { return w <= 0 || h <= 0; }
    [[nodiscard]] float* at(int x, int y) { return &d[(size_t(y) * size_t(w) + size_t(x)) * 4]; }
    [[nodiscard]] const float* at(int x, int y) const
    {
        return &d[(size_t(y) * size_t(w) + size_t(x)) * 4];
    }
    /// Pixel with the coordinates clamped to the picture.
    [[nodiscard]] const float* clamped(int x, int y) const
    {
        return at(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1));
    }
};

[[nodiscard]] FImage toFloat(const QImage& premultipliedArgb);
/// ARGB32_Premultiplied with the colour clamped to the alpha.
[[nodiscard]] QImage toImage(const FImage& image);

/// Blur kinds of the Filter menus (blurfilters:: index order).
enum BlurKind { BoxBlur = 0, BartlettBlur = 1, MultiBoxBlur = 2, GaussianBlur = 3 };

/// Gaussian blur, standard deviation @p sigmaX / @p sigmaY image pixels (the discrete kernel
/// over [-r, r], r = ceil(3 sigma); edges clamp). A sigma under 0.25 leaves that axis alone.
[[nodiscard]] FImage gaussian(const FImage& in, double sigmaX, double sigmaY);
/// Box blur of radius @p rx / @p ry (every pixel within the radius counts equally, the fraction
/// weights the outermost one).
[[nodiscard]] FImage box(const FImage& in, double rx, double ry);
/// Erode (per-channel minimum) or dilate (maximum) over a square of half-width @p radius.
[[nodiscard]] FImage morphology(const FImage& in, double radius, bool dilate);
/// The blur of the Filter menus: @p sizeX / @p sizeY image pixels. Gaussian: sigma = size / 2;
/// Box: radius = size; Multi-Box: @p passes boxes of radius size / passes (so one pass is Box
/// and two are Bartlett); Bartlett: two boxes of radius size / 2.
[[nodiscard]] FImage blurByKind(
    const FImage& in, int kind, double sizeX, double sizeY, int passes);

// ---- Soft Glow ----

struct SoftGlowParams
{
    int filter = GaussianBlur;
    int passes = 4;
    float threshold = 0.5f, gain = 1.0f;
    double sizeX = 20, sizeY = 20; ///< timeline px
    float scale[4] = {1, 1, 1, 1};
    bool channel[4] = {true, true, true, true};
};
[[nodiscard]] SoftGlowParams softGlowParams(const ParamSet& p, int64_t frame);
/// The part of premultiplied @p c above the threshold (rgb = a * max(straight - threshold, 0);
/// alpha = a scaled by how far the brightest channel is above it).
void softGlowHighlight(const SoftGlowParams& g, const float c[4], float h[4]);
/// @p c plus the blurred highlights @p glow times Gain and Color Scale, channels off removed;
/// colour clamped to the alpha.
void softGlowCombine(const SoftGlowParams& g, const float c[4], const float glow[4], float o[4]);
[[nodiscard]] FImage softGlow(const FImage& in, const SoftGlowParams& g, double scale);

// ---- Light Wrap ----

struct LightWrapParams
{
    float intensity = 0.5f, saturation = 1.0f, lumaTolerance = 0.0f;
    double diffuse = 15, fgBlur = 10, bgBlur = 0; ///< timeline px
    bool lumaBased = true, glow = false, wrapOnly = false, useConstant = false;
    bool screen = false; ///< Highlight Merge: Screen instead of Add
    float constant[3] = {1, 1, 1};
};
[[nodiscard]] LightWrapParams lightWrapParams(const ParamSet& p, int64_t frame);
/// The wrap colour and edge weight at a pixel: @p bg the (blurred) background, @p ab the blurred
/// foreground alpha. rgb is the straight-colour wrap strength (premultiplied by the edge weight),
/// w[3] the edge weight.
void lightWrapEdge(const LightWrapParams& l, const float bg[4], float ab, float w[4]);
/// The final pixel: @p fg the foreground, @p wrap the diffused edge picture.
void lightWrapCombine(const LightWrapParams& l, const float fg[4], const float wrap[4], float o[4]);
[[nodiscard]] FImage lightWrap(
    const FImage& fg, const FImage& bg, const LightWrapParams& l, double scale);

} // namespace kk::render::compmath
