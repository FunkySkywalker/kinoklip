// SPDX-License-Identifier: GPL-3.0-or-later
// EqPresets — equalizer presets of the strip EQ window (spec 137): the factory presets (Flat,
// Voice, Music, Telephone, Rumble Cut, De-Mud, Podcast Voice) and the user's own, kept as small
// JSON files in the config directory next to the project presets (core/ProjectPresets.h).
#pragma once

#include "core/MixerData.h"

#include <QList>
#include <QString>

namespace kk::eqpresets {

/// One equalizer preset.
struct Preset
{
    QString name; ///< the stable English name for factory presets, the user's text otherwise
    EqSettings eq;
    bool factory = false;
};

/// The factory presets in menu order. Choosing one switches the EQ on (Flat switches it off).
[[nodiscard]] QList<Preset> factoryPresets();
/// The user's presets, sorted by name (case-insensitive).
[[nodiscard]] QList<Preset> userPresets();
/// A factory or user preset by name (case-insensitive), or an empty name when unknown.
[[nodiscard]] Preset find(const QString& name);
/// True if @p name is a factory preset name (those cannot be overwritten or deleted).
[[nodiscard]] bool isFactoryName(const QString& name);
/// Saves @p eq as the user preset @p name (overwrites a user preset of that name). Refused
/// (false): an empty name or a factory preset's name.
bool saveUserPreset(const QString& name, const EqSettings& eq);
/// Deletes the user preset @p name. Returns whether a preset was deleted.
bool removeUserPreset(const QString& name);

/// The directory holding the user presets.
[[nodiscard]] QString presetsDirectory();
/// Redirects the config directory (tests); an empty string restores the default.
void setConfigDirectoryForTests(const QString& configDir);

} // namespace kk::eqpresets
