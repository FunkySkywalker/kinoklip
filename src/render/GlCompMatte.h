// SPDX-License-Identifier: GPL-3.0-or-later
// GlCompMatte — the GPU passes of the Compositing page keyers (Delta Keyer, Ultra Keyer) and
// Matte Control; each computes what render/CompMatteMath.h computes on the CPU. Owned by
// GlComposition.
#pragma once

#include "render/CompMatteMath.h"
#include "render/GlCompFilters.h"
#include "render/GlKit.h"

namespace kk::render {

class GlCompMatte
{
public:
    bool initialize(GlKit& kit, QString* why);
    void destroy(GlKit& kit);

    /// The keyed picture of @p src; @p plate, @p garbage and @p solid are null when not
    /// connected. @p fx supplies the blurs.
    [[nodiscard]] PooledTarget keyer(GlKit& kit, GlCompFilters& fx, const RenderTarget& src,
        const RenderTarget* plate, const RenderTarget* garbage, const RenderTarget* solid,
        const compmath::KeyerParams& k, double scale);
    [[nodiscard]] PooledTarget matteControl(GlKit& kit, GlCompFilters& fx, const RenderTarget& src,
        const RenderTarget* garbage, const RenderTarget* solid,
        const compmath::MatteControlParams& m, double scale);

private:
    /// Erode / blur / shape / restore fringe / invert of the matte picture @p matte. Empty handle
    /// when nothing changes.
    PooledTarget matteFilter(GlKit& kit, GlCompFilters& fx, const RenderTarget& matte,
        const compmath::MatteFilterParams& m, double scale);
    PooledTarget run(GlKit& kit, QSize size, const GlProgram& program);

    GlProgram m_key, m_shape, m_restore, m_final, m_alpha, m_control, m_max;
    GLint m_kBg = -1, m_kTune = -1, m_kLevels = -1, m_kFlags = -1;
    GLint m_sShape = -1, m_sThr = -1;
    GLint m_rThr = -1;
    GLint m_fBg = -1, m_fTune = -1, m_fLevels = -1, m_fFlags = -1, m_fFlags2 = -1, m_fSpill = -1,
          m_fFringe = -1, m_fReplace = -1, m_fView = -1;
    GLint m_cFlags = -1;
};

} // namespace kk::render
