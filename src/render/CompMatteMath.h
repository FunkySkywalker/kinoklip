// SPDX-License-Identifier: GPL-3.0-or-later
// CompMatteMath — the float reference maths of the Compositing page keyers (Delta Keyer, Ultra
// Keyer) and Matte Control: matte generation from a screen colour, the pre-matte and matte filters
// (erode / dilate, blur, gamma, clean, threshold, restore fringe), garbage and solid mattes, spill
// suppression, fringe colour correction and the replace modes. The CPU evaluation runs these;
// the GPU passes (GlCompMatte) compute the same and the render tests compare them.
// A matte picture holds the matte in all four channels.
#pragma once

#include "core/Animation.h"
#include "render/CompFilterMath.h"

namespace kk::render::compmath {

/// The Matte tab: what is done to a matte after it is keyed (also Matte Control's controls).
struct MatteFilterParams
{
    int filter = GaussianBlur;
    int passes = 4;
    double erode = 0; ///< timeline px, positive dilates (Contract/Expand), negative erodes
    double blur = 0; ///< timeline px
    float gamma = 1, cleanBg = 0, cleanFg = 0, low = 0, high = 1, restoreFringe = 0;
    bool invert = false;
};
[[nodiscard]] MatteFilterParams matteFilterParams(const ParamSet& p, int64_t frame);
/// Gamma, Clean Background / Foreground and the thresholds on one matte value.
[[nodiscard]] float matteShape(const MatteFilterParams& m, float value);
/// Restore Fringe: @p filtered the matte before the shape, @p shaped after it, @p near the
/// blurred shaped matte (how close the pixel is to the solid part).
[[nodiscard]] float matteRestore(const MatteFilterParams& m, float filtered, float shaped, float near);
/// Erode / dilate, blur, shape, restore fringe and invert of the matte picture @p matte.
[[nodiscard]] FImage matteFilter(const FImage& matte, const MatteFilterParams& m, double scale);

enum KeyerKind { DeltaKind = 0, UltraKind = 1 };

struct KeyerParams
{
    KeyerKind kind = DeltaKind;
    float bg[3] = {0, 0.78f, 0}; ///< screen colour, straight
    float gain = 1, balance = 0.5f, soft = 0; ///< Delta: Gain, Balance, Pre-Matte Soft Range
    float levels[3] = {1, 1, 1}; ///< Ultra: Red / Green / Blue Level
    float separation = 0; ///< Ultra: Matte Separation
    double preBlur = 0; ///< px, blur of the picture the key reads
    double closeSize = 0; ///< Ultra: Pre Matte Size, px
    MatteFilterParams pre; ///< erode and blur of the pre-matte only
    MatteFilterParams matte;
    int spill = 2; ///< spillmethods:: index (Medium)
    float despill = 1;
    float fringeGamma = 1, fringeSize = 0, fringeShape = 0, fringeColor[3] = {0, 0, 0};
    int replace = 0; ///< replacemodes:: index
    float replaceColor[3] = {0.5f, 0.5f, 0.5f};
    bool subtractBackground = false;
    bool solidInvert = false, garbageInvert = false;
    int view = 0; ///< viewmodes:: index
};
[[nodiscard]] KeyerParams keyerParams(const ParamSet& p, int64_t frame, KeyerKind kind);

/// The raw matte (1 = keep) of straight colour @p c against screen colour @p bg.
[[nodiscard]] float keyMatte(const KeyerParams& k, const float c[3], const float bg[3]);
/// Colour of the keyed pixel: @p src the straight source colour, @p m the final matte, @p bg the
/// screen colour at this pixel. Straight, clamped.
void keyColor(const KeyerParams& k, const float src[3], float m, const float bg[3], float out[3]);
/// Matte of the solid and garbage mattes @p solid / @p garbage (alpha of the picture, null = not
/// connected) applied to matte value @p m at one pixel's mask alphas.
[[nodiscard]] float applyMattes(const KeyerParams& k, float m, const float* solidAlpha, const float* garbageAlpha);

/// The keyer: @p src the premultiplied picture, @p plate the clean plate (or null; the screen
/// colour is then the Background Color everywhere), @p garbage / @p solid mattes (or null).
/// @p pre and @p matteOut, when given, receive the pre-matte and the final matte pictures.
[[nodiscard]] FImage keyer(const FImage& src, const FImage* plate, const FImage* garbage,
    const FImage* solid, const KeyerParams& k, double scale);

/// Matte Control: the alpha of @p src filtered and combined with the mattes; colour stays.
struct MatteControlParams
{
    MatteFilterParams matte;
    bool solidInvert = false, garbageInvert = false;
};
[[nodiscard]] MatteControlParams matteControlParams(const ParamSet& p, int64_t frame);
[[nodiscard]] FImage matteControl(const FImage& src, const FImage* garbage, const FImage* solid,
    const MatteControlParams& m, double scale);

} // namespace kk::render::compmath
