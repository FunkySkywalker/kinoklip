// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectPresets implementation.
#include "core/ProjectPresets.h"

#include "core/Serialization.h"
#include "core/TimelineData.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace kk::projectpresets {

namespace {

constexpr char kFormatName[] = "kinoklip-project-preset";
constexpr int kMaxDimension = 8192;
constexpr int kMinDimension = 16;
constexpr int kMaxNameLength = 80;

QString& configOverride()
{
    static QString dir;
    return dir;
}

QString configRoot()
{
    if (!configOverride().isEmpty())
        return configOverride();
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString trimmedName(const QString& name)
{
    return name.simplified().left(kMaxNameLength);
}

/// File name for a preset name: letters, digits and a few signs; anything else becomes "_".
QString baseFileName(const QString& name)
{
    QString out;
    for (const QChar c : name) {
        out += (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_')
                   || c == QLatin1Char('.'))
            ? c
            : QLatin1Char('_');
    }
    while (out.startsWith(QLatin1Char('.')))
        out.remove(0, 1);
    return out.isEmpty() ? QStringLiteral("preset") : out;
}

struct Entry
{
    QString path;
    Preset preset;
};

std::optional<QJsonObject> readObject(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("The file could not be opened.");
        return std::nullopt;
    }
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QObject::tr("The file is not a project settings preset.");
        return std::nullopt;
    }
    return doc.object();
}

QList<Entry> entries()
{
    QList<Entry> out;
    const QDir dir(presetsDirectory());
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files);
    for (const QFileInfo& info : files) {
        const auto object = readObject(info.absoluteFilePath(), nullptr);
        if (!object)
            continue;
        auto preset = fromJson(*object);
        if (preset && !preset->name.isEmpty())
            out.append({info.absoluteFilePath(), std::move(*preset)});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        return QString::compare(a.preset.name, b.preset.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

bool writeJson(const QString& path, const QJsonObject& object)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return file.commit();
}

} // namespace

QJsonObject toJson(const Preset& preset)
{
    const ProjectSettings& s = preset.settings;
    QJsonObject settings {{QLatin1String("width"), s.width}, {QLatin1String("height"), s.height},
        {QLatin1String("fpsNum"), s.fps.num}, {QLatin1String("fpsDen"), s.fps.den},
        {QLatin1String("retimeProcess"), int(s.retimeProcess)},
        {QLatin1String("timelineStart"), qint64(s.timelineStart)},
        {QLatin1String("stdVideoTransition"), s.stdVideoTransition},
        {QLatin1String("stdAudioTransition"), s.stdAudioTransition},
        {QLatin1String("stdTransitionFrames"), qint64(s.stdTransitionFrames)},
        {QLatin1String("toneMap"), serialization::toneMapToJson(s.toneMap)}};
    return {{QLatin1String("format"), QLatin1String(kFormatName)},
        {QLatin1String("version"), kFormatVersion}, {QLatin1String("name"), preset.name},
        {QLatin1String("settings"), settings}, {QLatin1String("cache"), preset.cache.toJson()}};
}

