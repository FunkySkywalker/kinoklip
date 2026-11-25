// SPDX-License-Identifier: GPL-3.0-or-later
// PluginPreview — draws one effect or transition of the GPU catalogue (built-in or plugin) onto
// a picture, without a timeline or a compositor: what plugin.render_preview shows an AI (or a
// developer) after every change of a shader, and what the tests use to compare a plugin with the
// built-in it ports. Needs a QGuiApplication on a platform with OpenGL (like the GL tests).
#pragma once

#include "core/Animation.h"
#include "render/GlEffects.h"
#include "render/GlTransitions.h"

#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QString>

#include <map>
#include <memory>

namespace kk::render {

/// A deterministic test picture: grey ramp, colour bars, skin tones, a circle and a grid, so
/// most effects show something. @p size must be at least 64 x 36.
[[nodiscard]] QImage previewTestChart(QSize size);

class PluginPreviewRig
{
public:
    PluginPreviewRig() = default;
    PluginPreviewRig(const PluginPreviewRig&) = delete;
    PluginPreviewRig& operator=(const PluginPreviewRig&) = delete;
    ~PluginPreviewRig() { destroy(); }

    /// Creates the context and the toolkit. False with @p why without OpenGL.
    bool initialize(QString* why);
    /// True if the context is OpenGL ES.
    [[nodiscard]] bool isEs() const;

    /// Applies effect @p id (a built-in id or "plugin:...") with @p params to @p source at
    /// clip-local frame @p localFrame (timeline rate @p fps). A null image with @p why when the
    /// effect is unknown or its shader does not compile; an effect that leaves the picture
    /// unchanged returns the source.
    QImage renderEffect(const QString& id, const ParamSet& params, const QImage& source,
        int64_t localFrame, double fps, QString* why);
    /// Draws transition @p id at @p progress (0 = all @p a, 1 = all @p b).
    QImage renderTransition(const QString& id, const ParamSet& params, double progress,
        const QImage& a, const QImage& b, QString* why);

    void destroy();

private:
    GLuint upload(const QImage& image);
    QImage readBack(const RenderTarget& target);
    GlEffect* effectFor(const QString& id, QString* why);
    GlTransition* transitionFor(const QString& id, QString* why);

    QOffscreenSurface m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    GlFunctions* m_gl = nullptr;
    std::unique_ptr<GlKit> m_kit;
    std::map<QString, std::unique_ptr<GlEffect>> m_effects;
    std::map<QString, std::unique_ptr<GlTransition>> m_transitions;
    quint64 m_generation = 0;
};

} // namespace kk::render
