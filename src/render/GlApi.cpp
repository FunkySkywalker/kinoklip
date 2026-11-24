// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/GlApi.h"

#include "render/RenderLog.h"

#include <QOpenGLContext>

namespace kk::render {

QString glApiName(GlApi api)
{
    return api == GlApi::Gles ? QStringLiteral("OpenGL ES 3.0") : QStringLiteral("OpenGL 3.3 core");
}

QSurfaceFormat glFormat(GlApi api)
{
    QSurfaceFormat format;
    if (api == GlApi::Gles) {
        format.setRenderableType(QSurfaceFormat::OpenGLES);
        format.setVersion(3, 0);
    } else {
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
    }
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    return format;
}

GlApi apiOf(const QSurfaceFormat& format)
{
    return format.renderableType() == QSurfaceFormat::OpenGLES ? GlApi::Gles : GlApi::Desktop;
}

QSurfaceFormat contextFormat(const QSurfaceFormat& base)
{
    QSurfaceFormat format = base;
    if (apiOf(base) == GlApi::Gles) {
        format.setVersion(3, 0);
        format.setProfile(QSurfaceFormat::NoProfile);
    } else {
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
    }
    return format;
}

GlApi requestedGlApi(bool* forced)
{
    const QByteArray value = qgetenv("KK_GL_API").trimmed().toLower();
    if (forced)
        *forced = value == "gles" || value == "es" || value == "desktop" || value == "gl";
    return value == "gles" || value == "es" ? GlApi::Gles : GlApi::Desktop;
}

QByteArray shaderHeader(GlApi api, ShaderStage stage)
{
    if (api == GlApi::Desktop)
        return QByteArrayLiteral("#version 330 core\n");
    // ES has no default float precision in fragment shaders and defaults sampler precision to
    // lowp, which would be wrong for the float tables: everything is highp, as on desktop.
    QByteArray header
        = QByteArrayLiteral("#version 300 es\nprecision highp float;\nprecision highp int;\n");
    if (stage == ShaderStage::Fragment) {
        header += QByteArrayLiteral("precision highp sampler2D;\nprecision highp sampler3D;\n");
    }
    return header;
}

bool isValidShaderHeader(GlApi api, const QByteArray& header)
{
    if (!header.endsWith('\n'))
        return false;
    const QByteArray expected = api == GlApi::Gles ? QByteArrayLiteral("#version 300 es\n")
                                                   : QByteArrayLiteral("#version 330 core\n");
    if (!header.startsWith(expected))
        return false;
    if (header.indexOf("#version", expected.size()) >= 0)
        return false;
    if (api == GlApi::Gles && !header.contains("precision highp float;"))
        return false;
    if (api == GlApi::Desktop && header.contains("precision"))
        return false;
    return true;
}

GlApi currentGlApi()
{
    const QOpenGLContext* ctx = QOpenGLContext::currentContext();
    return ctx && ctx->isOpenGLES() ? GlApi::Gles : GlApi::Desktop;
}

bool probeGlApi(GlApi api, QString* actual)
{
    QOpenGLContext context;
    context.setFormat(glFormat(api));
    if (!context.create())
        return false;
    const QSurfaceFormat got = context.format();
    if (actual)
        *actual = QStringLiteral("%1.%2").arg(got.majorVersion()).arg(got.minorVersion());
    if (api == GlApi::Gles)
        return context.isOpenGLES() && got.majorVersion() >= 3;
    return !context.isOpenGLES() && got.version() >= qMakePair(3, 3);
}

namespace {

void setDefaultApi(GlApi api)
{
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    const QSurfaceFormat wanted = glFormat(api);
    format.setRenderableType(wanted.renderableType());
    format.setVersion(wanted.majorVersion(), wanted.minorVersion());
    format.setProfile(wanted.profile());
    QSurfaceFormat::setDefaultFormat(format);
}

} // namespace

bool chooseGlApi(GlApi* chosen)
{
    bool forced = false;
    const GlApi requested = requestedGlApi(&forced);
    const GlApi order[2] = {requested, requested == GlApi::Desktop ? GlApi::Gles : GlApi::Desktop};
    const int tries = forced ? 1 : 2;
    for (int i = 0; i < tries; ++i) {
        QString version;
        if (!probeGlApi(order[i], &version)) {
            qCInfo(lcRender).noquote() << glApiName(order[i]) << "context not available";
            continue;
        }
        setDefaultApi(order[i]);
        qCInfo(lcRender).noquote() << "OpenGL API:" << glApiName(order[i]) << "- the driver gives"
                                   << version << (forced ? "(set by KK_GL_API)" : "");
        if (chosen)
            *chosen = order[i];
        return true;
    }
    setDefaultApi(GlApi::Desktop);
    qCWarning(lcRender) << "neither OpenGL 3.3 core nor OpenGL ES 3.0 is available: CPU fallbacks";
    return false;
}

void applyGlApiFromEnvironment()
{
    setDefaultApi(requestedGlApi());
}

namespace {

bool isEs(QOpenGLContext* ctx)
{
    return ctx && ctx->isOpenGLES();
}

bool hasExt(QOpenGLContext* ctx, const char* name)
{
    return ctx && ctx->hasExtension(QByteArray(name));
}

} // namespace

bool hasRenderableFloat(QOpenGLContext* ctx)
{
    if (!isEs(ctx))
        return true;
    return ctx->format().version() >= qMakePair(3, 2) || hasExt(ctx, "GL_EXT_color_buffer_float")
        || hasExt(ctx, "GL_EXT_color_buffer_half_float");
}

bool hasNorm16(QOpenGLContext* ctx)
{
    return !isEs(ctx) || hasExt(ctx, "GL_EXT_texture_norm16");
}

bool hasFloatLinear(QOpenGLContext* ctx)
{
    return !isEs(ctx) || hasExt(ctx, "GL_OES_texture_float_linear");
}

} // namespace kk::render
