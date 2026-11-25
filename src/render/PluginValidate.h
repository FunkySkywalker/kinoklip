// SPDX-License-Identifier: GPL-3.0-or-later
// PluginValidate — checks a plugin folder completely: the manifest against its rules, the files it
// names, the shader lint, and a compile of every shader on a GL context for desktop OpenGL 3.3
// and, where the machine can make one, OpenGL ES 3.0. plugin.validate and the Plugins dialog use
// it; the answer says where every problem is (file and line) so an editor or an AI can fix it.
#pragma once

#include "plugins/PluginManifest.h"

#include <QString>
#include <QVector>

namespace kk::render {

struct ValidatedItem
{
    QString fullId;
    QString kind; ///< "effect" or "transition"
    QString file;
    /// "ok", "error" or "skipped" (no context of that API on this machine).
    QString desktop;
    QString gles;
};

struct PluginValidation
{
    bool valid = false; ///< no error (warnings are fine); every shader compiled where it could
    QString pluginId;
    QString pluginName;
    QVector<plugins::Problem> problems; ///< errors and warnings
    QVector<ValidatedItem> items;
    QString note; ///< why compilation was skipped, if it was
};

/// Validates the plugin in folder @p dir. With @p compile false only the static checks run
/// (no GL context needed).
[[nodiscard]] PluginValidation validatePluginFolder(const QString& dir, bool compile);

} // namespace kk::render
