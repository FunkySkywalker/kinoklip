// SPDX-License-Identifier: GPL-3.0-or-later
// ExrWriter — writes a picture as an OpenEXR file with FFmpeg's "exr" encoder (LGPL, no extra
// library): 16-bit half or 32-bit float channels, uncompressed, RLE or ZIP. The Saver node of the
// Compositing page uses it (spec 128.10). FFmpeg's encoder has no PIZ / DWAA.
#pragma once

#include <QImage>
#include <QString>
#include <QVector>

namespace kk::render::exr {

/// Channel depth stored in the file (Saver "Bit Depth").
inline const QString kHalf = QStringLiteral("half");
inline const QString kFloat = QStringLiteral("float");
/// Compression values (Saver "Compression"): none, rle, zip1 (ZIP, one scan line per block),
/// zip16 (ZIP, 16 scan lines per block).
inline const QString kNone = QStringLiteral("none");
inline const QString kRle = QStringLiteral("rle");
inline const QString kZip1 = QStringLiteral("zip1");
inline const QString kZip16 = QStringLiteral("zip16");

struct Options
{
    QString depth = kHalf; ///< kHalf or kFloat
    QString compression = kZip16; ///< kNone, kRle, kZip1 or kZip16
    /// True: the sRGB colour of the image is converted to linear light (what compositing tools
    /// write to EXR); false: the values are written as they are, 0..1.
    bool linear = true;
};

/// True when this FFmpeg build has the exr encoder.
[[nodiscard]] bool available();

/// Writes @p image (Qt's ARGB32 / premultiplied) to @p file as premultiplied RGBA, the EXR
/// convention. False with a reason in @p error.
bool write(const QImage& image, const QString& file, const Options& options, QString* error);

/// What an EXR file holds, read back (header bytes + FFmpeg's exr decoder): tests and checks.
struct Info
{
    bool ok = false;
    int width = 0;
    int height = 0;
    bool hasAlpha = false;
    int bitsPerChannel = 0; ///< 16 (half) or 32 (float), from the header
    QString compression; ///< kNone, kRle, kZip1, kZip16, or "other"
    QVector<float> rgba; ///< width * height * 4: R, G, B, A of every pixel, row by row
    /// R, G, B, A of pixel (@p x, @p y); empty outside the picture.
    [[nodiscard]] QVector<float> pixel(int x, int y) const;
};
[[nodiscard]] Info read(const QString& file);

} // namespace kk::render::exr
