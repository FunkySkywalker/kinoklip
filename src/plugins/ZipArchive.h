// SPDX-License-Identifier: GPL-3.0-or-later
// ZipArchive — a small, dependency-free reader (stored and deflated entries) and writer (stored
// entries) of zip files, enough for .kkplugin packages. Reading is hardened: entry names that
// leave the target folder, huge or too many entries and bad checksums are refused.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace kk::plugins::zip {

/// Limits applied when reading an archive that came from somewhere else.
inline constexpr int kMaxEntries = 1000;
inline constexpr qint64 kMaxEntryBytes = 16 * 1024 * 1024;
inline constexpr qint64 kMaxTotalBytes = 64 * 1024 * 1024;

/// Extracts the zip file @p zipPath into the existing folder @p targetDir. Directories are
/// created as needed; no file is written outside @p targetDir. Returns false with @p error.
[[nodiscard]] bool extractAll(const QString& zipPath, const QString& targetDir, QString* error);

/// Names of the files in the archive (directories left out), or empty with @p error.
[[nodiscard]] QStringList listFiles(const QString& zipPath, QString* error);

/// Reads one file of the archive.
[[nodiscard]] bool readFile(
    const QString& zipPath, const QString& name, QByteArray* data, QString* error);

/// Writes the files of folder @p sourceDir (recursively, relative names, sorted, "/" separators)
/// into a new zip file @p zipPath (entries stored, not compressed: plugins are small text).
[[nodiscard]] bool createFromFolder(
    const QString& sourceDir, const QString& zipPath, QString* error);

/// Inflates raw deflate data (RFC 1951); false on corrupt data or when more than @p maxBytes
/// would come out. Exposed for tests.
[[nodiscard]] bool inflate(
    const QByteArray& in, qint64 expectedSize, qint64 maxBytes, QByteArray* out);

/// CRC-32 (zip polynomial).
[[nodiscard]] quint32 crc32(const QByteArray& data);

} // namespace kk::plugins::zip
