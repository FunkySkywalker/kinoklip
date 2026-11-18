// SPDX-License-Identifier: GPL-3.0-or-later
// CompLoader — reads the pictures of the Compositing page's Loader node (spec 128) from disk: a
// still image, a numbered image sequence (any one file of it names the whole sequence) or a movie
// file. Used by the CPU and the GPU evaluation alike. Thread-safe (one lock; decoders are kept
// for the last few files).
#pragma once

#include "core/Animation.h"

#include <QImage>
#include <QSize>
#include <QSizeF>
#include <QString>

namespace kk::render {

/// What a Loader file is.
struct LoaderClip
{
    enum class Kind { None, Still, Sequence, Movie };
    Kind kind = Kind::None;
    int count = 0; ///< frames (1 for a still image)
    QSize size; ///< picture size of the first frame
    [[nodiscard]] bool ok() const { return kind != Kind::None && count > 0; }
    [[nodiscard]] bool still() const { return kind == Kind::Still; }
};

class CompLoader
{
public:
    /// Looks at @p path (cached; a changed file is looked at again). Kind::None when it cannot
    /// be read.
    [[nodiscard]] static LoaderClip describe(const QString& path);
    /// Frame @p index (0-based, 0 .. count - 1) as premultiplied ARGB32 at its own size, at most
    /// @p maxSize when that is not empty (movies decode scaled; images are scaled after). A null
    /// image when the file or the frame cannot be read.
    [[nodiscard]] static QImage frame(const QString& path, int index, QSize maxSize = {});
    /// The picture of a Loader node with parameters @p params at comp frame @p compFrame as an
    /// image of @p canvas px (the timeline frame is @p frameSize px): the clip frame its timing
    /// picks (render/CompTimeMath.h), placed like a clip; transparent when nothing shows.
    [[nodiscard]] static QImage picture(
        const ParamSet& params, int64_t compFrame, QSize canvas, QSizeF frameSize);
    /// Forgets everything held (tests; files changed on disk).
    static void clear();

    /// @p picture drawn like a clip at zoom 1 into a transparent image of @p canvas px: fitted
    /// into the timeline frame of @p frameSize timeline px (the aspect ratio kept) and centred.
    [[nodiscard]] static QImage placed(const QImage& picture, QSize canvas, QSizeF frameSize);
};

} // namespace kk::render
