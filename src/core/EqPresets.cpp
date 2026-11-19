// SPDX-License-Identifier: GPL-3.0-or-later
// EqPresets implementation.
#include "core/EqPresets.h"

#include "core/MixerJson.h"
#include "core/MixerOps.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace kk::eqpresets {

namespace {

constexpr char kFormatName[] = "kinoklip-eq-preset";
constexpr int kMaxNameLength = 60;

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

QString baseFileName(const QString& name)
{
    QString out;
    for (const QChar c : name) {
        out += (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_'))
            ? c
            : QLatin1Char('_');
    }
    return out.isEmpty() ? QStringLiteral("preset") : out;
}

EqBand band(EqBandType type, double hz, double gainDb = 0.0, double q = 0.707, bool on = true)
{
    EqBand b;
    b.type = type;
    b.enabled = on;
    b.frequencyHz = hz;
    b.gainDb = gainDb;
    b.q = q;
    return b;
}

/// A preset from the six bands in slot order (high pass, low shelf, two bells, high shelf, low
/// pass); a slot not mentioned stays neutral and off.
Preset make(const char* name, std::array<EqBand, kEqBandCount> bands)
{
    Preset p;
    p.name = QString::fromLatin1(name);
    p.factory = true;
    p.eq.enabled = true;
    p.eq.bands = bands;
    return p;
}

struct Entry
{
    QString path;
    Preset preset;
};

QList<Entry> entries()
{
    QList<Entry> out;
    const QDir dir(presetsDirectory());
    for (const QFileInfo& info : dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject()
            || doc.object().value(QLatin1String("format")).toString()
                != QLatin1String(kFormatName))
            continue;
        Preset p;
        p.name = trimmedName(doc.object().value(QLatin1String("name")).toString());
        p.eq = serialization::eqFromJson(doc.object().value(QLatin1String("eq")).toObject());
        // Same limits as the controls; a preset always switches the EQ on.
        ChannelStrip strip;
        strip.eq = p.eq;
        mixerops::clampStrip(strip);
        p.eq = strip.eq;
        p.eq.enabled = true;
        if (!p.name.isEmpty())
            out.append({info.absoluteFilePath(), p});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        return QString::compare(a.preset.name, b.preset.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

} // namespace

QList<Preset> factoryPresets()
{
    using T = EqBandType;
    const EqBand off1 = band(T::HighPass, 80.0, 0.0, 0.707, false);
    const EqBand off6 = band(T::LowPass, 16000.0, 0.0, 0.707, false);
    const EqBand off2 = band(T::LowShelf, 100.0, 0.0, 0.707);
    const EqBand off3 = band(T::Bell, 500.0, 0.0, 1.0);
    const EqBand off4 = band(T::Bell, 2000.0, 0.0, 1.0);
    const EqBand off5 = band(T::HighShelf, 8000.0, 0.0, 0.707);
    QList<Preset> out;
    Preset flat = make("Flat", {off1, off2, off3, off4, off5, off6});
    flat.eq.enabled = false;
    flat.eq.bands = defaultEqBands();
    out << flat;
    out << make("Voice", {band(T::HighPass, 80.0), band(T::LowShelf, 150.0, -2.0),
                   band(T::Bell, 300.0, -2.5, 1.0), band(T::Bell, 3500.0, 3.0, 1.0),
                   band(T::HighShelf, 9000.0, 2.0), off6});
    out << make("Music", {band(T::HighPass, 30.0), band(T::LowShelf, 100.0, 1.5),
                   band(T::Bell, 400.0, -1.5, 0.8), band(T::Bell, 3000.0, 1.0, 0.8),
                   band(T::HighShelf, 10000.0, 2.0), off6});
    out << make("Telephone", {band(T::HighPass, 400.0), off2, band(T::Bell, 1000.0, 4.0, 0.9),
                   off4, off5, band(T::LowPass, 3400.0)});
    out << make("Rumble Cut", {band(T::HighPass, 100.0), off2, off3, off4, off5, off6});
    out << make("De-Mud", {band(T::HighPass, 70.0), off2, band(T::Bell, 250.0, -4.0, 1.2),
                   band(T::Bell, 450.0, -2.0, 1.5), off5, off6});
    out << make("Podcast Voice", {band(T::HighPass, 80.0), band(T::LowShelf, 140.0, 2.0),
                   band(T::Bell, 250.0, -2.0, 1.0), band(T::Bell, 4000.0, 3.0, 1.0),
                   band(T::HighShelf, 10000.0, 1.5), band(T::LowPass, 16000.0)});
    return out;
}

QList<Preset> userPresets()
{
    QList<Preset> out;
    for (const Entry& e : entries())
        out.append(e.preset);
    return out;
}

bool isFactoryName(const QString& name)
{
    const QString wanted = trimmedName(name);
    for (const Preset& p : factoryPresets()) {
        if (QString::compare(p.name, wanted, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

Preset find(const QString& name)
{
    const QString wanted = trimmedName(name);
    for (const Preset& p : factoryPresets()) {
        if (QString::compare(p.name, wanted, Qt::CaseInsensitive) == 0)
            return p;
    }
    for (const Entry& e : entries()) {
        if (QString::compare(e.preset.name, wanted, Qt::CaseInsensitive) == 0)
            return e.preset;
    }
    return {};
}

bool saveUserPreset(const QString& name, const EqSettings& eq)
{
    const QString stored = trimmedName(name);
    if (stored.isEmpty() || isFactoryName(stored))
        return false;
    QString path;
    for (const Entry& e : entries()) {
        if (QString::compare(e.preset.name, stored, Qt::CaseInsensitive) == 0) {
            path = e.path;
            break;
        }
    }
    if (path.isEmpty()) {
        const QString base = presetsDirectory() + QLatin1Char('/') + baseFileName(stored);
        path = base + QStringLiteral(".json");
        for (int n = 2; QFile::exists(path); ++n)
            path = base + QLatin1Char('-') + QString::number(n) + QStringLiteral(".json");
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QJsonObject object {{QLatin1String("format"), QLatin1String(kFormatName)},
        {QLatin1String("version"), 1}, {QLatin1String("name"), stored},
        {QLatin1String("eq"), serialization::eqToJson(eq)}};
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return file.commit();
}

bool removeUserPreset(const QString& name)
{
    const QString wanted = trimmedName(name);
    for (const Entry& e : entries()) {
        if (QString::compare(e.preset.name, wanted, Qt::CaseInsensitive) == 0)
            return QFile::remove(e.path);
    }
    return false;
}

QString presetsDirectory()
{
    return configRoot() + QStringLiteral("/eq-presets");
}

void setConfigDirectoryForTests(const QString& configDir)
{
    configOverride() = configDir;
}

} // namespace kk::eqpresets
