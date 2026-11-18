// SPDX-License-Identifier: GPL-3.0-or-later
// MacroFiles — macros saved as files (spec 120): a Group or Macro of a composition written to
// <AppData>/macros/<name>.kkmacro, inserted into other compositions, and offered as a title in
// the Effects Library (a title clip whose composition is the macro).
#pragma once

#include "core/Composition.h"

#include <QString>
#include <QVector>

#include <optional>

namespace kk::macrofiles {

/// Prefix of the Effects Library id of a saved macro title ("macro:" + file base name).
inline const QString kIdPrefix = QStringLiteral("macro:");

/// A saved macro: the group node, its members (with the links between them) and the name of the
/// node that is its output.
struct MacroFile
{
    QString name;
    quint64 outputId = 0; ///< id (within nodes) of the member that is the macro's output; 0 = none
    QVector<CompNode> nodes; ///< the group node first, then its members
};

/// The folder macros are saved in (created when missing).
[[nodiscard]] QString directory();
/// File base name for @p name: letters, digits and "-" (others become "-"), never empty.
[[nodiscard]] QString baseName(const QString& name);
/// Path of the file for @p name.
[[nodiscard]] QString pathFor(const QString& name);

/// Builds the file content of group (or macro) @p group of @p comp; nullopt for another node.
[[nodiscard]] std::optional<MacroFile> fromGroup(const Composition& comp, quint64 group);
/// Writes @p macro (replacing a file of the same name). Returns the path, or an empty string
/// with @p error set.
QString save(const MacroFile& macro, QString* error = nullptr);
/// Reads a macro file; nullopt when it is not one.
[[nodiscard]] std::optional<MacroFile> load(const QString& path);
/// Saved macros: name and path, sorted by name.
struct Entry
{
    QString name;
    QString path;
    QString id() const;
};
[[nodiscard]] QVector<Entry> list();
/// Deletes the file of macro @p name. False if there was none.
bool remove(const QString& name);
/// The path of the macro with Effects Library id @p id ("macro:<base>"), or empty.
[[nodiscard]] QString pathOfId(const QString& id);

/// Puts @p macro into @p comp as a new group at @p position (new ids and names, controls
/// connected as saved). Returns the group's id, 0 for an empty macro. @p newOutput gets the id
/// the output node has now (0 if none).
quint64 insert(Composition& comp, const MacroFile& macro, QPointF position, quint64* newOutput = nullptr);

/// The composition of a title clip made from @p macro: the macro inserted and its output node
/// connected to MediaOut (MediaIn is left unconnected, so the title is the macro's picture).
[[nodiscard]] Composition titleComposition(const MacroFile& macro);

} // namespace kk::macrofiles
