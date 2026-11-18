// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectPresets — Project Settings presets and the user default config (Resolve's "Presets"
// page, "Save As User Default Config", "Reset to factory default"). Presets are small JSON
// files in the app's config directory; the user default config is what new projects start from.
#pragma once

#include "core/MediaLinks.h"
#include "core/Project.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace kk::projectpresets {

/// Everything a preset stores: the project settings (without the project name) and the
/// optimized media / proxy / render cache settings.
struct Preset
{
    QString name;
    ProjectSettings settings;
    MediaCacheSettings cache;
};

/// One line of the Presets list.
struct PresetInfo
{
    QString name;
    int width = 0;
    int height = 0;
    Rational fps {24, 1};
};

/// Version of the file format written by toJson(). Readers ignore unknown keys and keep the
/// factory value for a missing one; a file of a newer version is read the same way.
constexpr int kFormatVersion = 1;

/// The preset as the JSON object stored in a file.
[[nodiscard]] QJsonObject toJson(const Preset& preset);
/// Reads toJson() output. Missing keys keep the factory values, unknown keys are ignored,
/// out-of-range values fall back to the factory value. Returns nothing (and fills @p error)
/// only when the object is not a preset at all.
[[nodiscard]] std::optional<Preset> fromJson(const QJsonObject& json, QString* error = nullptr);

/// Directory that holds the presets, and the user default config file. Both live in the
/// app config directory (QStandardPaths::AppConfigLocation) unless a test redirected it.
[[nodiscard]] QString presetsDirectory();
[[nodiscard]] QString userDefaultPath();
/// Tests: put everything below @p configDir instead of the real config directory. An empty
/// string restores the real one.
void setConfigDirectoryForTests(const QString& configDir);

/// The saved presets, sorted by name. Files that cannot be read are skipped.
[[nodiscard]] QList<PresetInfo> list();
/// The saved preset called @p name (case-insensitive), if any.
[[nodiscard]] std::optional<Preset> load(const QString& name);
/// Saves @p preset under its name, replacing a preset of the same name. False if the name is
/// empty or the file cannot be written.
bool save(const Preset& preset);
/// Removes the preset called @p name. False if there is none.
bool remove(const QString& name);

/// Writes @p preset to @p path / reads a preset file. readFile() fills @p error (a sentence for
/// the user) when the file is unreadable or not a preset.
bool writeFile(const QString& path, const Preset& preset);
[[nodiscard]] std::optional<Preset> readFile(const QString& path, QString* error = nullptr);

/// The user default config: what new projects start from. Nothing when none was saved (or the
/// file is broken), then the factory defaults apply.
[[nodiscard]] std::optional<Preset> userDefault();
bool setUserDefault(const Preset& preset);
/// Reset to factory default: removes the user default config.
bool resetUserDefault();

/// The built-in defaults of a new project (1920 x 1080, 24 fps, ...), as a preset.
[[nodiscard]] Preset factoryDefault();
/// @p project's current settings and cache settings as a preset called @p name.
[[nodiscard]] Preset fromProject(const Project& project, const QString& name = {});

} // namespace kk::projectpresets
