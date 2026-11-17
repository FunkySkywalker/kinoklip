// SPDX-License-Identifier: GPL-3.0-or-later
// GradeMath — the colour maths of the Color page as plain CPU functions: primaries (offset,
// temperature / tint, lift / gamma / gain with lum mix, contrast / pivot, hue / saturation),
// curve splines baked into tables, HSL conversion, the qualifier key and the power window
// matte. The GLSL of render/GlGrade.cpp mirrors these functions line by line and
// render/CpuGrade.cpp runs them per pixel; test_graderender compares both.
#pragma once

#include "core/Grade.h"

#include <array>
#include <cstdint>
#include <vector>

namespace kk::grademath {

/// Rec. 709 luma weights (the pictures are display-referred Rec. 709 / sRGB-like RGB).
inline constexpr float kLumaR = 0.2126f;
inline constexpr float kLumaG = 0.7152f;
inline constexpr float kLumaB = 0.0722f;
/// Gamma wheel range: exponent = 2^(-kGammaRange * gamma), so gamma +0.5 halves the exponent.
inline constexpr float kGammaRange = 2.0f;
/// Temperature / tint strength: +1 multiplies red (temperature) / divides green (tint) by
/// 1 + this and the opposite channel by 1 - this.
inline constexpr float kTemperatureRange = 0.25f;
inline constexpr float kTintRange = 0.25f;
/// Samples per baked curve table (inclusive end points; linear interpolation between them).
inline constexpr int kCurveTableSize = 256;
/// Smallest qualifier softness (an edge of zero width would be undefined for smoothstep).
inline constexpr float kMinSoftness = 1e-4f;

/// Rec. 709 luma of straight RGB @p c.
[[nodiscard]] inline float luma(const float c[3])
{
    return kLumaR * c[0] + kLumaG * c[1] + kLumaB * c[2];
}

/// Primaries of a node evaluated at one frame (channel index 0 R, 1 G, 2 B, 3 master).
struct PrimaryValues
{
    float lift[4] = {0, 0, 0, 0};
    float gamma[4] = {0, 0, 0, 0};
    float gain[4] = {1, 1, 1, 1};
    float offset[4] = {0, 0, 0, 0};
    float contrast = 1.0f;
    float pivot = 0.435f;
    float saturation = 1.0f;
    float hueDegrees = 0.0f;
    float temperature = 0.0f;
    float tint = 0.0f;
    float lumMix = 1.0f;
    /// Hue rotation about the grey axis as a row-major 3x3 matrix (from hueDegrees; identity
    /// for 0). The GPU receives exactly these floats.
    float hueMatrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    /// The values of @p node at frame @p localFrame (hueMatrix filled in).
    [[nodiscard]] static PrimaryValues at(const GradeNode& node, int64_t localFrame);
    /// True if applyPrimaries() changes nothing.
    [[nodiscard]] bool isIdentity() const;
};

/// Applies the primaries to straight (un-premultiplied) RGB @p rgb in place. Order: offset,
/// temperature / tint, per-channel lift / gamma / gain, master lift / gamma / gain (mixed
/// between per-channel and luma-only by lumMix), contrast around the pivot, hue rotation about
/// the grey axis, saturation around Rec. 709 luma. No clamping.
void applyPrimaries(const PrimaryValues& p, float rgb[3]);

/// RGB Mixer palette of a node at one frame (row-major: out = m * in; row 0 makes the red
/// output). Monochrome replaces the matrix by one grey weighting of R, G and B.
struct MixerValues
{
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool mono = false;
    float grey[3] = {0.3f, 0.59f, 0.11f};
    bool preserveLum = false;

