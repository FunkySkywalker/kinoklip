// SPDX-License-Identifier: GPL-3.0-or-later
// LoudnessStandards implementation.
#include "render/LoudnessStandards.h"

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace kk::loudness {

namespace {
struct Preset
{
    Standard standard;
    const char* key;
    double lufs;
    double truePeakDb;
};
constexpr std::array<Preset, kStandardCount> kPresets {{
    {Standard::EbuR128, "ebu_r128", -23.0, -1.0},
    {Standard::AtscA85, "atsc_a85", -24.0, -2.0},
    {Standard::Streaming, "streaming", -14.0, -1.0},
    {Standard::Podcast, "podcast", -16.0, -1.0},
    {Standard::Custom, "custom", -23.0, -1.0},
}};
const Preset& preset(Standard s)
{
    return kPresets[size_t(std::clamp(int(s), 0, kStandardCount - 1))];
}
} // namespace

double Target::lufs() const
{
    return standard == Standard::Custom ? customLufs : presetLufs(standard);
}

double Target::truePeakDb() const
{
    return standard == Standard::Custom ? customTruePeakDb : presetTruePeakDb(standard);
}

const std::array<Standard, kStandardCount>& standards()
{
    static const std::array<Standard, kStandardCount> all {Standard::EbuR128, Standard::AtscA85,
        Standard::Streaming, Standard::Podcast, Standard::Custom};
    return all;
}

double presetLufs(Standard standard)
{
    return preset(standard).lufs;
}

double presetTruePeakDb(Standard standard)
{
    return preset(standard).truePeakDb;
}

QString standardKey(Standard standard)
{
    return QLatin1String(preset(standard).key);
}

Standard standardFromKey(const QString& key, Standard fallback)
{
    for (const Preset& p : kPresets) {
        if (key == QLatin1String(p.key))
            return p.standard;
    }
    return fallback;
}

QString standardName(Standard standard)
{
    switch (standard) {
    case Standard::EbuR128:
        return QCoreApplication::translate("kk::loudness", "EBU R128 (-23 LUFS)");
    case Standard::AtscA85:
        return QCoreApplication::translate("kk::loudness", "ATSC A/85 (-24 LKFS)");
    case Standard::Streaming:
        return QCoreApplication::translate("kk::loudness", "YouTube / streaming (-14 LUFS)");
    case Standard::Podcast:
        return QCoreApplication::translate("kk::loudness", "Podcast (-16 LUFS)");
    case Standard::Custom:
        break;
    }
    return QCoreApplication::translate("kk::loudness", "Custom");
}

double deviationLu(double loudnessLufs, double targetLufs)
{
    return loudnessLufs <= -199.0 ? 0.0 : loudnessLufs - targetLufs;
}

QString deviationText(double loudnessLufs, double targetLufs)
{
    if (loudnessLufs <= -199.0)
        return {};
    const double d = deviationLu(loudnessLufs, targetLufs);
    if (std::abs(d) <= kOnTargetLu)
        return QCoreApplication::translate("kk::loudness", "On target");
    return d > 0 ? QCoreApplication::translate("kk::loudness", "Over by %1 LU").arg(d, 0, 'f', 1)
                 : QCoreApplication::translate("kk::loudness", "Under by %1 LU").arg(-d, 0, 'f', 1);
}

Target clamped(Target t)
{
    t.customLufs = std::clamp(t.customLufs, kMinTargetLufs, kMaxTargetLufs);
    t.customTruePeakDb = std::clamp(t.customTruePeakDb, kMinCeilingDb, kMaxCeilingDb);
    return t;
}

QString targetToString(const Target& t)
{
    return QStringLiteral("%1|%2|%3")
        .arg(standardKey(t.standard))
        .arg(t.customLufs, 0, 'f', 1)
        .arg(t.customTruePeakDb, 0, 'f', 1);
}

Target targetFromString(const QString& text, const Target& fallback)
{
    const QStringList parts = text.split(QLatin1Char('|'));
    if (parts.size() != 3)
        return fallback;
    Target t = fallback;
    t.standard = standardFromKey(parts[0], fallback.standard);
    bool okA = false, okB = false;
    const double a = parts[1].toDouble(&okA);
    const double b = parts[2].toDouble(&okB);
    if (okA)
        t.customLufs = a;
    if (okB)
        t.customTruePeakDb = b;
    return clamped(t);
}

} // namespace kk::loudness
