// SPDX-License-Identifier: GPL-3.0-or-later
// GlApi — which OpenGL flavour the compositor and viewers run on: desktop OpenGL 3.3 core, or
// OpenGL ES 3.0 where the driver offers no 3.3 core context (Raspberry Pi 5: desktop GL 3.1 and
// ES 3.1 only). Shaders are written once in the common subset of GLSL 330 core and GLSL ES 300;
// the version header is chosen when a program is built.
//
// Same pattern as mpv (its GLSL "#version" line and precision statements are generated from
// the context's capabilities) and Qt's own examples (QOpenGLContext::isOpenGLES() picks the
// header): one body, a per-API header, capabilities queried from the live context.
#pragma once

#include <QByteArray>
#include <QString>
#include <QSurfaceFormat>

class QOpenGLContext;

namespace kk::render {

enum class GlApi { Desktop, Gles };

/// Which stage a shader header is for.
enum class ShaderStage { Vertex, Fragment };

/// "OpenGL 3.3 core" / "OpenGL ES 3.0".
[[nodiscard]] QString glApiName(GlApi api);

/// Surface format that asks for @p api: 3.3 core, or ES 3.0 (the driver may return a newer ES).
[[nodiscard]] QSurfaceFormat glFormat(GlApi api);

/// @p base with the version/profile of the API it already names (renderable type): OpenGLES
/// gives ES 3.0, anything else desktop 3.3 core. Every place that creates a context uses it,
/// so they follow the default format that chooseGlApi() set.
[[nodiscard]] QSurfaceFormat contextFormat(const QSurfaceFormat& base);

/// The API of @p format (renderable type).
[[nodiscard]] GlApi apiOf(const QSurfaceFormat& format);

/// KK_GL_API from the environment: "gles" or "desktop"; @p forced says whether it was set.
[[nodiscard]] GlApi requestedGlApi(bool* forced = nullptr);

/// Version line(s) that start every shader of @p stage on @p api: "#version 330 core" for
/// desktop; "#version 300 es" plus default precisions for ES. Ends with a newline.
[[nodiscard]] QByteArray shaderHeader(GlApi api, ShaderStage stage);

/// True if @p header is a valid first part of a shader for @p api (version line first, newline
/// last, precisions only on ES).
[[nodiscard]] bool isValidShaderHeader(GlApi api, const QByteArray& header);

/// API of the context that is current on this thread (Desktop when there is none).
[[nodiscard]] GlApi currentGlApi();

/// Creates a throw-away context with @p api's format and reports whether it works (the real
/// version in @p actual). Needs a QGuiApplication.
[[nodiscard]] bool probeGlApi(GlApi api, QString* actual = nullptr);

/// Picks the API: KK_GL_API if set, else desktop 3.3 core if a context can be made, else
/// ES 3.0. Sets the default surface format to the choice and logs one line.
/// @return false when none works (the default format stays desktop; CPU fallbacks apply).
/// Needs a QGuiApplication; call before any widget or compositor is created.
bool chooseGlApi(GlApi* chosen = nullptr);

/// Sets the default surface format from KK_GL_API (desktop when unset) without probing: for
/// tests that run without chooseGlApi().
void applyGlApiFromEnvironment();

/// True if the context can render to RGBA16F (always on desktop; ES: EXT_color_buffer_float
/// or half_float, or ES 3.2).
[[nodiscard]] bool hasRenderableFloat(QOpenGLContext* ctx);
/// True if 16-bit normalised textures (GL_R16 / GL_RG16) exist (desktop; ES: EXT_texture_norm16).
[[nodiscard]] bool hasNorm16(QOpenGLContext* ctx);
/// True if 32-bit float textures can be filtered linearly (desktop; ES: OES_texture_float_linear).
[[nodiscard]] bool hasFloatLinear(QOpenGLContext* ctx);

} // namespace kk::render
