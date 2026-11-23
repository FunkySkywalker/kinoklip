// SPDX-License-Identifier: GPL-3.0-or-later
// DpxWriter — writes a picture as a DPX file with FFmpeg's "dpx" encoder (LGPL, no extra
// library): 8-bit RGB. Export Still of the Color page's gallery uses it.
#pragma once

#include <QImage>
#include <QString>

namespace kk::render::dpx {

/// True when this FFmpeg build has the dpx encoder.
[[nodiscard]] bool available();

/// Writes @p image (alpha dropped) to @p file. False with a reason in @p error.
bool write(const QImage& image, const QString& file, QString* error);

} // namespace kk::render::dpx