    /// The values of @p node at frame @p localFrame.
    [[nodiscard]] static MixerValues at(const GradeNode& node, int64_t localFrame);
    /// True if applyMixer() changes nothing.
    [[nodiscard]] bool isIdentity() const;
};

/// Smallest luminance / weight sum the Preserve Luminance rescale divides by.
inline constexpr float kMixerEpsilon = 1e-5f;

/// Applies the RGB Mixer to straight RGB @p rgb in place. Matrix mode: out = M in. Monochrome
/// (maths of darktable's channelmixer.c process_gray: weighted sum replicated into all three
/// channels, no clamp except a floor at 0): grey = max(w . in, 0). Preserve Luminance (our rule,
/// the exact Resolve rule is not public): matrix mode scales the result by luma(in) / luma(out)
/// when luma(out) > epsilon; Monochrome divides the weights by their sum, so the sliders act as
/// shares of one whole (when the sum is above epsilon).
void applyMixer(const MixerValues& v, float rgb[3]);

/// One lift / gamma / gain transfer: pow(max(gain * (x + lift * (1 - x)), 0), 2^(-2 gamma)).
[[nodiscard]] float liftGammaGain(float x, float lift, float gamma, float gain);

/// The curve of @p points (kind @p curve) at @p x: monotone cubic (Fritsch-Carlson, no
/// overshoot) through the points. Channel and lum / sat curves get neutral end points at
/// x = 0 and 1 when the points do not reach them; hue curves are periodic (x wraps). An empty
/// list returns the neutral value.
[[nodiscard]] double evaluateCurve(const CurvePoints& points, GradeCurve curve, double x);

/// Inserts a point at (@p x, @p y) into @p points (kept sorted; a point at the same x is
/// replaced). An empty hue / lum / sat curve also gets two neutral neighbours (1/12 away), so
/// dragging the new point bends the curve locally. Returns its index.
int addCurvePoint(CurvePoints& points, GradeCurve curve, double x, double y);

/// Baked curve tables of a node: sample i is the curve at x = i / (kCurveTableSize - 1).
/// Y/R/G/B tables hold the output value, hue-vs-hue the hue shift in turns (y - 0.5), the
/// saturation and luminance curves a factor (2 y).
struct CurveTables
{
    std::array<std::array<float, kCurveTableSize>, kGradeCurveCount> table {};
    std::array<bool, kGradeCurveCount> identity {};
    /// True if every curve is identity (the curve stage is skipped).
    bool allIdentity = true;

