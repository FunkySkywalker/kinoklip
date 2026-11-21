// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/TransitionPresets.h"

#include "core/Serialization.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace kk::transitionpresets {

namespace {
constexpr int kFormat = 1;
constexpr const char* kExtension = ".kktransition";
QString g_directory;
}

void setDirectory(const QString& path)
{
    g_directory = path;
}

QString directory()
{
    const QString dir = g_directory.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/transition-presets")
        : g_directory;
    QDir().mkpath(dir);
    return dir;
}

QString baseName(const QString& name)
{
    QString out;
    for (const QChar c : name.trimmed())
        out.append(c.isLetterOrNumber() ? c : QLatin1Char('-'));
    return out.isEmpty() ? QStringLiteral("transition") : out;
}

QString idOf(const QString& name)
{
    return kIdPrefix + baseName(name);
}

QString pathFor(const QString& name)
{
    return directory() + QLatin1Char('/') + baseName(name) + QLatin1String(kExtension);
}

QString save(const Preset& preset, QString* error)
{
    const QString path = pathFor(preset.name);
    const QJsonObject o {{QStringLiteral("kinoklipTransition"), kFormat},
        {QStringLiteral("name"), preset.name}, {QStringLiteral("duration"), qint64(preset.duration)},
        {QStringLiteral("composition"), serialization::compositionToJson(preset.composition)}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(o).toJson()) < 0
        || !file.commit()) {
        if (error)
            *error = file.errorString();
        return {};
    }
    return path;
}

std::optional<Preset> load(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
    if (o.value(QStringLiteral("kinoklipTransition")).toInt() < 1
        || !o.value(QStringLiteral("composition")).isObject())
        return std::nullopt;
    Preset p;
    p.name = o.value(QStringLiteral("name")).toString();
    p.duration = std::max<int64_t>(1, o.value(QStringLiteral("duration")).toInteger(25));
    p.composition = serialization::compositionFromJson(o.value(QStringLiteral("composition")).toObject());
    if (p.name.isEmpty() || p.composition.nodes.isEmpty())
        return std::nullopt;
    return p;
}

std::optional<Preset> loadById(const QString& id)
{
    if (!id.startsWith(kIdPrefix))
        return std::nullopt;
    const QString path = directory() + QLatin1Char('/') + id.mid(kIdPrefix.size())
        + QLatin1String(kExtension);
    return QFileInfo::exists(path) ? load(path) : std::nullopt;
}

QVector<Entry> list()
{
    QVector<Entry> out;
    const QDir dir(directory());
    for (const QFileInfo& f : dir.entryInfoList({QStringLiteral("*") + QLatin1String(kExtension)},
             QDir::Files, QDir::Name)) {
        if (const auto p = load(f.absoluteFilePath()))
            out.append({p->name, f.absoluteFilePath()});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    return out;
}

bool remove(const QString& name)
{
    return QFile::remove(pathFor(name));
}

bool rename(const QString& name, const QString& newName)
{
    const QString trimmed = newName.trimmed();
    if (trimmed.isEmpty())
        return false;
    auto preset = load(pathFor(name));
    if (!preset)
        return false;
    if (baseName(trimmed) != baseName(name) && QFileInfo::exists(pathFor(trimmed)))
        return false;
    const QString oldPath = pathFor(name);
    preset->name = trimmed;
    if (save(*preset).isEmpty())
        return false;
    if (pathFor(trimmed) != oldPath)
        QFile::remove(oldPath);
    return true;
}

Composition stretched(const Composition& comp, int64_t fromFrames, int64_t toFrames)
{
    Composition out = comp;
    if (fromFrames < 1 || toFrames < 1 || fromFrames == toFrames)
        return out;
    const double scale = double(toFrames + 1) / double(fromFrames + 1);
    for (CompNode& node : out.nodes) {
        for (AnimatedValue& value : node.params.values) {
            for (Keyframe& key : value.keys)
                key.frame = int64_t(std::llround(double(key.frame + 1) * scale)) - 1;
            // Keys that landed on one frame (a very short transition): keep the later one.
            QVector<Keyframe> kept;
            for (const Keyframe& key : std::as_const(value.keys)) {
                if (!kept.isEmpty() && kept.last().frame == key.frame)
                    kept.last() = key;
                else
                    kept.append(key);
            }
            value.keys = kept;
        }
    }
    return out;
}

TransitionData makeTransition(const Preset& preset, int64_t durationFrames)
{
    TransitionData t;
    t.type = kCompositionTransition;
    t.duration = durationFrames;
    t.composition = stretched(preset.composition, preset.duration, durationFrames);
    return t;
}

} // namespace kk::transitionpresets
