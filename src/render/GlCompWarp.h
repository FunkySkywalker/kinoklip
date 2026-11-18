// SPDX-License-Identifier: GPL-3.0-or-later
// GlCompWarp — the GPU passes of the Compositing page warp nodes (Displace, Vector Distort, Grid
// Warp); each computes what render/CompWarpMath.h computes on the CPU. Owned by GlComposition.
#pragma once

#include "render/CompWarpMath.h"
#include "render/GlKit.h"

namespace kk::render {

class GlCompWarp
{
public:
    bool initialize(GlKit& kit, QString* why);
    void destroy(GlKit& kit);

    /// @p in displaced by @p map. Sizes in target pixels, @p scale = target px per timeline px.
    [[nodiscard]] PooledTarget displace(GlKit& kit, const RenderTarget& in, const RenderTarget& map,
        const compmath::DisplaceParams& d, double scale);
    [[nodiscard]] PooledTarget vectorDistort(GlKit& kit, const RenderTarget& in,
        const RenderTarget& map, const compmath::VectorDistortParams& v, double scale);
    [[nodiscard]] PooledTarget vectorMotionBlur(GlKit& kit, const RenderTarget& in,
        const RenderTarget& map, const compmath::VectorBlurParams& v, double scale);
    /// Empty handle when the grid moves nothing.
    [[nodiscard]] PooledTarget gridWarp(
        GlKit& kit, const RenderTarget& in, const compmath::GridWarpParams& g);

private:
    GlProgram m_displace, m_vector, m_grid, m_blur;
    GLint m_bF = -1, m_bSamples = -1, m_bBias = -1, m_bEdges = -1;
    GLint m_dA = -1, m_dCh = -1, m_dB = -1;
    GLint m_vF = -1, m_vSteps = -1, m_vEdges = -1;
    GLint m_gCols = -1, m_gRows = -1, m_gD = -1;
};

} // namespace kk::render
