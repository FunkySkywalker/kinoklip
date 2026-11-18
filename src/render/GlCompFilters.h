// SPDX-License-Identifier: GPL-3.0-or-later
// GlCompFilters — the GPU passes of the Compositing page nodes that look at neighbouring pixels
// (Soft Glow, Light Wrap, the keyer matte filters, Grid Warp, Displace, Vector Distort). Each
// pass computes what render/CompFilterMath.h computes on the CPU. Owned by GlComposition.
#pragma once

#include "render/CompFilterMath.h"
#include "render/GlKit.h"

namespace kk::render {

class GlCompFilters
{
public:
    bool initialize(GlKit& kit, QString* why);
    void destroy(GlKit& kit);

    /// The blur of the Filter menus (compmath::blurByKind()) of @p src, sizes in target pixels.
    /// Empty handle: nothing to blur (use @p src itself).
    [[nodiscard]] PooledTarget blur(GlKit& kit, const RenderTarget& src, int kind, double sizeX,
        double sizeY, int passes);

    [[nodiscard]] PooledTarget softGlow(GlKit& kit, const RenderTarget& in,
        const compmath::SoftGlowParams& g, double scale);
    [[nodiscard]] PooledTarget lightWrap(GlKit& kit, const RenderTarget& fg, const RenderTarget& bg,
        const compmath::LightWrapParams& l, double scale);

private:
    /// A new target of @p size, bound, blending off, @p program in use with its matrices set.
    PooledTarget pass(GlKit& kit, QSize size, const GlProgram& program);

    GlProgram m_highlight, m_glowCombine, m_alpha, m_wrapEdge, m_wrapCombine;
    GLint m_hlThreshold = -1, m_gcMul = -1;
    GLint m_weA = -1, m_weConst = -1, m_wcA = -1;
};

} // namespace kk::render
