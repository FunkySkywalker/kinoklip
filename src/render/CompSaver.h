// SPDX-License-Identifier: GPL-3.0-or-later
// CompSaver — the Compositing page's Saver node (spec 128): writes the picture arriving at the
// node to image files when the composition is RENDERED (the page's Render Savers action, Deliver),
// one file per frame. While frames are only drawn for the viewers or for playback the Saver passes
// its input through and writes nothing.
#pragma once

#include "core/Composition.h"
#include "render/ExrWriter.h"
#include "render/RenderTypes.h"

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>

namespace kk {
class Compositor;
struct TimelineData;
}

namespace kk::render::compsaver {

/// The Savers of @p comp that write: enabled, "Save When Rendered" on, with a file name.
[[nodiscard]] QVector<quint64> activeSavers(const Composition& comp);

/// File name of frame number @p number: the extension of @p base is replaced by the one of
/// @p format ("png", "tiff", "exr"), the number (at least 4 digits) goes before it: ("/d/shot.png", 7,
/// "png") -> "/d/shot0007.png".
[[nodiscard]] QString fileName(const QString& base, int number, const QString& format);
/// Writes @p image (premultiplied ARGB32, as the compositions produce it) as @p format. False
/// with a reason in @p error. OpenEXR with @p exr (nullptr: half, ZIP, linear).
bool writeImage(const QImage& image, const QString& file, const QString& format, QString* error,
    const exr::Options* exr = nullptr);

struct Result
{
    int files = 0; ///< files written
    QStringList errors;
    bool cancelled = false;
};

/// Renders comp frames [@p first, @p last] of the composition of clip @p clipId of @p timeline
/// (a video clip of its tracks) and writes every active Saver's picture, numbered from the
/// Saver's First Number. @p progress(done, total) returns false to cancel. Blocks.
/// @p transition 1 / 2: the Savers are those of the graph of the clip's transitionIn / transitionOut
/// (comp frames 0 .. duration - 1 of the transition window; spec 128.10).
Result run(Compositor& compositor, std::shared_ptr<const TimelineData> timeline, quint64 clipId,
    int64_t first, int64_t last, const std::function<bool(int, int)>& progress = nullptr,
    std::shared_ptr<const MediaPathMap> media = nullptr, int transition = 0);

} // namespace kk::render::compsaver
