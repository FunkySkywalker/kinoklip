// SPDX-License-Identifier: GPL-3.0-or-later
// PowerGradeStore implementation: one JSON file next to the application settings.
#include "core/PowerGradeStore.h"

#include "core/Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace kk {

namespace {
constexpr int kPowerGradeVersion = 1; ///< "formatVersion" of powergrades.json
}

PowerGradeStore::PowerGradeStore(const QString& filePath, QObject* parent)
    : QObject(parent)
    , m_gallery(new GradeGallery(this, tr("PowerGrade")))
    , m_filePath(filePath.isEmpty() ? defaultFilePath() : filePath)
{
    load();
    connect(m_gallery, &GradeGallery::changed, this, [this] {
        if (!m_loading)
            save();
    });
}

QString PowerGradeStore::defaultFilePath()
{
    // KK_POWERGRADE_FILE: another file (screenshots and tests keep the user's file untouched).
    if (const QString file = qEnvironmentVariable("KK_POWERGRADE_FILE"); !file.isEmpty())
        return file;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("powergrades.json"));
}

bool PowerGradeStore::load(QString* error)
{
    m_loading = true;
    bool ok = true;
    QJsonObject root;
    QFile file(m_filePath);
    if (file.exists()) {
        const QJsonDocument doc = file.open(QIODevice::ReadOnly)
            ? QJsonDocument::fromJson(file.readAll())
            : QJsonDocument();
        file.close();
        if (!doc.isObject()) {
            ok = false;
            if (error)
                *error = tr("%1 is not a valid PowerGrade file").arg(m_filePath);
            const QString backup = m_filePath + QStringLiteral(".bak");
            QFile::remove(backup);
            QFile::copy(m_filePath, backup);
            qCWarning(lcCore) << "powergrades unreadable:" << m_filePath << "copied to" << backup;
        } else {
            root = doc.object();
        }
    }
    m_gallery->clear();
    m_gallery->extraFromJson(root.value(QLatin1String("extra")).toObject());
    m_gallery->fromJson(root.value(QLatin1String("stills")).toArray());
    m_loading = false;
    return ok;
}

bool PowerGradeStore::save(QString* error) const
{
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QJsonObject root;
    root[QLatin1String("formatVersion")] = kPowerGradeVersion;
    root[QLatin1String("extra")] = m_gallery->extraToJson();
    root[QLatin1String("stills")] = m_gallery->toJson();
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(root).toJson()) < 0
        || !file.commit()) {
        if (error)
            *error = tr("Cannot write %1").arg(m_filePath);
        return false;
    }
    return true;
}

} // namespace kk
