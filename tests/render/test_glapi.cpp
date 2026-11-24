// SPDX-License-Identifier: GPL-3.0-or-later
// GlApi: the shader headers of both OpenGL flavours, the surface formats they ask for, and the
// KK_GL_API switch. Needs no OpenGL context.
#include "render/GlApi.h"

#include <QSurfaceFormat>
#include <QtTest>

using namespace kk::render;

class TestGlApi : public QObject
{
    Q_OBJECT
private slots:
    void desktopHeaders()
    {
        for (auto stage : {ShaderStage::Vertex, ShaderStage::Fragment}) {
            const QByteArray h = shaderHeader(GlApi::Desktop, stage);
            QVERIFY(isValidShaderHeader(GlApi::Desktop, h));
            QVERIFY(h.startsWith("#version 330 core\n"));
            QVERIFY(!h.contains("precision"));
        }
    }

    void esHeaders()
    {
        for (auto stage : {ShaderStage::Vertex, ShaderStage::Fragment}) {
            const QByteArray h = shaderHeader(GlApi::Gles, stage);
            QVERIFY(isValidShaderHeader(GlApi::Gles, h));
            QVERIFY(h.startsWith("#version 300 es\n"));
            QVERIFY(h.contains("precision highp float;"));
            QVERIFY(h.contains("precision highp int;"));
        }
        // Sampler precisions only exist in the fragment stage.
        QVERIFY(shaderHeader(GlApi::Gles, ShaderStage::Fragment)
                    .contains("precision highp sampler2D;"));
        QVERIFY(!shaderHeader(GlApi::Gles, ShaderStage::Vertex).contains("sampler"));
    }

    void validatorRejectsMixedHeaders()
    {
        QVERIFY(!isValidShaderHeader(
            GlApi::Gles, shaderHeader(GlApi::Desktop, ShaderStage::Fragment)));
        QVERIFY(!isValidShaderHeader(
            GlApi::Desktop, shaderHeader(GlApi::Gles, ShaderStage::Fragment)));
        QVERIFY(!isValidShaderHeader(GlApi::Desktop, "#version 330 core"));
        QVERIFY(!isValidShaderHeader(GlApi::Desktop, "#version 330 core\n#version 330 core\n"));
    }

    void formats()
    {
        const QSurfaceFormat desktop = glFormat(GlApi::Desktop);
        QCOMPARE(desktop.renderableType(), QSurfaceFormat::OpenGL);
        QCOMPARE(desktop.version(), qMakePair(3, 3));
        QCOMPARE(desktop.profile(), QSurfaceFormat::CoreProfile);
        const QSurfaceFormat es = glFormat(GlApi::Gles);
        QCOMPARE(es.renderableType(), QSurfaceFormat::OpenGLES);
        QCOMPARE(es.version(), qMakePair(3, 0));
        QCOMPARE(apiOf(desktop), GlApi::Desktop);
        QCOMPARE(apiOf(es), GlApi::Gles);
        // contextFormat() keeps the API of its base and fixes version and profile.
        QSurfaceFormat base;
        base.setRenderableType(QSurfaceFormat::OpenGLES);
        QCOMPARE(contextFormat(base).version(), qMakePair(3, 0));
        QCOMPARE(contextFormat(QSurfaceFormat()).version(), qMakePair(3, 3));
        QCOMPARE(contextFormat(QSurfaceFormat()).profile(), QSurfaceFormat::CoreProfile);
    }

    void environmentSwitch()
    {
        bool forced = true;
        qunsetenv("KK_GL_API");
        QCOMPARE(requestedGlApi(&forced), GlApi::Desktop);
        QVERIFY(!forced);
        qputenv("KK_GL_API", "gles");
        QCOMPARE(requestedGlApi(&forced), GlApi::Gles);
        QVERIFY(forced);
        qputenv("KK_GL_API", "desktop");
        QCOMPARE(requestedGlApi(&forced), GlApi::Desktop);
        QVERIFY(forced);
        qunsetenv("KK_GL_API");
    }

    void names()
    {
        QVERIFY(glApiName(GlApi::Desktop).contains("3.3"));
        QVERIFY(glApiName(GlApi::Gles).contains("ES"));
    }
};

QTEST_APPLESS_MAIN(TestGlApi)
#include "test_glapi.moc"
