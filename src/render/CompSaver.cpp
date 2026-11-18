// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/CompSaver.h"

#include "core/NodeRegistry.h"
#include "core/TimelineData.h"
#include "render/Compositor.h"

#include <QDir>
#include <QFileInfo>

#include <cmath>

namespace kk::render::compsaver {

QVector<quint64> activeSavers(const Composition& comp)
{
    QVector<quint64> out;
    for (const CompNode& n : comp.nodes) {
        if (n.type != nodetypes::Saver || !n.enabled)
            continue;
        if (!n.params.staticValue(nodekeys::SaverActive, true).toBool())
            continue;
        if (n.params.staticValue(nodekeys::SaverPath).toString().trimmed().isEmpty())
            continue;
        out.append(n.id);
    }
    return out;
}

QString fileName(const QString& base, int number, const QString& format)
{
    const QFileInfo info(base);
    const QString extension = format == saverformats::Tiff ? QStringLiteral("tif") : QStringLiteral("png");
    return QDir(info.path()).filePath(
        QStringLiteral("%1%2.%3").arg(info.completeBaseName()).arg(number, 4, 10, QLatin1Char('0')).arg(extension));
}

bool writeImage(const QImage& image, const QString& file, const QString& format, QString* error)
{
    if (image.isNull()) {
        if (error)
            *error = QStringLiteral("no picture");
        return false;
    }
    // The compositions hold premultiplied colour; files hold straight colour.
    const QImage straight = image.convertToFormat(QImage::Format_ARGB32);
    const bool ok = straight.save(
        file, format == saverformats::Tiff ? "TIFF" : "PNG");
    if (!ok && error)
        *error = QStringLiteral("cannot write %1").arg(file);
    return ok;
}

Result run(Compositor& compositor, std::shared_ptr<const TimelineData> timeline, quint64 clipId,
    int64_t first, int64_t last, const std::function<bool(int, int)>& progress,
    std::shared_ptr<const MediaPathMap> media)
{
    Result result;
    if (!timeline)
        return result;
    const ClipData* clip = nullptr;
    for (const TrackData& track : timeline->videoTracks) {
        const int i = track.indexOfClip(clipId);
        if (i >= 0)
            clip = &track.clips[size_t(i)];
    }
    if (!clip || !clip->composition)
        return result;
    const QVector<quint64> savers = activeSavers(*clip->composition);
    const int total = int(savers.size() * std::max<int64_t>(0, last - first + 1));
    int done = 0;
    for (const quint64 id : savers) {
        const CompNode* node = clip->composition->node(id);
        const QString base = node->params.staticValue(nodekeys::SaverPath).toString();
        const QString format = node->params.staticValue(nodekeys::SaverFormat, saverformats::Png).toString();
        const int start = int(std::lround(node->params.value(nodekeys::SaverStart, 0, 0.0)));
        QDir().mkpath(QFileInfo(base).path());
        for (int64_t f = first; f <= last; ++f) {
            if (progress && !progress(done, total)) {
                result.cancelled = true;
                return result;
            }
            const RenderedFramePtr frame = compositor
                                               .renderCompositionNode(timeline, clipId, id, f, {},
                                                   Quality::Full, OutputImage, media)
                                               .get();
            QString error;
            if (!frame || !frame->ok) {
                result.errors << QStringLiteral("frame %1 could not be rendered").arg(f);
            } else if (writeImage(frame->image,
                           fileName(base, start + int(f - first), format), format, &error)) {
                ++result.files;
            } else {
                result.errors << error;
            }
            ++done;
        }
    }
    if (progress)
        progress(done, total);
    return result;
}

} // namespace kk::render::compsaver
