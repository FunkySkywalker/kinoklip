// SPDX-License-Identifier: GPL-3.0-or-later
// PluginPreview — see PluginPreview.h.
#include "render/PluginPreview.h"

#include "plugins/PluginRegistry.h"
#include "render/GlPluginEffect.h"

#include <QPainter>

namespace kk::render {

QImage previewTestChart(QSize size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(32, 36, 44));
    QPainter p(&image);
    const int w = size.width();
    const int h = size.height();
    // Top band: horizontal grey ramp.
    for (int x = 0; x < w; ++x) {
        const int v = x * 255 / std::max(1, w - 1);
        p.fillRect(x, 0, 1, h / 5, QColor(v, v, v));
    }
    // Colour bars.
    const QColor bars[] = {Qt::white, QColor(255, 255, 0), QColor(0, 255, 255), QColor(0, 255, 0),
        QColor(255, 0, 255), QColor(255, 0, 0), QColor(0, 0, 255), Qt::black};
    for (int i = 0; i < 8; ++i)
        p.fillRect(i * w / 8, h / 5, (i + 1) * w / 8 - i * w / 8, h / 5, bars[i]);
    // Skin tones and a saturated ramp.
    const QColor skins[] = {QColor(255, 224, 189), QColor(224, 172, 105), QColor(198, 134, 66),
        QColor(141, 85, 36), QColor(90, 56, 37)};
    for (int i = 0; i < 5; ++i)
        p.fillRect(i * w / 5, 2 * h / 5, (i + 1) * w / 5 - i * w / 5, h / 5, skins[i]);
    // A circle and a grid for geometry (distortions, blurs, stencils).
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(230, 90, 40));
    const int r = h / 6;
    p.drawEllipse(QPoint(w / 4, 4 * h / 5), r, r);
    p.setPen(QPen(QColor(220, 220, 220), std::max(1, h / 180)));
    for (int x = w / 2; x < w; x += std::max(8, w / 16))
        p.drawLine(x, 3 * h / 5, x, h);
    for (int y = 3 * h / 5; y < h; y += std::max(8, h / 10))
        p.drawLine(w / 2, y, w, y);
    p.end();
    return image;
}

bool PluginPreviewRig::initialize(QString* why)
{
    m_surface.create();
    m_context = std::make_unique<QOpenGLContext>();
    if (!m_context->create() || !m_surface.isValid() || !m_context->makeCurrent(&m_surface)) {
        if (why)
            *why = QStringLiteral("no OpenGL context (a headless session needs xvfb-run)");
        m_context.reset();
        return false;
    }
    if (!m_context->isOpenGLES() && m_context->format().version() < qMakePair(3, 3)) {
        if (why)
            *why = QStringLiteral("OpenGL 3.3 core is not available");
        m_context.reset();
        return false;
    }
    m_gl = m_context->extraFunctions();
    if (!m_gl) {
        if (why)
            *why = QStringLiteral("OpenGL functions are not available");
        m_context.reset();
        return false;
    }
    m_kit = std::make_unique<GlKit>(m_gl);
    QString kitWhy;
    if (!m_kit->initialize(&kitWhy)) {
        if (why)
            *why = kitWhy;
        m_kit.reset();
        m_context.reset();
        return false;
    }
    return true;
}

bool PluginPreviewRig::isEs() const
{
    return m_context && m_context->isOpenGLES();
}

void PluginPreviewRig::destroy()
{
    if (!m_context)
        return;
    if (m_context->makeCurrent(&m_surface)) {
        for (auto& [id, effect] : m_effects)
            effect->destroy(*m_kit);
        for (auto& [id, transition] : m_transitions)
            transition->destroy(*m_kit);
        m_effects.clear();
        m_transitions.clear();
        m_kit->destroy();
    }
    m_kit.reset();
    m_context.reset();
}

GLuint PluginPreviewRig::upload(const QImage& image)
{
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    GLuint texture = 0;
    m_gl->glGenTextures(1, &texture);
    m_gl->glBindTexture(GL_TEXTURE_2D, texture);
    m_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rgba.width(), rgba.height(), 0, GL_RGBA,
        GL_UNSIGNED_BYTE, rgba.constBits());
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

QImage PluginPreviewRig::readBack(const RenderTarget& target)
{
    // Through an 8-bit target: the RGBA16F layer cannot be read as bytes on every API.
    const QSize size = target.size;
    PooledTarget bytes = m_kit->acquire(size, GL_RGBA8);
    m_kit->bindTarget(*bytes);
    m_kit->clearTransparent();
    m_kit->setBlendOver(false);
    m_kit->blit(target.texture, GlKit::quadToNdc(QRectF(QPointF(0, 0), size), size));
    QImage out(size, QImage::Format_RGBA8888_Premultiplied);
    m_gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, bytes->fbo);
    m_gl->glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, out.bits());
    m_gl->glUseProgram(0);
    return out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

