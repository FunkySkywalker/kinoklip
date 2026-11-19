// SPDX-License-Identifier: GPL-3.0-or-later
// CompPaint — the Paint node of the Compositing page (spec 131): its strokes (core/PaintStrokes.h)
// drawn over the picture on its Input. One function does the drawing and both the GPU and the CPU
// renderer call it, so they give the same pixels. Strokes are vectors in frame fractions and are
// redrawn on every render at the size asked for, so they do not depend on the resolution.
#pragma once

#include "core/Composition.h"
#include "core/PaintStrokes.h"

#include <QImage>
#include <QSize>

#include <cstdint>

namespace kk::render::paint {

/// Strength of a brush at @p distance pixels from its centre: 1 inside, a smooth fall-off to 0 at
/// @p radius over the soft part (softness 0..1 of the radius, at least one pixel for a clean edge).
[[nodiscard]] float brushWeight(float distance, float radius, float softness);

/// @p input (null: transparent) with every stroke of @p strokes that is visible at comp @p frame
/// drawn over it, in order, as a premultiplied ARGB32 picture of @p size. Null when no stroke is
/// visible at the frame (the caller passes the input through).
[[nodiscard]] QImage drawStrokes(
    const QVector<kk::paint::Stroke>& strokes, int64_t frame, QSize size, const QImage& input);

/// drawStrokes() for the strokes of Paint node @p node.
[[nodiscard]] QImage renderNode(const CompNode& node, int64_t frame, QSize size, const QImage& input);

} // namespace kk::render::paint
