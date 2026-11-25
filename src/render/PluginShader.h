// SPDX-License-Identifier: GPL-3.0-or-later
// PluginShader — turns a plugin's GLSL file into the fragment shader the renderer compiles
// (the prelude with the standard and parameter uniforms, the author's code, the entry point),
// checks it without a GL context (the lint of the rules that differ between desktop GL 3.3 and
// OpenGL ES 3.0) and compiles it on an offscreen context. Used by the plugin effects and
// transitions (GlPluginEffect.cpp) and by plugin.validate.
#pragma once

#include "plugins/PluginManifest.h"
#include "plugins/PluginRegistry.h"
#include "render/GlApi.h"

#include <QString>
#include <QVector>

#include <string>

namespace kk::render::pluginshader {

/// The fragment shader of effect @p spec (the part after the version header and the common
/// fragment header that GlProgram::build adds). Line 1 of the author's code is line 1 of the
/// compiler's messages.
[[nodiscard]] std::string composeEffect(const plugins::PluginShaderSpec& spec);
/// Same for a transition.
[[nodiscard]] std::string composeTransition(const plugins::PluginShaderSpec& spec);
/// composeEffect() or composeTransition() by the kind of @p spec.
[[nodiscard]] std::string compose(const plugins::PluginShaderSpec& spec);

/// The uniform name of parameter @p key: "u_<key>".
[[nodiscard]] QString uniformName(const QString& key);

/// Checks the author's code without a GL context: forbidden or legacy constructs, a missing
/// entry function, ES-reserved words, int-to-float conversions that ES refuses. Each problem
/// has the line of the construct.
[[nodiscard]] QVector<plugins::Problem> lint(const plugins::PluginShaderSpec& spec);

/// Turns a driver compile log (Mesa, NVIDIA, ANGLE and glslang styles) into problems with the
/// line in the author's file; lines outside it (the entry point we add) get line 0.
[[nodiscard]] QVector<plugins::Problem> parseCompileLog(
    const QString& log, const QString& file, int userLineCount);

/// Result of compiling a shader on a throw-away offscreen context.
struct CompileCheck
{
    bool attempted = false; ///< false: no context of that API could be made (see `note`)
    bool compiled = false;
    GlApi api = GlApi::Desktop;
    QString note; ///< why it was not attempted, or the raw log
    QVector<plugins::Problem> problems;
};
/// Compiles @p spec for @p api. Needs a QGuiApplication on a platform with OpenGL (an xcb or
/// wayland session, or xvfb); otherwise `attempted` is false. GUI thread only.
[[nodiscard]] CompileCheck compileOnContext(const plugins::PluginShaderSpec& spec, GlApi api);

} // namespace kk::render::pluginshader
