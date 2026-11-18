// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/CompLoader.h"

#include "core/NodeRegistry.h"
#include "media/ImageLoader.h"
#include "media/ImageSequence.h"
#include "media/VideoDecoder.h"
#include "render/CompTimeMath.h"

#include <QDateTime>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace kk::render {

namespace {

struct Entry
{
    QString path;
    QDateTime modified;
    LoaderClip clip;
    media::SequenceInfo sequence;
    std::unique_ptr<media::VideoDecoder> decoder;
    double fps = 25.0;
    qint64 used = 0;
};

constexpr size_t kMaxEntries = 6;
QMutex g_lock;
std::vector<std::unique_ptr<Entry>> g_entries;
qint64 g_tick = 0;

Entry* open(const QString& path)
{
    const QFileInfo info(path);
    const QDateTime modified = info.lastModified();
    for (auto& e : g_entries) {
        if (e->path == path && e->modified == modified) {
            e->used = ++g_tick;
            return e.get();
        }
    }
    // Replace a changed entry, or make room.
    std::erase_if(g_entries, [&](const auto& e) { return e->path == path; });
    if (g_entries.size() >= kMaxEntries) {
        const auto oldest = std::min_element(g_entries.begin(), g_entries.end(),
            [](const auto& a, const auto& b) { return a->used < b->used; });
        g_entries.erase(oldest);
    }
    auto e = std::make_unique<Entry>();
    e->path = path;
    e->modified = modified;
    e->used = ++g_tick;
    if (media::ImageSequence::isSequencePath(path)) {
        QString pattern;
        Rational rate;
        media::ImageSequence::parsePath(path, &pattern, &rate);
        e->sequence = media::ImageSequence::scan(path);
        if (e->sequence.found) {
            e->sequence.pattern = pattern;
            e->clip.kind = LoaderClip::Kind::Sequence;
        }
    } else if (info.isFile() && media::ImageLoader::handlesExtension(path)) {
        e->sequence = media::ImageSequence::detect(path);
        if (e->sequence.found) {
            e->clip.kind = LoaderClip::Kind::Sequence;
        } else {
            const media::ImageHeader header = media::ImageLoader::readHeader(path);
            if (header.valid) {
                e->clip.kind = LoaderClip::Kind::Still;
                e->clip.count = 1;
                e->clip.size = header.size;
            }
        }
    } else if (info.isFile()) {
        auto decoder = std::make_unique<media::VideoDecoder>();
        decoder->setThreadCount(1);
        if (decoder->open(path)) {
            const media::MediaInfo& mi = decoder->info();
            const double fps = mi.frameRate.isValid() ? mi.frameRate.toDouble() : 25.0;
            e->fps = fps;
            e->clip.kind = LoaderClip::Kind::Movie;
            e->clip.count = std::max(1, int(std::lround(double(mi.durationUs) * fps / 1e6)));
            e->clip.size = QSize(mi.displayWidth, mi.displayHeight);
            e->decoder = std::move(decoder);
        }
    }
    if (e->clip.kind == LoaderClip::Kind::Sequence) {
        e->clip.count = e->sequence.frameCount();
        const QString first = media::ImageSequence::fileForNumber(
            e->sequence.pattern, e->sequence.firstNumber);
        const media::ImageHeader header = media::ImageLoader::readHeader(first);
        e->clip.size = header.size;
        if (!header.valid)
            e->clip = LoaderClip();
    }
    g_entries.push_back(std::move(e));
    return g_entries.back().get();
}

} // namespace

LoaderClip CompLoader::describe(const QString& path)
{
    if (path.isEmpty())
        return {};
    QMutexLocker lock(&g_lock);
    return open(path)->clip;
}

QImage CompLoader::frame(const QString& path, int index, QSize maxSize)
{
    if (path.isEmpty())
        return {};
    QMutexLocker lock(&g_lock);
    Entry* e = open(path);
    if (!e->clip.ok() || index < 0 || index >= e->clip.count)
        return {};
    QImage image;
    switch (e->clip.kind) {
    case LoaderClip::Kind::Still:
        image = media::ImageLoader::loadCached(path).image;
        break;
    case LoaderClip::Kind::Sequence: {
        // A gap in the numbering repeats the previous file, like the sequence media does.
        for (int n = e->sequence.firstNumber + index; n >= e->sequence.firstNumber; --n) {
            const QString file = media::ImageSequence::fileForNumber(e->sequence.pattern, n);
            if (QFileInfo::exists(file)) {
                image = media::ImageLoader::loadCached(file).image;
                break;
            }
        }
        break;
    }
    case LoaderClip::Kind::Movie:
        image = e->decoder->frameAt(int64_t(std::floor(double(index) * 1e6 / e->fps)) + 10, maxSize);
        break;
    case LoaderClip::Kind::None:
        break;
    }
    if (image.isNull())
        return {};
    if (image.format() != QImage::Format_ARGB32_Premultiplied)
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    return image;
}

void CompLoader::clear()
{
    QMutexLocker lock(&g_lock);
    g_entries.clear();
    media::ImageLoader::clearCache();
}

QImage CompLoader::picture(
    const ParamSet& params, int64_t compFrame, QSize canvas, QSizeF frameSize)
{
    const QString path = params.staticValue(nodekeys::LoaderPath).toString();
    const LoaderClip clip = describe(path);
    if (!clip.ok())
        return placed({}, canvas, frameSize);
    const int index = timemath::loaderFrame(
        timemath::loaderTiming(params, compFrame), clip.count, clip.still(), compFrame);
    if (index < 0)
        return placed({}, canvas, frameSize);
    return placed(frame(path, index), canvas, frameSize);
}

QImage CompLoader::placed(const QImage& picture, QSize canvas, QSizeF frameSize)
{
    QImage out(canvas, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    if (picture.isNull() || frameSize.isEmpty())
        return out;
    // Fitted into the frame like a clip (aspect kept, never cropped), centred.
    const double fit = std::min(frameSize.width() / picture.width(), frameSize.height() / picture.height());
    const QSizeF content(picture.width() * fit, picture.height() * fit);
    const double scale = canvas.width() / frameSize.width();
    const QRectF target((frameSize.width() - content.width()) / 2 * scale,
        (frameSize.height() - content.height()) / 2 * scale, content.width() * scale,
        content.height() * scale);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(target, picture);
    return out;
}

} // namespace kk::render
