// SPDX-License-Identifier: GPL-3.0-or-later
// GalleryStillFiles — Import Still / Export Still of the Color page's gallery. A still goes out as
// a picture (PNG, TIFF, JPEG or DPX, through Qt and FFmpeg) plus a grade file next to it with
// the same base name and the suffix ".kkgrade" (Resolve writes a .drx the same way; ours is JSON:
// {"kinoklipStill": 1, "name", "created", "grade", "image" (base64 JPEG)}). Import reads a
// picture (and the grade file beside it, when there is one) or a grade file alone.
#pragma once

#include "core/GradeGallery.h"

#include <QString>
#include <QStringList>

namespace kk::gallerystills {

/// Suffix of the grade file.
inline const QString kGradeSuffix = QStringLiteral("kkgrade");

/// Picture suffixes Export Still writes: png, tif, jpg and, when FFmpeg has the encoder, dpx.
[[nodiscard]] QStringList exportSuffixes();

/// Writes @p still to @p path: the picture in the format of its suffix (png when there is none
/// or it is unknown) and, unless @p withGrade is false, the grade file beside it. A path that
/// ends in ".kkgrade" writes only the grade file. False with a reason in @p error.
bool exportStill(const GalleryStill& still, const QString& path, bool withGrade, QString* error);

/// The grade file that goes with picture @p imagePath (same folder and base name).
[[nodiscard]] QString gradeFileFor(const QString& imagePath);

/// Reads a still from @p path: a grade file (the still's grade, name and picture) or a picture
/// (its grade comes from the grade file beside it, else it has none). @p still gets no id or
/// album. False with a reason in @p error.
bool importStill(const QString& path, GalleryStill* still, QString* error);

/// The still in grade-file JSON form and back (also used by tests).
[[nodiscard]] QJsonObject stillToFileJson(const GalleryStill& still);
[[nodiscard]] bool stillFromFileJson(const QJsonObject& json, GalleryStill* still);

} // namespace kk::gallerystills
