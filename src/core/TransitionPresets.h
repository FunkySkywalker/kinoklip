// SPDX-License-Identifier: GPL-3.0-or-later
// TransitionPresets — transitions built on the Compositing page, saved as files (spec 128.10):
// the node graph of a Fusion Composition transition written to
// <AppData>/transition-presets/<name>.kktransition, listed under Video Transitions > User
// Presets in the Effects Library (id "tpreset:<base>") and applied like any transition (drag onto
// a cut, double-click, Ctrl+T as standard transition). Applying copies the graph into the
// transition, so a preset that is renamed or deleted later does not change transitions already
// on the timeline.
#pragma once

#include "core/Composition.h"
#include "core/TimelineData.h"

#include <QString>
#include <QVector>

#include <optional>

namespace kk::transitionpresets {

/// Prefix of the Effects Library id of a preset ("tpreset:" + file base name).
inline const QString kIdPrefix = QStringLiteral("tpreset:");
/// The Effects Library folder of the presets (effectgroups::UserTransitions).
inline constexpr char kGroup[] = "user_transitions";

struct Preset
{
    QString name;
    int64_t duration = 25; ///< length the graph was built for (frames)
    Composition composition;
};

/// The folder presets are saved in (created when missing); tests may point it elsewhere with
/// setDirectory().
[[nodiscard]] QString directory();
/// Replaces the folder (tests); an empty path restores the default.
void setDirectory(const QString& path);
/// File base name for @p name: letters, digits and "-" (others become "-"), never empty.
[[nodiscard]] QString baseName(const QString& name);
/// The Effects Library id of preset @p name.
[[nodiscard]] QString idOf(const QString& name);
/// Path of the file for @p name.
[[nodiscard]] QString pathFor(const QString& name);

/// Writes @p preset (replacing a file of the same name). Returns the path, or an empty string
/// with @p error set. The registry knows the new entry afterwards.
QString save(const Preset& preset, QString* error = nullptr);
/// Reads a preset file; nullopt when it is not one.
[[nodiscard]] std::optional<Preset> load(const QString& path);
/// The preset with Effects Library id @p id ("tpreset:<base>"); nullopt when there is none.
[[nodiscard]] std::optional<Preset> loadById(const QString& id);

struct Entry
{
    QString name;
    QString path;
    [[nodiscard]] QString id() const { return idOf(name); }
};
/// Saved presets sorted by name.
[[nodiscard]] QVector<Entry> list();
/// Deletes preset @p name. False if there was none.
bool remove(const QString& name);
/// Renames preset @p name to @p newName (the file is rewritten under the new name). False when
/// there is no such preset, @p newName is empty or another preset has that file name.
bool rename(const QString& name, const QString& newName);

/// The transition a preset makes: type Fusion Composition, the saved graph with its time keys
/// stretched from the preset's length to @p durationFrames.
[[nodiscard]] TransitionData makeTransition(const Preset& preset, int64_t durationFrames);
/// @p comp built for @p fromFrames long, with every key moved so it fits @p toFrames: frame f
/// maps to round((f + 1) * (to + 1) / (from + 1)) - 1, so the "-1" start and the "duration" end
/// of the default blend stay where they are.
[[nodiscard]] Composition stretched(const Composition& comp, int64_t fromFrames, int64_t toFrames);

} // namespace kk::transitionpresets
