// SPDX-License-Identifier: GPL-3.0-or-later
// CompWarpMath — the float reference maths of the Compositing page warp nodes: Displace, Vector
// Distort and Grid Warp (the same grid maths as the Edit page Warper effect). The CPU evaluation
// runs these; the GPU passes (GlCompWarp) compute the same, and the render tests compare them.
// Positions: pixel centres at (x + 0.5, y + 0.5), row 0 at the top. Displacements in timeline
// pixels are multiplied by the image scale; "up" means towards row 0.
#pragma once

#include "core/Animation.h"
#include "render/CompFilterMath.h"

namespace kk::render::compmath {

/// Map channels of Displace (choice order).
enum MapChannel { MapRed = 0, MapGreen = 1, MapBlue = 2, MapAlpha = 3, MapLuma = 4 };
/// Value 0..1 of channel @p ch of premultiplied @p c (straight colour; luma Rec. 709).
[[nodiscard]] float mapChannel(const float c[4], int ch);
/// Bilinear sample of @p image at pixel position (@p x, @p y) in pixel-centre-at-0.5 terms
/// shifted by -0.5 (so (0, 0) is the centre of the first pixel), edge mode @p edges
/// (edgemodes:: index). Transparent outside for Canvas.
void sampleBilinear(const FImage& image, double x, double y, int edges, float out[4]);

// ---- Displace ----

struct DisplaceParams
{
    bool radial = false;
    int xChannel = MapRed, yChannel = MapGreen;
    float xRefraction = 20, yRefraction = 20; ///< timeline px at map value 1 above Offset
    float offset = 0.5f; ///< map value that displaces nothing
    float centre[2] = {0.5f, 0.5f}; ///< radial: 0..1, y from the bottom
    float lightPower = 0, lightAngle = 0; ///< shading from the map's slope, angle in degrees
    int edges = 0;
};
[[nodiscard]] DisplaceParams displaceParams(const ParamSet& p, int64_t frame);
/// The source position (pixels) output pixel (@p x, @p y) reads, and the shading factor.
void displaceAt(const DisplaceParams& d, const FImage& map, int x, int y, double scale,
    double src[2], float* shade);
[[nodiscard]] FImage displace(
    const FImage& in, const FImage& map, const DisplaceParams& d, double scale);

// ---- Vector Distort ----

struct VectorDistortParams
{
    float range = 32; ///< timeline px a channel value of 0 or 1 stands for
    float multiplier = 1;
    int steps = 1;
    int edges = 0;
};
[[nodiscard]] VectorDistortParams vectorDistortParams(const ParamSet& p, int64_t frame);
/// The vector (pixels, y up) the map holds at map position (@p x, @p y).
void vectorAt(const VectorDistortParams& v, const FImage& map, double x, double y, double scale,
    double out[2]);
[[nodiscard]] FImage vectorDistort(
    const FImage& in, const FImage& map, const VectorDistortParams& v, double scale);

// ---- Grid Warp ----

/// The grid of the Warper effect: displacement per grid point in content units (x across,
/// y down, 0..1 over the frame), row 0 at the top.
struct GridWarpParams
{
    int cols = 0, rows = 0;
    std::vector<float> d; ///< 2 floats per point, row by row
    [[nodiscard]] bool isIdentity() const;
};
[[nodiscard]] GridWarpParams gridWarpParams(
    const ParamSet& p, int64_t frame, double scale, int width, int height);
/// Where the source of content position @p t (0..1 each way) is: t - field(t) after the
/// fixed-point inversion the shader does.
void gridWarpSource(const GridWarpParams& g, const double t[2], double out[2]);
[[nodiscard]] FImage gridWarp(const FImage& in, const GridWarpParams& g);

} // namespace kk::render::compmath