std::optional<Preset> fromJson(const QJsonObject& json, QString* error)
{
    if (json.value(QLatin1String("format")).toString() != QLatin1String(kFormatName)
        || !json.value(QLatin1String("settings")).isObject()) {
        if (error)
            *error = QObject::tr("The file is not a project settings preset.");
        return std::nullopt;
    }
    Preset preset = factoryDefault();
    preset.name = trimmedName(json.value(QLatin1String("name")).toString());
    const QJsonObject o = json.value(QLatin1String("settings")).toObject();
    ProjectSettings& s = preset.settings;
    const auto intIn = [&o](const char* key, int lo, int hi, int fallback) {
        const QJsonValue v = o.value(QLatin1String(key));
        if (!v.isDouble())
            return fallback;
        const double d = v.toDouble();
        return d >= lo && d <= hi ? int(d) : fallback;
    };
    s.width = intIn("width", kMinDimension, kMaxDimension, s.width);
    s.height = intIn("height", kMinDimension, kMaxDimension, s.height);
    const int num = intIn("fpsNum", 1, 1'000'000, 0);
    const int den = intIn("fpsDen", 1, 1'000'000, 0);
    if (num > 0 && den > 0) {
        const Rational fps {num, den};
        if (fps.toDouble() >= 1.0 && fps.toDouble() <= 1000.0)
            s.fps = fps;
    }
    const int process = intIn("retimeProcess", 0, 255, 0);
    if (process == int(RetimeProcess::Nearest) || process == int(RetimeProcess::FrameBlend)
        || process == int(RetimeProcess::OpticalFlow))
        s.retimeProcess = RetimeProcess(process);
    s.timelineStart = oneHourTimecode(s.fps);
    if (o.value(QLatin1String("timelineStart")).isDouble())
        s.timelineStart = std::max<int64_t>(
            0, int64_t(o.value(QLatin1String("timelineStart")).toDouble()));
    s.stdVideoTransition = o.value(QLatin1String("stdVideoTransition")).toString();
    s.stdAudioTransition = o.value(QLatin1String("stdAudioTransition")).toString();
    s.stdTransitionFrames
        = intIn("stdTransitionFrames", 0, 100000, int(s.stdTransitionFrames));
    s.toneMap = serialization::toneMapFromJson(o.value(QLatin1String("toneMap")).toObject());
    preset.cache = MediaCacheSettings::fromJson(json.value(QLatin1String("cache")).toObject());
    return preset;
}

QString presetsDirectory()
{
    return configRoot() + QStringLiteral("/project-presets");
}

QString userDefaultPath()
{
    return configRoot() + QStringLiteral("/project-default.json");
}

void setConfigDirectoryForTests(const QString& configDir)
{
    configOverride() = configDir;
}

QList<PresetInfo> list()
{
    QList<PresetInfo> out;
    for (const Entry& e : entries())
        out.append({e.preset.name, e.preset.settings.width, e.preset.settings.height,
            e.preset.settings.fps});
    return out;
}

std::optional<Preset> load(const QString& name)
{
    const QString wanted = trimmedName(name);
    for (const Entry& e : entries()) {
        if (QString::compare(e.preset.name, wanted, Qt::CaseInsensitive) == 0)
            return e.preset;
    }
    return std::nullopt;
}

bool save(const Preset& preset)
{
    Preset stored = preset;
    stored.name = trimmedName(preset.name);
    if (stored.name.isEmpty())
        return false;
    QString path;
    for (const Entry& e : entries()) {
        if (QString::compare(e.preset.name, stored.name, Qt::CaseInsensitive) == 0) {
            path = e.path;
            break;
        }
    }
    if (path.isEmpty()) {
        const QString base = presetsDirectory() + QLatin1Char('/') + baseFileName(stored.name);
        path = base + QStringLiteral(".json");
        for (int n = 2; QFile::exists(path); ++n)
            path = base + QLatin1Char('-') + QString::number(n) + QStringLiteral(".json");
    }
    return writeJson(path, toJson(stored));
}

bool remove(const QString& name)
{
    const QString wanted = trimmedName(name);
    for (const Entry& e : entries()) {
        if (QString::compare(e.preset.name, wanted, Qt::CaseInsensitive) == 0)
            return QFile::remove(e.path);
    }
    return false;
}

bool writeFile(const QString& path, const Preset& preset)
{
    return writeJson(path, toJson(preset));
}

std::optional<Preset> readFile(const QString& path, QString* error)
{
    const auto object = readObject(path, error);
    return object ? fromJson(*object, error) : std::nullopt;
}

std::optional<Preset> userDefault()
{
    if (!QFile::exists(userDefaultPath()))
        return std::nullopt;
    return readFile(userDefaultPath());
}

bool setUserDefault(const Preset& preset)
{
    return writeJson(userDefaultPath(), toJson(preset));
}

bool resetUserDefault()
{
    return !QFile::exists(userDefaultPath()) || QFile::remove(userDefaultPath());
}

Preset factoryDefault()
{
    Preset preset;
    preset.settings.width = 1920;
    preset.settings.height = 1080;
    preset.settings.fps = Rational {24, 1};
    preset.settings.timelineStart = oneHourTimecode(preset.settings.fps);
    return preset;
}

Preset fromProject(const Project& project, const QString& name)
{
    Preset preset;
    preset.name = name;
    preset.settings = project.settings();
    preset.cache = project.cacheSettings();
    return preset;
}

} // namespace kk::projectpresets
