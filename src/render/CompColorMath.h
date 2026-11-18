// SPDX-License-Identifier: GPL-3.0-or-later
// CompColorMath — the float reference maths of the Compositing page colour nodes of spec 123:
// Brightness Contrast, Alpha Multiply / Divide, Channel Mapper, Color Curves, Hue Curves and the
// Custom Tool. The CPU evaluation (CpuComposition) runs these on straight (not premultiplied)
// colour; the GPU shaders of GlCompColor are written to give the same result and the render
// tests compare the two. Values are 0..1 per channel; every node clamps its result to 0..1.
#pragma once

#include "core/Animation.h"
#include "core/Expression.h"
#include "core/GradeMath.h"

#include <QString>
#include <QStringList>

#include <array>
#include <memory>

namespace kk::render::compmath {

// ---- Brightness Contrast ----

struct BrightnessContrastParams
{
    bool channel[4] = {true, true, true, false}; ///< Red, Green, Blue, Alpha switches
    float gain = 1, lift = 0, gamma = 1, contrast = 0, brightness = 0, saturation = 1;
    float low = 0, high = 1; ///< the input range stretched to 0..1
    [[nodiscard]] bool isIdentity() const;
};
[[nodiscard]] BrightnessContrastParams brightnessContrastParams(const ParamSet& p, int64_t frame);
/// One channel value through the chain: range, gain, lift, gamma, contrast, brightness.
[[nodiscard]] float brightnessContrastChannel(const BrightnessContrastParams& b, float v);
/// @p rgba straight colour, in place.
void applyBrightnessContrast(const BrightnessContrastParams& b, float rgba[4]);

// ---- Alpha Multiply / Divide ----

/// Straight colour times alpha (Alpha Multiply) or divided by it, kept at most 1 (Alpha Divide).
void alphaMultiply(float rgba[4]);
void alphaDivide(float rgba[4]);

// ---- Channel Mapper ----

struct ChannelMapperParams
{
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}; ///< [out][in]
    float offset[4] = {0, 0, 0, 0};
    [[nodiscard]] bool isIdentity() const;
};
[[nodiscard]] ChannelMapperParams channelMapperParams(const ParamSet& p, int64_t frame);
void applyChannelMapper(const ChannelMapperParams& c, float rgba[4]);

// ---- Color Curves and Hue Curves ----

/// Curve tables of a Color Curves / Hue Curves node: the Grade tables (Y, R, G, B, hue vs hue,
/// hue vs sat, hue vs lum, lum vs sat, sat vs sat) and the alpha table.
struct CurvesNodeParams
{
    grademath::CurveTables tables;
    std::array<float, grademath::kCurveTableSize> alpha {};
    bool alphaOn = false;
    [[nodiscard]] bool isIdentity() const { return tables.allIdentity && !alphaOn; }
};
[[nodiscard]] CurvesNodeParams colorCurvesParams(const ParamSet& p);
[[nodiscard]] CurvesNodeParams hueCurvesParams(const ParamSet& p);
void applyCurvesNode(const CurvesNodeParams& c, float rgba[4]);

// ---- Custom Tool ----

/// The variable names of a Custom Tool formula, in the order of the values handed to evaluate:
/// time, x, y, w, h, n1..n8, r1 g1 b1 a1, r2 g2 b2 a2, i1..i4.
[[nodiscard]] const QStringList& customToolVariables();
inline constexpr int kCustomNumbers = 8;
inline constexpr int kCustomInters = 4;

struct CustomToolParams
{
    std::shared_ptr<const expr::Program> inter[kCustomInters]; ///< null: the value 0
    std::shared_ptr<const expr::Program> channel[4]; ///< null: the channel of Image 1 as it is
    QString error[kCustomInters + 4]; ///< why a formula did not compile (empty: fine)
    double n[kCustomNumbers] = {0, 0, 0, 0, 0, 0, 0, 0};
    [[nodiscard]] bool isIdentity() const;
};
[[nodiscard]] CustomToolParams customToolParams(const ParamSet& p, int64_t frame);
/// The formula texts of a node (Intermediate 1 to 4, then Red, Green, Blue, Alpha): the stored
/// string, or the default of that field.
[[nodiscard]] QString customToolText(const ParamSet& p, int field);
/// One pixel: @p x / @p y 0..1 (y up), @p w / @p h the frame size in timeline px, @p img1 /
/// @p img2 straight rgba (img2 transparent when the node has no Image 2), result straight rgba.
void customToolPixel(const CustomToolParams& c, double time, double x, double y, double w,
    double h, const float img1[4], const float img2[4], float out[4]);
/// The GLSL statements of the body of main(): they read the floats v_time, v_x, ... of the
/// variables above (declared by the caller) and write v_i1..v_i4 and the vec4 `result` (straight).
[[nodiscard]] QString customToolGlsl(const CustomToolParams& c, const ParamSet& p);

} // namespace kk::render::compmath
