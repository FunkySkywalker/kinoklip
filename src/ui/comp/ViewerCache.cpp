// SPDX-License-Identifier: GPL-3.0-or-later
// Disk cache of the Compositing viewers, see ViewerCache.h.
#include "ui/comp/ViewerCache.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <QDateTime>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace kk::viewercache {

namespace {

constexpr quint32 kMagic = 0x4b4b5643; // "KKVC"
constexpr int kVersion = 1;
constexpr const char* kExtension = ".kkv";

QString pathFor(const QByteArray& key)
{
    return directory() + QLatin1Char('/') + QString::fromLatin1(key) + QLatin1String(kExtension);
}

} // namespace

QString directory()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/comp-viewer");
    QDir().mkpath(dir);
    return dir;
}

qint64 limit()
{
    const int mb = QSettings().value(QStringLiteral("compositing/viewerCacheMb"), 0).toInt();
    return mb > 0 ? qint64(mb) * 1024 * 1024 : kDefaultLimit;
}

QByteArray makeKey(const QByteArray& content, quint64 node, int64_t frame, QSize size, int flags)
{
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(content);
    QByteArray tail;
    QDataStream s(&tail, QIODevice::WriteOnly);
    s << kVersion << node << qint64(frame) << qint32(size.width()) << qint32(size.height())
      << qint32(flags);
    h.addData(tail);
    return h.result().toHex();
}

bool store(const QByteArray& key, const QImage& image)
{
    if (image.isNull())
        return false;
    const QImage img = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QByteArray pixels(reinterpret_cast<const char*>(img.constBits()), qsizetype(img.sizeInBytes()));
    QSaveFile file(pathFor(key));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QDataStream out(&file);
    out << kMagic << qint32(kVersion) << qint32(img.width()) << qint32(img.height())
        << qint32(img.bytesPerLine());
    // Level 1: fast; pictures with flat areas still shrink a lot.
    out << qCompress(pixels, 1);
    if (!file.commit())
        return false;
    return true;
}

QImage load(const QByteArray& key)
{
    QFile file(pathFor(key));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QDataStream in(&file);
    quint32 magic = 0;
    qint32 version = 0, w = 0, h = 0, stride = 0;
    QByteArray packed;
    in >> magic >> version >> w >> h >> stride >> packed;
    if (in.status() != QDataStream::Ok || magic != kMagic || version != kVersion || w <= 0 || h <= 0
        || w > 32768 || h > 32768)
        return {};
    const QByteArray pixels = qUncompress(packed);
    if (pixels.size() != qsizetype(stride) * h || stride < w * 4)
        return {};
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    if (img.bytesPerLine() != stride)
        return {};
    std::memcpy(img.bits(), pixels.constData(), size_t(pixels.size()));
    file.close();
    // Counts as recently used.
    QFile touch(file.fileName());
    if (touch.open(QIODevice::ReadWrite)) {
        touch.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
    }
    return img;
}

bool contains(const QByteArray& key)
{
    return QFileInfo::exists(pathFor(key));
}

Usage usage()
{
    Usage u;
    const QDir dir(directory());
    for (const QFileInfo& f : dir.entryInfoList({QStringLiteral("*") + QLatin1String(kExtension)}, QDir::Files)) {
        u.bytes += f.size();
        ++u.files;
    }
    return u;
}

int trim(qint64 maxBytes)
{
    if (maxBytes < 0)
        maxBytes = limit();
    QFileInfoList files = QDir(directory()).entryInfoList(
        {QStringLiteral("*") + QLatin1String(kExtension)}, QDir::Files, QDir::Time | QDir::Reversed);
    qint64 total = 0;
    for (const QFileInfo& f : files)
        total += f.size();
    int removed = 0;
    for (const QFileInfo& f : files) { // oldest first
        if (total <= maxBytes)
            break;
        total -= f.size();
        if (QFile::remove(f.absoluteFilePath()))
            ++removed;
    }
    return removed;
}

void trimSometimes()
{
    static std::atomic<int> counter {0};
    if (counter.fetch_add(1) % 40 == 39)
        trim();
}

int clear()
{
    int removed = 0;
    for (const QFileInfo& f : QDir(directory()).entryInfoList({QStringLiteral("*") + QLatin1String(kExtension)}, QDir::Files))
        removed += QFile::remove(f.absoluteFilePath()) ? 1 : 0;
    return removed;
}

} // namespace kk::viewercache
