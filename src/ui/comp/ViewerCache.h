// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerCache — the Compositing viewers' disk cache (spec 121): pictures the viewer rendered,
// kept in the user's cache folder so scrubbing back over a frame, looping the clip or reopening
// the page shows it at once. The file name is a hash of everything that decides the picture
// (the clip, its composition, the node, the frame, the size and the viewer's quality switches),
// so an edit simply makes the old file unreachable; the folder is trimmed to a size limit,
// oldest files first.
#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

namespace kk::viewercache {

/// Default size limit of the cache folder in bytes (2 GiB; settings key
/// "compositing/viewerCacheMb" overrides it).
inline constexpr qint64 kDefaultLimit = 2048LL * 1024 * 1024;

/// The cache folder (created when missing): <cache>/comp-viewer.
[[nodiscard]] QString directory();
/// Size limit in bytes from the settings.
[[nodiscard]] qint64 limit();

/// A key (40 hex digits) from the parts that decide a viewer picture.
[[nodiscard]] QByteArray makeKey(const QByteArray& content, quint64 node, int64_t frame, QSize size,
    int flags);

/// Stores @p image (any format, converted to ARGB32 premultiplied) under @p key. False when the
/// file cannot be written.
bool store(const QByteArray& key, const QImage& image);
/// The picture stored under @p key, or a null image. Touches the file so it counts as recently
/// used.
[[nodiscard]] QImage load(const QByteArray& key);
/// True if a file for @p key exists.
[[nodiscard]] bool contains(const QByteArray& key);

/// Deletes the oldest files until the folder is within @p maxBytes (default: limit()). Returns
/// the number of files removed.
int trim(qint64 maxBytes = -1);
/// trim() once in about every 40 calls (after storing a picture: keeps the folder bounded without
/// listing it every time). Thread-safe.
void trimSometimes();
/// Deletes every file. Returns the number removed.
int clear();
/// Bytes and number of files in the folder now.
struct Usage
{
    qint64 bytes = 0;
    int files = 0;
};
[[nodiscard]] Usage usage();

} // namespace kk::viewercache
