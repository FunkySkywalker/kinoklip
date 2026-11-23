// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/GalleryStillFiles.h"

#include "core/Serialization.h"
#include "media/ImageLoader.h"
#include "render/DpxWriter.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonDocument>
#include <QSaveFile>

namespace kk::gallerystills {

namespace {

constexpr int kFileVersion = 1;

void setError(QString* error, const QString& text)
{
    if (error)
        *error = text;
}

bool writeJsonFile(const QString& path, const QJsonObject& json, QString* error)
{
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(json).toJson()) < 0
        || !out.commit()) {
        setError(error, QCoreApplication::translate("kk::gallerystills", "Cannot write %1").arg(path));
        return false;
    }
    return true;
}

} // namespace

QStringList exportSuffixes()
{
    QStringList list {QStringLiteral("png"), QStringLiteral("tif"), QStringLiteral("jpg")};
    if (render::dpx::available())
        list.append(QStringLiteral("dpx"));
    return list;
}

QString gradeFileFor(const QString& imagePath)
{
    const QFileInfo info(imagePath);
    return info.dir().filePath(info.completeBaseName() + QLatin1Char('.') + kGradeSuffix);
}

QJsonObject stillToFileJson(const GalleryStill& still)
{
    QJsonObject o {{QLatin1String("kinoklipStill"), kFileVersion}, {QLatin1String("name"), still.name},
        {QLatin1String("created"), still.createdMs},
        {QLatin1String("grade"), serialization::gradeToJson(still.grade)}};
    if (!still.image.isNull())
        o[QLatin1String("image")] = galleryImageToBase64(still.image);
    return o;
}

bool stillFromFileJson(const QJsonObject& json, GalleryStill* still)
{
    if (json.value(QLatin1String("kinoklipStill")).toInt() < 1 || !still)
        return false;
    still->name = json.value(QLatin1String("name")).toString();
    still->createdMs = json.value(QLatin1String("created")).toInteger();
    still->grade = serialization::gradeFromJson(json.value(QLatin1String("grade")).toObject());
    still->image = galleryImageFromBase64(json.value(QLatin1String("image")).toString());
    return true;
}

bool exportStill(const GalleryStill& still, const QString& path, bool withGrade, QString* error)
{
    const QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    if (suffix == kGradeSuffix)
        return writeJsonFile(path, stillToFileJson(still), error);
    if (still.image.isNull()) {
        setError(error, QCoreApplication::translate("kk::gallerystills", "The still has no picture"));
        return false;
    }
    if (suffix == QLatin1String("dpx")) {
        if (!render::dpx::write(still.image, path, error))
            return false;
    } else {
        // Qt writes png, tiff and jpeg; the picture is flattened for jpeg (no alpha).
        QImage image = still.image;
        const bool jpeg = suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg");
        if (jpeg)
            image = image.convertToFormat(QImage::Format_RGB32);
        QImageWriter writer(path, suffix.isEmpty() ? QByteArray("png") : QByteArray());
        if (jpeg)
            writer.setQuality(95);
        if (!writer.write(image)) {
            setError(error, writer.errorString());
            return false;
        }
    }
    return !withGrade || writeJsonFile(gradeFileFor(path), stillToFileJson(still), error);
}

bool importStill(const QString& path, GalleryStill* still, QString* error)
{
    if (!still)
        return false;
    const QFileInfo info(path);
    if (!info.isFile()) {
        setError(error, QCoreApplication::translate("kk::gallerystills", "%1 does not exist").arg(path));
        return false;
    }
    GalleryStill result;
    auto readGradeFile = [&](const QString& file, bool required) {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) {
            if (required)
                setError(error, QCoreApplication::translate("kk::gallerystills", "Cannot read %1").arg(file));
            return false;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (!stillFromFileJson(doc.object(), &result)) {
            if (required) {
                setError(error,
                    QCoreApplication::translate("kk::gallerystills", "%1 is not a Kinoklip grade file")
                        .arg(file));
            }
            return false;
        }
        return true;
    };
    if (info.suffix().compare(kGradeSuffix, Qt::CaseInsensitive) == 0) {
        if (!readGradeFile(path, true))
            return false;
        if (result.name.isEmpty())
            result.name = info.completeBaseName();
        *still = result;
        return true;
    }
    const media::LoadedImage loaded = media::ImageLoader::load(path);
    if (loaded.image.isNull()) {
        setError(error, loaded.error.isEmpty()
                ? QCoreApplication::translate("kk::gallerystills", "Cannot read the picture %1").arg(path)
                : loaded.error);
        return false;
    }
    const QString sidecar = gradeFileFor(path);
    if (QFileInfo::exists(sidecar))
        readGradeFile(sidecar, false);
    result.image = loaded.image;
    if (result.name.isEmpty())
        result.name = info.completeBaseName();
    *still = result;
    return true;
}

} // namespace kk::gallerystills
