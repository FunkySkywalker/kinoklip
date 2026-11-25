// SPDX-License-Identifier: GPL-3.0-or-later
// OpenFX effects on the GPU pipeline (docs/behavior-spec.md section 158): the layer goes to the
// plugin as premultiplied float RGBA, comes back into the compositor, keyframes move the result
// frame by frame, a missing plugin renders as bypass, and the preview rig draws the same as the
// real compositor. Skips without OpenGL.
#include "RenderTestUtil.h"
#include "core/EffectRegistry.h"
#include "plugins/OfxRegistry.h"
#include "render/Compositor.h"
#include "render/PluginPreview.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace kk;
using namespace kk::test;
using namespace kk::render;
using kk::plugins::OfxRegistry;

namespace {

const QString kGain = QStringLiteral("ofx:org.kinoklip.test.Gain");

int channelDiff(const QColor& a, const QColor& b)
{
    return std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()),
        std::abs(a.blue() - b.blue())});
}

} // namespace

class TestOfxRender : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    PluginPreviewRig m_rig;
    bool m_gl = false;
    std::unique_ptr<Compositor> m_gpu;

    bool gpuReady() const { return m_gl && m_gpu && m_gpu->isGpu(); }

    TimelineData build(const QString& type, double gain, bool keyframed = false)
    {
        TimelineData tl = emptyTimeline(160, 90);
        ClipData clip = solidClip(tl, QColor(200, 100, 50), 0, 20);
        if (!type.isEmpty()) {
            EffectInstance fx = EffectRegistry::instance().createEffect(type, tl.allocateId());
            fx.params.setBase(QStringLiteral("gain"), gain);
            if (keyframed) {
                AnimatedValue& g = fx.params.animated(QStringLiteral("gain"), gain);
                g.setKey(0, 0.2);
                g.setKey(19, 1.0);
            }
            clip.effects.append(fx);
        }
        addClip(tl, TrackType::Video, 0, std::move(clip));
        return tl;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_ofxrender"));
        QSettings().clear();
        QVERIFY(m_dir.isValid());
        const QString dir = m_dir.filePath(QStringLiteral("ofx"));
        QDir().mkpath(dir);
        QVERIFY(QFile::link(QStringLiteral(KK_OFX_TEST_DIR "/KkTest.ofx.bundle"),
            dir + QStringLiteral("/KkTest.ofx.bundle")));
        OfxRegistry& reg = OfxRegistry::instance();
        reg.setProbeProgram(QStringLiteral(KK_OFX_PROBE_PATH));
        reg.setCacheFile(m_dir.filePath(QStringLiteral("cache.json")));
        reg.setSearchDirs({dir});
        reg.reload();
        QVERIFY(reg.plugin(kGain));
        QString why;
        m_gl = m_rig.initialize(&why);
        if (!m_gl)
            qInfo("no OpenGL: %s", qPrintable(why));
        m_gpu = std::make_unique<Compositor>();
    }

    void cleanupTestCase()
    {
        m_gpu.reset();
        m_rig.destroy();
    }

    void theGainReachesTheCompositorsPicture()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        const QImage plain = m_gpu->renderImage(snap(build({}, 1)), 0);
        const QImage half = m_gpu->renderImage(snap(build(kGain, 0.5)), 0);
        QVERIFY(!plain.isNull() && !half.isNull());
        const QColor p = plain.pixelColor(80, 45);
        const QColor h = half.pixelColor(80, 45);
        QVERIFY2(channelDiff(h, QColor(p.red() / 2, p.green() / 2, p.blue() / 2)) <= 3,
            qPrintable(QStringLiteral("%1 vs %2").arg(p.name(), h.name())));
        // gain 1 is the identity: the plugin says so and the picture is untouched
        QCOMPARE(m_gpu->renderImage(snap(build(kGain, 1.0)), 0), plain);
    }

    void keyframesMoveTheResultFrameByFrame()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        const QColor base = m_gpu->renderImage(snap(build({}, 1)), 0).pixelColor(80, 45);
        int last = -1;
        for (const int frame : {0, 6, 12, 19}) {
            const QColor c = m_gpu->renderImage(snap(build(kGain, 1, true)), frame).pixelColor(80, 45);
            const double expected = 0.2 + 0.8 * frame / 19.0;
            QVERIFY2(std::abs(c.red() - base.red() * expected) <= 3,
                qPrintable(QStringLiteral("frame %1: %2").arg(frame).arg(c.red())));
            QVERIFY(c.red() >= last);
            last = c.red();
        }
    }

    void thePreviewRigDrawsWhatTheCompositorDraws()
    {
        if (!m_gl)
            QSKIP("no OpenGL");
        ParamSet p;
        p.setBase(QStringLiteral("gain"), 0.5);
        QImage src(64, 36, QImage::Format_ARGB32_Premultiplied);
        src.fill(QColor(200, 100, 50));
        QString why;
        const QImage out = m_rig.renderEffect(kGain, p, src, 0, 25, &why);
        QVERIFY2(!out.isNull(), qPrintable(why));
        QVERIFY(channelDiff(out.pixelColor(10, 10), QColor(100, 50, 25)) <= 2);
    }

    void aMissingPluginRendersAsBypass()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        OfxRegistry& reg = OfxRegistry::instance();
        const QImage plain = m_gpu->renderImage(snap(build({}, 1)), 0);
        const QImage half = m_gpu->renderImage(snap(build(kGain, 0.5)), 0);
        QVERIFY(plain != half);
        reg.setEnabled(false);
        QCOMPARE(m_gpu->renderImage(snap(build(kGain, 0.5)), 0), plain);
        reg.setEnabled(true);
        QCOMPARE(m_gpu->renderImage(snap(build(kGain, 0.5)), 0), half);
        TimelineData tl = build({}, 1);
        tl.videoTracks[0].clips[0].effects.append(
            EffectRegistry::instance().createEffect(QStringLiteral("ofx:nobody.has.this"), 99));
        tl.videoTracks[0].clips[0].effects.last().type = QStringLiteral("ofx:nobody.has.this");
        QCOMPARE(m_gpu->renderImage(snap(tl), 0), plain);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestOfxRender>(argc, argv);
}

#include "test_ofxrender.moc"
