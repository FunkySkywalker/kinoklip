// SPDX-License-Identifier: GPL-3.0-or-later
// RotoAssist — snaps a point placed on a picture to the nearest strong edge (the Roto Assist of
// the Bezier mask). Pure maths on a QImage; no widgets (spec section 117).
#pragma once

#include <QImage>
#include <QPointF>

#include <optional>

namespace kk::roto {

/// The strongest edge near @p p (pixels of @p image): Sobel gradient magnitude of the
/// luminance (0 = flat, about 1 for a black / white step) inside a circle of @p radius pixels,
/// weighted towards the centre (score = magnitude / (1 + (distance / radius)^2)). Returns
/// nullopt when no pixel of the circle reaches @p minStrength, so the point stays where it is.
[[nodiscard]] std::optional<QPointF> snapToEdge(
    const QImage& image, QPointF p, double radius, double minStrength = 0.08);

} // namespace kk::roto