    /// Bakes @p curves.
    [[nodiscard]] static CurveTables bake(const GradeCurves& curves);
    /// Bit i set when curve i is not identity (the GPU's uCurveMask).
    [[nodiscard]] int mask() const;
    /// Table @p c at @p x (0..1, clamped), linearly interpolated between samples (the GPU
    /// fetches the two samples and interpolates the same way).
    [[nodiscard]] float sample(GradeCurve c, float x) const;
};

/// Applies the curves to RGB @p rgb in place: values clamped to 0..1, the Y curve on every
/// channel, the R / G / B curves, then the HSL curves (hue vs hue / sat / lum, lum vs sat, sat
/// vs sat; hue, saturation and luma taken from the colour entering this stage).
void applyCurves(const CurveTables& tables, float rgb[3]);

/// RGB (0..1) to HSL: hue in turns [0, 1), saturation and lightness 0..1.
void rgbToHsl(const float rgb[3], float hsl[3]);
/// HSL to RGB, inverse of rgbToHsl().
void hslToRgb(const float hsl[3], float rgb[3]);

/// GLSL-style smoothstep (edge0 < edge1 required).
[[nodiscard]] float smoothStep(float edge0, float edge1, float x);

/// Qualifier key (0..1) of straight RGB @p rgb (clamped to 0..1 first). 1 when the qualifier
/// is disabled; invert applies.
[[nodiscard]] float qualifierKey(const GradeQualifier& q, const float rgb[3]);

/// A window evaluated at one frame in picture pixels (the pixel grid the grade is rendered
/// on): everything the per-pixel coverage needs.
struct WindowValues
{
    int shape = 0; ///< GradeWindowShape
    bool invert = false;
    float opacity = 1.0f;
    float cx = 0, cy = 0; ///< centre, pixels
    float cosR = 1, sinR = 0; ///< rotation (clockwise in y-down pixels)
    float halfW = 0, halfH = 0; ///< half size, pixels
    float softIn = 0.5f, softOut = 0.5f; ///< pixels, at least half a pixel each
    std::vector<float> points; ///< Polygon: x, y pairs in window-local pixels (unrotated)
};

/// Where the graded picture lies in the pixel grid: picture pixel (x, y) (centre at +0.5)
/// maps to picture fractions u = (x + 0.5 - originX) / width, v = (y + 0.5 - originY) / height.
struct PictureGeometry
{
    double originX = 0, originY = 0; ///< top-left corner of the picture, pixels
    double width = 1, height = 1; ///< size of the picture, pixels
};

/// Position, size and angle of a window (picture fractions, y down; degrees clockwise).
struct WindowGeometry
{
    double cx = 0.5, cy = 0.5;
    double width = 0.5, height = 0.5;
    double rotation = 0.0;
};

/// The geometry of window @p w of @p node at @p localFrame as the picture shows it: the
/// window's own values moved, scaled and turned by the node's tracker data (GradeTrack) about
/// the track's pivot. @p aspect is picture width / height. Without tracker data: the window's
/// values.
[[nodiscard]] WindowGeometry trackedWindowGeometry(
    const GradeNode& node, const GradeWindow& w, int64_t localFrame, double aspect);
/// The inverse: the values a window must store so that it shows as @p shown at @p localFrame
/// (what a drag in the viewer sets while a track is present).
[[nodiscard]] WindowGeometry untrackedWindowGeometry(
    const GradeTrack& track, const WindowGeometry& shown, int64_t localFrame, double aspect);
/// A copy of @p w whose geometry keys are the constants of trackedWindowGeometry() (what the
/// viewer overlay draws). @p w itself when the node has no tracker data.
[[nodiscard]] GradeWindow trackedWindow(
    const GradeNode& node, const GradeWindow& w, int64_t localFrame, double aspect);

/// The renderable windows of @p node at @p localFrame (at most kMaxGradeWindows, polygons at
/// most kMaxWindowPoints points) in the pixels of @p geometry.
[[nodiscard]] std::vector<WindowValues> evaluateWindows(
    const GradeNode& node, int64_t localFrame, const PictureGeometry& geometry);

/// Coverage (0..1, before invert and opacity) of window @p w at pixel centre (@p px, @p py):
/// a signed distance (exact for the rectangle and polygon, first-order for the ellipse, the
/// distance to the line for the gradient) mapped through 1 - smoothstep(-softIn, softOut, d).
[[nodiscard]] float windowCoverage(const WindowValues& w, float px, float py);

/// Union matte of @p windows at pixel centre (@p px, @p py): 1 without windows.
[[nodiscard]] float windowsMatte(const std::vector<WindowValues>& windows, float px, float py);

// ---- Blur palette (Blur / Sharpen / Mist) ----

/// Blur radius at full strength (radius 1) as a fraction of the picture height, so the look is
/// the same at every resolution (about 32 px on a 1080 line picture).
inline constexpr float kBlurMaxSigma = 0.03f;
/// The fixed radius of the unsharp mask, fraction of the picture height.
inline constexpr float kSharpSigma = 0.003f;
/// Unsharp strength at radius 0 (Blur mode) or 1 (Sharpen mode, level 0.5).
inline constexpr float kSharpGain = 2.0f;
/// Widths of a Gaussian below this (pixels) are not blurred.
inline constexpr float kMinBlurSigma = 0.05f;
/// Most taps on each side of the centre.
inline constexpr int kMaxBlurTaps = 256;
/// Coring: details below coringSoftness * this (0..1 of full scale) are not sharpened.
inline constexpr float kCoringRange = 0.1f;

/// One step of the Blur palette on a node's picture: per colour channel a Gaussian of
/// sigmaH x sigmaV pixels (0 = none) of the node's picture, put back as
///   out = c + (b - c) mix + (c - b) sharp coring(|c - b|)
/// (b = the blurred channel; mix 1, sharp 0 = a blur; mix 0, sharp > 0 = an unsharp mask).
struct BlurStage
{
    float sigmaH[3] = {0, 0, 0};
    float sigmaV[3] = {0, 0, 0};
    float mix[3] = {0, 0, 0};
    float sharp[3] = {0, 0, 0};
    float coring = 0.0f; ///< 0..1 (Coring Softness)

