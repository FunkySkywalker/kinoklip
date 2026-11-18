// SPDX-License-Identifier: GPL-3.0-or-later
// GlCompColor — the GPU passes of the Compositing page colour nodes of spec 123 (Brightness
// Contrast, Alpha Multiply / Divide, Channel Mapper, Color Curves, Hue Curves, Custom Tool);
// each computes what render/CompColorMath.h computes on the CPU. Owned by GlComposition.
#pragma once

#include "render/CompColorMath.h"
#include "render/GlKit.h"

#include <QHash>

#include <memory>

namespace kk::render {

class GlCompColor
{
public:
    bool initialize(GlKit& kit, QString* why);
    void destroy(GlKit& kit);

    /// Each returns an empty handle when the node changes nothing.
    [[nodiscard]] PooledTarget brightnessContrast(
        GlKit& kit, const RenderTarget& in, const compmath::BrightnessContrastParams& b);
    [[nodiscard]] PooledTarget alpha(GlKit& kit, const RenderTarget& in, bool divide);
    [[nodiscard]] PooledTarget channelMapper(
        GlKit& kit, const RenderTarget& in, const compmath::ChannelMapperParams& c);
    [[nodiscard]] PooledTarget curves(
        GlKit& kit, const RenderTarget& in, const compmath::CurvesNodeParams& c);
    /// @p second: Image 2 or null. @p frame: the frame size in timeline px.
    [[nodiscard]] PooledTarget customTool(GlKit& kit, const RenderTarget& in,
        const RenderTarget* second, const compmath::CustomToolParams& c, const ParamSet& params,
        double time, QSizeF frame);

private:
    struct Custom
    {
        GlProgram program;
        GLint n = -1, info = -1;
    };
    GlProgram m_bc, m_alpha, m_mapper, m_curves;
    GLint m_bcA = -1, m_bcB = -1, m_bcC = -1;
    GLint m_alDivide = -1;
    GLint m_mapM = -1, m_mapOff = -1;
    GLint m_cvMask = -1, m_cvAlpha = -1;
    GLuint m_curveTex = 0;
    /// Custom Tool programs by their GLSL body (the formulas change the shader).
    QHash<QString, std::shared_ptr<Custom>> m_custom;
};

} // namespace kk::render
