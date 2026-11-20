// SPDX-License-Identifier: GPL-3.0-or-later
// GradeLut — "Generate 3D LUT (CUBE)": bakes a grade into a 3D look-up table. Every lattice
// colour runs through the grade's node graph (render::gradePixel(), the maths the viewer and
// the render use), so the table applied with the Apply LUT effect or a LUT node gives the
// grade's colours. Only what is a pure colour mapping can go into a LUT: windows, qualifiers,
// the Blur palette, Midtone Detail and tracking depend on the picture and are left out (like
// Resolve's generated LUTs), the node Mix, bypass, curves, primaries, the RGB Mixer, LUT and
// AgX stages are baked. The values are clamped to 0..1 (the table's domain).
#pragma once

#include "core/Grade.h"

#include <QByteArray>
#include <QString>

#include <cstdint>

namespace kk::render {

/// Sizes offered by the clip menu (points per axis).
inline constexpr int kGradeLutSizes[] = {17, 33, 65};
inline constexpr int kMaxGradeLutSize = 129;

/// The .cube text of @p grade at clip-local @p frame with @p size points per axis (clamped to
/// 2..kMaxGradeLutSize), red changing fastest, TITLE @p title. An empty grade gives the identity.
[[nodiscard]] QByteArray gradeToCube(
    const ClipGrade& grade, int64_t frame, int size, const QString& title);

/// Writes gradeToCube() to @p path. @return false with a reason in @p error.
bool writeGradeCube(const ClipGrade& grade, int64_t frame, int size, const QString& title,
    const QString& path, QString* error = nullptr);

} // namespace kk::render