    /// True if the stage changes the picture.
    [[nodiscard]] bool active() const;
    /// True if the three channels use the same sigmas (one blur serves all of them).
    [[nodiscard]] bool uniformSigma() const;
};

/// The Blur palette of a node at one frame: the Blur, Sharpen and Mist stages in that order.
/// Radius 0.5 is neutral in Blur and Sharpen mode; above it Blur blurs and Sharpen sharpens,
/// below it Blur sharpens and Sharpen blurs; Mist (a soft glow mixed over the picture) starts
/// at radius 0. H/V Ratio 0.5 blurs both ways, 0 only horizontally, 1 only vertically.
/// Scaling 0.5 = the nominal size (0 none, 1 twice).
struct BlurValues
{
    std::array<BlurStage, 3> stages;

    /// True if any stage changes the picture.
    [[nodiscard]] bool active() const;
    /// The values of @p node at @p localFrame for a picture @p pictureHeight pixels high.
    [[nodiscard]] static BlurValues at(
        const GradeNode& node, int64_t localFrame, double pictureHeight);
};

/// Taps on each side of the centre of a Gaussian of @p sigma pixels (0 = none).
[[nodiscard]] int blurTapRadius(float sigma);
/// Normalised weights (2 radius + 1 of them) of a Gaussian of @p sigma pixels.
[[nodiscard]] std::vector<float> blurKernel(float sigma);
/// Coring factor of detail @p detail (0..1): smoothstep(0, coring * kCoringRange, |detail|), 1
/// when coring is 0.
[[nodiscard]] float coringFactor(float detail, float coring);
/// One channel of a stage: colour @p c with blurred colour @p b.
[[nodiscard]] float blurCombine(float c, float b, float mix, float sharp, float coring);

/// Layer mixer step: lays node output @p o (fed from @p in, key @p alpha) over @p result:
/// result = (1 - alpha) result + alpha in + (o - in), per channel, in place. Equals
/// mix(result, graded, alpha) when o = mix(in, graded, alpha), with no division by alpha.
void layerOver(float result[3], const float in[3], const float o[3], float alpha);

/// Matte Finesse sizes (fractions of the picture height at value 1, so the look is the same
/// at every resolution): Clean Black / White and Shrink / Grow reach about 16 px on a 1080
/// line picture, Blur Radius a Gaussian sigma of about 22 px; Denoise is a median of up to
/// 7 x 7 pixels.
inline constexpr float kMatteMaxRadius = 0.015f;
inline constexpr float kMatteMaxSigma = 0.02f;
inline constexpr int kMatteMaxDenoise = 3;
inline constexpr int kMatteMaxTaps = 48;

/// The Matte Finesse values of a qualifier in pixels of one picture grid.
struct MatteFinesse
{
    int cleanBlack = 0; ///< opening radius
    int cleanWhite = 0; ///< closing radius
    int shrinkGrow = 0; ///< < 0 erodes, > 0 dilates by that many pixels
    int denoise = 0; ///< median radius
    float blurSigma = 0.0f;
    float ratio = 0.0f; ///< In / Out Ratio, -1..1
    [[nodiscard]] bool active() const;
    [[nodiscard]] static MatteFinesse at(const GradeQualifier& q, double pictureHeight);
};

/// In / Out Ratio: moves the soft edge of a blurred matte value @p blurred (0..1) inside
/// (@p ratio > 0) or outside (< 0) the original edge.
[[nodiscard]] float matteRatio(float blurred, float ratio);
/// Median of the (2 radius + 1)^2 window of the w x h matte @p src into @p dst (edges clamped).
void matteMedian(const std::vector<float>& src, std::vector<float>& dst, int w, int h, int radius);
/// Cleans the w x h matte @p m (0..1) in place, in the order: Clean Black, Clean White,
/// Denoise, Shrink / Grow, Blur Radius with In / Out Ratio. The GPU pass runs the same steps.
void applyMatteFinesse(const MatteFinesse& f, std::vector<float>& m, int w, int h);

} // namespace kk::grademath
