// SPDX-License-Identifier: GPL-3.0-or-later
// GlPluginEffect — the GPU side of plugin effects and transitions (internal to src/render): a
// GlEffect / GlTransition made from the GLSL file of a plugin (plugins/PluginRegistry.h), with
// the parameters of its manifest as uniforms. The renderer asks for them by descriptor id
// ("plugin:<plugin id>/<item id>") where it finds no built-in.
#pragma once

#include "render/GlEffects.h"
#include "render/GlTransitions.h"

#include <QString>

#include <memory>

namespace kk::render {

/// The effect for plugin effect @p fullId (an "ofx:" id gives the OpenFX effect), or null when no enabled plugin provides it.
[[nodiscard]] std::unique_ptr<GlEffect> makePluginEffect(const QString& fullId);
/// The transition for plugin transition @p fullId, or null.
[[nodiscard]] std::unique_ptr<GlTransition> makePluginTransition(const QString& fullId);
/// True for ids of plugin items ("plugin:...", and "ofx:..." of OpenFX effects).
[[nodiscard]] bool isPluginId(const QString& id);

} // namespace kk::render