GlEffect* PluginPreviewRig::effectFor(const QString& id, QString* why)
{
    // A plugin reload (new shader text) builds the plugin effects again.
    const quint64 generation = plugins::PluginRegistry::instance().generation();
    if (generation != m_generation) {
        m_generation = generation;
        for (auto it = m_effects.begin(); it != m_effects.end();) {
            if (isPluginId(it->first)) {
                it->second->destroy(*m_kit);
                it = m_effects.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = m_transitions.begin(); it != m_transitions.end();) {
            if (isPluginId(it->first)) {
                it->second->destroy(*m_kit);
                it = m_transitions.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (const auto it = m_effects.find(id); it != m_effects.end())
        return it->second.get();
    std::unique_ptr<GlEffect> effect;
    if (const GlEffectFactory* factory = findGlEffectFactory(id))
        effect = factory->create();
    else if (isPluginId(id))
        effect = makePluginEffect(id);
    if (!effect) {
        if (why)
            *why = QStringLiteral("no GPU effect \"%1\" (plugin missing or disabled?)").arg(id);
        return nullptr;
    }
    QString log;
    if (!effect->initialize(*m_kit, &log)) {
        if (why)
            *why = log;
        return nullptr;
    }
    return m_effects.emplace(id, std::move(effect)).first->second.get();
}

GlTransition* PluginPreviewRig::transitionFor(const QString& id, QString* why)
{
    effectFor(QString(), nullptr); // refreshes the generation
    if (const auto it = m_transitions.find(id); it != m_transitions.end())
        return it->second.get();
    std::unique_ptr<GlTransition> transition;
    if (const GlTransitionFactory* factory = findGlTransitionFactory(id))
        transition = factory->create();
    else if (isPluginId(id))
        transition = makePluginTransition(id);
    if (!transition) {
        if (why)
            *why = QStringLiteral("no GPU transition \"%1\" (plugin missing or disabled?)").arg(id);
        return nullptr;
    }
    QString log;
    if (!transition->initialize(*m_kit, &log)) {
        if (why)
            *why = log;
        return nullptr;
    }
    return m_transitions.emplace(id, std::move(transition)).first->second.get();
}

QImage PluginPreviewRig::renderEffect(const QString& id, const ParamSet& params,
    const QImage& source, int64_t localFrame, double fps, QString* why)
{
    if (!m_context || !m_context->makeCurrent(&m_surface)) {
        if (why)
            *why = QStringLiteral("no OpenGL context");
        return {};
    }
    GlEffect* effect = effectFor(id, why);
    if (!effect)
        return {};
    const QSize size = source.size();
    const GLuint texture = upload(source);
    PooledTarget layer = m_kit->acquire(size, GL_RGBA16F);
    m_kit->bindTarget(*layer);
    m_kit->clearTransparent();
    m_kit->setBlendOver(false);
    m_kit->blit(texture, GlKit::quadToNdc(QRectF(QPointF(0, 0), size), size));
    EffectInput in;
    in.layer = layer.get();
    in.scale = 1.0;
    in.localFrame = localFrame;
    in.params = &params;
    in.fps = fps;
    PooledTarget out = effect->apply(*m_kit, in);
    const QImage image = readBack(out ? *out : *layer);
    m_gl->glDeleteTextures(1, &texture);
    return image;
}

QImage PluginPreviewRig::renderTransition(const QString& id, const ParamSet& params,
    double progress, const QImage& a, const QImage& b, QString* why)
{
    if (!m_context || !m_context->makeCurrent(&m_surface)) {
        if (why)
            *why = QStringLiteral("no OpenGL context");
        return {};
    }
    GlTransition* transition = transitionFor(id, why);
    if (!transition)
        return {};
    const QSize size = a.size();
    const GLuint ta = upload(a);
    const GLuint tb = upload(b);
    PooledTarget target = m_kit->acquire(size, GL_RGBA8);
    m_kit->bindTarget(*target);
    m_kit->clearTransparent();
    m_kit->setBlendOver(true);
    TransitionInput in;
    in.a = ta;
    in.b = tb;
    in.progress = progress;
    in.params = &params;
    in.localFrame = 0;
    in.size = size;
    in.target = target.get();
    transition->draw(*m_kit, in);
    m_gl->glUseProgram(0);
    QImage out(size, QImage::Format_RGBA8888_Premultiplied);
    m_gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, target->fbo);
    m_gl->glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, out.bits());
    m_gl->glDeleteTextures(1, &ta);
    m_gl->glDeleteTextures(1, &tb);
    return out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

} // namespace kk::render
