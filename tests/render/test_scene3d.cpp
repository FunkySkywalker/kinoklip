// SPDX-License-Identifier: GPL-3.0-or-later
// Spec 130: the 3D nodes of the Compositing page (Image Plane 3D, Shape3D, Text3D, Merge3D,
// Camera3D, lights, Renderer3D) and the 3D viewer. Matrix maths and the scene as pure functions;
// Renderer3D's pixels on the GPU and the CPU renderer.
#include "RenderTestUtil.h"
#include "core/CompOps.h"
#include "core/EditOps.h"
#include "core/NodeRegistry.h"
#include "core/Serialization.h"
#include "render/CompScene3D.h"
#include "render/Compositor.h"
#include "render/TimelineEval.h"

#include <QTest>

#include <cmath>
#include <memory>

using namespace kk;
using namespace kk::test;
namespace co = kk::compops;
namespace s3 = kk::render::scene3d;
namespace k = kk::nodekeys;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

bool closeTo(double a, double b, double tol = 1e-4)
{
    return std::abs(a - b) <= tol;
}

bool closeVec(const QVector3D& a, const QVector3D& b, float tol = 1e-4f)
{
    return (a - b).length() <= tol;
}

TimelineData compTimeline(const Composition& comp)
{
    TimelineData tl = emptyTimeline(kW, kH);
    ClipData c = solidClip(tl, QColor(0, 0, 0), 0, 50);
    c.composition = comp;
    addClip(tl, TrackType::Video, 0, c);
    return tl;
}

/// A picture node of one colour.
quint64 addColor(Composition& c, QColor color)
{
    const quint64 id = co::addNode(c, nodetypes::Background, {});
    c.node(id)->params.setStatic(k::Color, color);
    return id;
}

/// An Image Plane 3D showing @p texture.
quint64 addPlane(Composition& c, quint64 texture, double size = 1.0)
{
    const quint64 id = co::addNode(c, nodetypes::ImagePlane3D, {});
    c.node(id)->params.setBase(k::Size, size);
    co::connect(c, texture, 0, id, nodeports::Texture);
    return id;
}

/// A Shape3D of colour @p color.
quint64 addShape(Composition& c, const QString& shape, double size, QColor color)
{
    const quint64 id = co::addNode(c, nodetypes::Shape3D, {});
    c.node(id)->params.setStatic(k::Shape, shape);
    c.node(id)->params.setBase(k::Size, size);
    c.node(id)->params.setStatic(k::Color, color);
    return id;
}

quint64 addMerge(Composition& c, std::initializer_list<quint64> scenes)
{
    const quint64 id = co::addNode(c, nodetypes::Merge3D, {});
    const QString ports[] = {nodeports::Scene1, nodeports::Scene2, nodeports::Scene3,
        nodeports::Scene4};
    int i = 0;
    for (quint64 s : scenes)
        co::connect(c, s, 0, id, ports[i++]);
    return id;
}

/// A Renderer3D of @p scene (no anti-aliasing unless @p samples says so).
quint64 addRenderer(Composition& c, quint64 scene, int samples = 1)
{
    const quint64 id = co::addNode(c, nodetypes::Renderer3D, {});
    c.node(id)->params.setStatic(k::AntiAlias, QString::number(samples));
    co::connect(c, scene, 0, id, nodeports::Input);
    return id;
}

quint64 addLight(Composition& c, const QString& type, double intensity)
{
    const quint64 id = co::addNode(c, type, {});
    c.node(id)->params.setBase(k::Intensity, intensity);
    return id;
}

} // namespace

class TestScene3D : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;

    QVector<Compositor*> backends() { return {m_gpu.get(), m_cpu.get()}; }

    QImage nodeImage(Compositor* c, const TimelineData& tl, quint64 id, int64_t frame)
    {
        const RenderedFramePtr f = c->renderCompositionNode(snap(tl), tl.videoTracks[0].clips[0].id,
                                        id, frame, {}, Quality::Full, OutputImage, nullptr, nullptr, 0)
                                       .get();
        return f && f->ok ? f->image : QImage();
    }

    static QString who(const Compositor* all, const Compositor* gpu)
    {
        return all == gpu ? QStringLiteral("GPU") : QStringLiteral("CPU");
    }

    /// The same node on both renderers; fails on a null picture or a difference above @p tol.
    bool sameOnBoth(const TimelineData& tl, quint64 id, int64_t frame, QImage* out, int tol = 2)
    {
        const QImage a = nodeImage(m_gpu.get(), tl, id, frame);
        const QImage b = nodeImage(m_cpu.get(), tl, id, frame);
        if (a.isNull() || a.size() != b.size())
            return false;
        int worst = 0;
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x) {
                const Px p = px(a, x, y), q = px(b, x, y);
                worst = std::max({worst, std::abs(p.r - q.r), std::abs(p.g - q.g),
                    std::abs(p.b - q.b), std::abs(p.a - q.a)});
            }
        if (out)
            *out = b;
        return worst <= tol;
    }

private slots:
    void initTestCase()
    {
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
    }
    void cleanupTestCase()
    {
        m_gpu.reset();
        m_cpu.reset();
    }

    // ---- Port types ----

    void sceneOutputsConnectOnlyToSceneInputs()
    {
        Composition c = co::defaultComposition();
        const quint64 bg = addColor(c, Qt::red);
        const quint64 plane = addPlane(c, bg);
        const quint64 shape = addShape(c, shapes3d::Cube, 0.3, Qt::blue);
        const quint64 mg = co::addNode(c, nodetypes::Merge3D, {});
        const quint64 rn = co::addNode(c, nodetypes::Renderer3D, {});
        const quint64 merge2d = co::addNode(c, nodetypes::Merge, {});
        QCOMPARE(int(co::connect(c, plane, 0, mg, nodeports::Scene1)), int(co::ConnectError::None));
        QCOMPARE(int(co::connect(c, shape, 0, mg, nodeports::Scene2)), int(co::ConnectError::None));
        QCOMPARE(int(co::connect(c, mg, 0, rn, nodeports::Input)), int(co::ConnectError::None));
        // A scene never feeds a picture input, and a picture never feeds a scene input.
        QVERIFY(co::connect(c, plane, 0, merge2d, nodeports::Foreground) != co::ConnectError::None);
        QVERIFY(co::connect(c, plane, 0, c.mediaOutId, nodeports::Input) != co::ConnectError::None);
        QVERIFY(co::connect(c, bg, 0, mg, nodeports::Scene3) != co::ConnectError::None);
        QVERIFY(co::connect(c, bg, 0, rn, nodeports::Input) != co::ConnectError::None);
        // A scene does not feed a particle input; the rendered picture feeds the picture graph.
        const quint64 pr = co::addNode(c, nodetypes::PRender, {});
        QVERIFY(co::connect(c, mg, 0, pr, nodeports::Input) != co::ConnectError::None);
        QCOMPARE(int(co::connect(c, rn, 0, merge2d, nodeports::Foreground)),
            int(co::ConnectError::None));
        // A texture is a picture input.
        QVERIFY(co::connect(c, mg, 0, plane, nodeports::Texture) != co::ConnectError::None);
        QVERIFY(NodeRegistry::instance().find(nodetypes::Merge3D)->outputKind == PortKind::Scene3D);
        QVERIFY(NodeRegistry::instance().find(nodetypes::Renderer3D)->outputKind == PortKind::Image);
    }

    void addingSceneNodesFromTheToolbar()
    {
        Composition c = co::defaultComposition();
        const co::AddResult p = co::addNodeAfter(c, nodetypes::ImagePlane3D, c.mediaOutId);
        QVERIFY(p.node != 0);
        QCOMPARE(p.merge, quint64(0)); // no 2D Merge is made for a 3D source
        const co::AddResult m = co::addNodeAfter(c, nodetypes::Merge3D, p.node);
        QCOMPARE(c.node(m.node)->inputs.value(nodeports::Scene1).nodeId, p.node);
        const co::AddResult r = co::addNodeAfter(c, nodetypes::Renderer3D, m.node);
        QCOMPARE(c.node(r.node)->inputs.value(nodeports::Input).nodeId, m.node);
    }

    void everyControlHasATooltip()
    {
        for (const QString& type : {nodetypes::ImagePlane3D, nodetypes::Shape3D, nodetypes::Text3D,
                 nodetypes::Merge3D, nodetypes::Camera3D, nodetypes::AmbientLight,
                 nodetypes::DirectionalLight, nodetypes::PointLight, nodetypes::SpotLight,
                 nodetypes::Renderer3D}) {
            const NodeDescriptor* d = NodeRegistry::instance().find(type);
            QVERIFY(d);
            QVERIFY(!d->description.isEmpty());
            QVERIFY(d->category == NodeCategory::ThreeD);
            for (const ParamDescriptor& p : d->params)
                QVERIFY2(!p.tooltip.isEmpty(), qPrintable(type + "/" + p.key));
        }
    }

    void sceneNodesHaveNoCommonControlsButRendererHas()
    {
        const NodeRegistry& r = NodeRegistry::instance();
        QVERIFY(!r.hasCommonControls(nodetypes::Merge3D));
        QVERIFY(!r.hasCommonControls(nodetypes::Camera3D));
        QVERIFY(r.hasCommonControls(nodetypes::Renderer3D));
    }

    void everyTransformIsKeyframable()
    {
        for (const QString& type : {nodetypes::ImagePlane3D, nodetypes::Shape3D, nodetypes::Text3D,
                 nodetypes::Merge3D, nodetypes::Camera3D, nodetypes::PointLight,
                 nodetypes::SpotLight, nodetypes::DirectionalLight}) {
            const NodeDescriptor* d = NodeRegistry::instance().find(type);
            for (const QString& key : {k::RotateX, k::RotateY, k::RotateZ, k::ScaleUniform}) {
                const ParamDescriptor* p = d->param(key);
                if (p)
                    QVERIFY2(p->animatable, qPrintable(type + "/" + key));
            }
            for (const QString& key : {k::TranslateX, k::TranslateY, k::TranslateZ}) {
                const ParamDescriptor* p = d->param(key);
                if (p)
                    QVERIFY2(p->animatable, qPrintable(type + "/" + key));
            }
        }
        QVERIFY(NodeRegistry::instance().find(nodetypes::Camera3D)->param(k::TranslateZ));
    }

    // ---- Matrices ----

    void transformMatrixAppliesScaleThenRotationThenTranslation()
    {
        ParamSet p;
        p.setBase(k::TranslateX, 1);
        p.setBase(k::TranslateY, 2);
        p.setBase(k::TranslateZ, 3);
        QVERIFY(closeVec(s3::transformMatrix(p, 0).map(QVector3D(0, 0, 0)), QVector3D(1, 2, 3)));
        p.setBase(k::RotateZ, 90);
        // (1, 0, 0) turns to (0, 1, 0), then moves.
        QVERIFY(closeVec(s3::transformMatrix(p, 0).map(QVector3D(1, 0, 0)), QVector3D(1, 3, 3)));
        p.setBase(k::ScaleUniform, 2);
        p.setBase(k::ScaleX, 0.5);
        // Scale x by 1 (2 * 0.5), then the 90 degree turn, then the move.
        QVERIFY(closeVec(s3::transformMatrix(p, 0).map(QVector3D(1, 0, 0)), QVector3D(1, 3, 3)));
        QVERIFY(closeVec(s3::transformMatrix(p, 0).map(QVector3D(0, 1, 0)), QVector3D(-1, 2, 3)));
        // Without scale (cameras, lights) the scale is ignored.
        QVERIFY(closeVec(
            s3::transformMatrix(p, 0, false).map(QVector3D(0, 1, 0)), QVector3D(0, 2, 3)));
    }

    void rotationsAreAppliedXThenYThenZ()
    {
        ParamSet p;
        p.setBase(k::RotateX, 90);
        p.setBase(k::RotateY, 90);
        // (0, 1, 0): X turns it to (0, 0, 1), then Y turns that to (1, 0, 0).
        QVERIFY(closeVec(s3::transformMatrix(p, 0).map(QVector3D(0, 1, 0)), QVector3D(1, 0, 0)));
    }

    void transformsAreKeyframed()
    {
        ParamSet p;
        p.animated(k::TranslateX, 0).setKey(0, 0.0);
        p.animated(k::TranslateX, 0).setKey(10, 5.0);
        p.animated(k::RotateY, 0).setKey(0, 0.0);
        p.animated(k::RotateY, 0).setKey(10, 90.0);
        const QMatrix4x4 a = s3::transformMatrix(p, 0);
        const QMatrix4x4 b = s3::transformMatrix(p, 10);
        QVERIFY(closeVec(a.map(QVector3D(0, 0, 1)), QVector3D(0, 0, 1)));
        QVERIFY(closeVec(b.map(QVector3D(0, 0, 1)), QVector3D(6, 0, 0)));
        const QVector3D mid = s3::transformMatrix(p, 5).map(QVector3D(0, 0, 0));
        QVERIFY(mid.x() > 0.5f && mid.x() < 4.5f);
    }

    void cameraAngleOfViewFromFocalLength()
    {
        ParamSet p;
        QVERIFY(closeTo(s3::angleOfViewOf(p, 0), s3::kDefaultAngleOfView, 1e-3));
        p.setStatic(k::ViewBy, cameraviews::FocalLength);
        p.setBase(k::FocalLength, 50);
        QVERIFY(closeTo(s3::angleOfViewOf(p, 0), s3::kDefaultAngleOfView, 1e-3));
        p.setBase(k::FocalLength, 25); // 2 atan(18 / 25)
        QVERIFY(closeTo(s3::angleOfViewOf(p, 0), 2 * std::atan(0.72) * 180 / M_PI, 1e-3));
        p.setStatic(k::ViewBy, cameraviews::AngleOfView);
        p.setBase(k::AngleOfView, 90);
        QVERIFY(closeTo(s3::angleOfViewOf(p, 0), 90, 1e-9));
    }

    void defaultCameraFillsTheFrameWithAUnitPlane()
    {
        const s3::Camera cam = s3::defaultCamera();
        const QMatrix4x4 vp = s3::projectionMatrix(cam, 16.0 / 9.0) * s3::viewMatrix(cam);
        const QVector4D c = vp.map(QVector4D(0.5f, 0, 0, 1));
        QVERIFY(closeTo(c.x() / c.w(), 1.0, 1e-3)); // the plane's right edge is the frame's edge
        const QVector4D top = vp.map(QVector4D(0, 0.5f * 9.0f / 16.0f, 0, 1));
        QVERIFY(closeTo(top.y() / top.w(), 1.0, 1e-3));
        const QVector4D mid = vp.map(QVector4D(0, 0, 0, 1));
        QVERIFY(closeTo(mid.x(), 0) && closeTo(mid.y(), 0));
    }

    void cameraNearAndFarMapToTheDepthRange()
    {
        s3::Camera cam;
        cam.nearClip = 0.5;
        cam.farClip = 50;
        const QMatrix4x4 p = s3::projectionMatrix(cam, 1.0);
        const QVector4D nearP = p.map(QVector4D(0, 0, -0.5f, 1));
        const QVector4D farP = p.map(QVector4D(0, 0, -50.0f, 1));
        QVERIFY(closeTo(nearP.z() / nearP.w(), -1, 1e-4));
        QVERIFY(closeTo(farP.z() / farP.w(), 1, 1e-4));
    }

    void viewMatrixIgnoresScaleAndFollowsTheCamera()
    {
        ParamSet p;
        p.setBase(k::TranslateX, 2);
        p.setBase(k::TranslateZ, 5);
        p.setBase(k::RotateY, 90);
        CompNode n;
        n.type = nodetypes::Camera3D;
        n.params = p;
        QMatrix4x4 parent;
        parent.scale(3, 3, 3); // Merge3D scale: the camera is not scaled by it
        const s3::Camera cam = s3::cameraOf(n, 0, parent);
        const QMatrix4x4 v = s3::viewMatrix(cam);
        // The camera sits at the translation (scaled by the parent) and looks along +X after the turn.
        const QVector3D eye = cam.model.column(3).toVector3D();
        QVERIFY(closeVec(v.map(eye), QVector3D(0, 0, 0)));
        const QVector3D ahead = eye + QVector3D(1, 0, 0); // minus Z turned by 90 degrees about Y is -X...
        const QVector3D camAhead = v.map(ahead);
        QVERIFY(closeTo(std::abs(camAhead.z()), 1, 1e-4));
    }

    // ---- Rendering ----

    void imagePlaneFacingTheCameraMatchesThe2dImage()
    {
        Composition c = co::defaultComposition();
        const quint64 bg = co::addNode(c, nodetypes::Background, {});
        c.node(bg)->params.setStatic(k::Mode, backgroundmodes::FourCorner);
        c.node(bg)->params.setStatic(k::Color, QColor(255, 0, 0));
        c.node(bg)->params.setStatic(k::Color2, QColor(0, 255, 0));
        c.node(bg)->params.setStatic(k::Color3, QColor(0, 0, 255));
        c.node(bg)->params.setStatic(k::Color4, QColor(255, 255, 0));
        const quint64 plane = addPlane(c, bg);
        const quint64 rn = addRenderer(c, plane);
        co::connect(c, rn, 0, c.mediaOutId, nodeports::Input);
        const TimelineData tl = compTimeline(c);
        for (Compositor* b : backends()) {
            const QImage flat = nodeImage(b, tl, bg, 0);
            const QImage drawn = nodeImage(b, tl, rn, 0);
            QVERIFY(!flat.isNull() && !drawn.isNull());
            int worst = 0;
            for (int y = 0; y < kH; ++y)
                for (int x = 0; x < kW; ++x) {
                    const Px p = px(flat, x, y), q = px(drawn, x, y);
                    worst = std::max({worst, std::abs(p.r - q.r), std::abs(p.g - q.g),
                        std::abs(p.b - q.b), std::abs(p.a - q.a)});
                }
            QVERIFY2(worst <= 4, qPrintable(who(b, m_gpu.get()) + ": " + QString::number(worst)));
        }
        QVERIFY(sameOnBoth(tl, rn, 0, nullptr, 2));
        // With the default 2 x 2 anti-aliasing the picture is still the same within a small blur.
        c.node(rn)->params.setStatic(k::AntiAlias, QStringLiteral("2"));
        const TimelineData aa = compTimeline(c);
        const QImage flat = nodeImage(m_cpu.get(), aa, bg, 0);
        const QImage drawn = nodeImage(m_cpu.get(), aa, rn, 0);
        long total = 0;
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                const Px p = px(flat, x, y), q = px(drawn, x, y);
                total += std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b);
            }
        QVERIFY2(double(total) / (kW * kH * 3) < 1.0, qPrintable(QString::number(total)));
    }

    void imagePlaneSizeAndMoveShowOnBothRenderers()
    {
        Composition c = co::defaultComposition();
        const quint64 red = addColor(c, Qt::red);
        const quint64 plane = addPlane(c, red, 0.5);
        c.node(plane)->params.setBase(k::TranslateX, 0.25);
        const quint64 rn = addRenderer(c, plane);
        const TimelineData tl = compTimeline(c);
        for (Compositor* b : backends()) {
            const QImage img = nodeImage(b, tl, rn, 0);
            QVERIFY(!img.isNull());
            const QString w = who(b, m_gpu.get());
            // Size 0.5 centred at x = 0.25 units: frame x from 80 + 80 = 160 to 320 (half the width).
            QVERIFY2(near(px(img, 240, 90), 255, 0, 0, 255, 2), qPrintable(w));
            QVERIFY2(px(img, 150, 90).a <= 2, qPrintable(w));
            QVERIFY2(px(img, 240, 10).a <= 2, qPrintable(w)); // above the plane (it is 90 px high)
            QVERIFY2(px(img, 240, 60).a >= 250, qPrintable(w));
        }
    }

    void aPlaneWithoutAPictureDrawsNothing()
    {
        Composition c = co::defaultComposition();
        const quint64 plane = co::addNode(c, nodetypes::ImagePlane3D, {});
        const quint64 rn = addRenderer(c, plane);
        const TimelineData tl = compTimeline(c);
        for (Compositor* b : backends())
            QVERIFY(px(nodeImage(b, tl, rn, 0), kW / 2, kH / 2).a <= 2);
    }

    void opacityAndDisabledNodes()
    {
        Composition c = co::defaultComposition();
        const quint64 red = addColor(c, Qt::red);
        const quint64 plane = addPlane(c, red);
        c.node(plane)->params.setBase(k::Opacity, 0.5);
        const quint64 rn = addRenderer(c, plane);
        TimelineData tl = compTimeline(c);
        for (Compositor* b : backends())
            QVERIFY(near(px(nodeImage(b, tl, rn, 0), kW / 2, kH / 2), 128, 0, 0, 128, 3));
        c.node(plane)->enabled = false;
        tl = compTimeline(c);
        for (Compositor* b : backends())
            QVERIFY(px(nodeImage(b, tl, rn, 0), kW / 2, kH / 2).a <= 2);
    }

    void cubeSilhouette()
    {
        // A cube of side 0.3 facing the camera: the front face (0.15 closer than the origin) is a
        // square of 2 * 0.15 * 2.7778 / (25/18 - 0.15) = 0.672 of the frame width, 107.6 px.
        Composition c = co::defaultComposition();
        const quint64 cube = addShape(c, shapes3d::Cube, 0.3, QColor(0, 255, 0));
        const quint64 rn = addRenderer(c, cube);
        const TimelineData tl = compTimeline(c);
        const int cx = kW / 2, cy = kH / 2;
        for (Compositor* b : backends()) {
            const QImage img = nodeImage(b, tl, rn, 0);
            QVERIFY(!img.isNull());
            const QString w = who(b, m_gpu.get());
            QVERIFY2(near(px(img, cx, cy), 0, 255, 0, 255, 2), qPrintable(w));
            for (int d : {-52, 52}) {
                QVERIFY2(px(img, cx + d, cy + 40).a >= 250, qPrintable(w + " inside"));
                QVERIFY2(px(img, cx + 40, cy + d).a >= 250, qPrintable(w + " inside"));
            }
            for (int d : {-57, 57}) {
                QVERIFY2(px(img, cx + d, cy).a <= 2, qPrintable(w + " outside"));
                QVERIFY2(px(img, cx, cy + d).a <= 2, qPrintable(w + " outside"));
            }
            // The sides are hidden behind the front face; nothing else is drawn.
            QVERIFY(px(img, 5, 5).a <= 2);
        }
    }

    void cubeTurnedShowsASideAndStaysInsideItsBounds()
    {
        Composition c = co::defaultComposition();
        const quint64 cube = addShape(c, shapes3d::Cube, 0.3, QColor(0, 255, 0));
        c.node(cube)->params.setBase(k::RotateY, 45);
        const quint64 rn = addRenderer(c, cube);
        const TimelineData tl = compTimeline(c);
        for (Compositor* b : backends()) {
            const QImage img = nodeImage(b, tl, rn, 0);
            // A cube turned 45 degrees is a diamond: wider than the side at the middle, a
            // vertical edge at the centre; the corner of the frame is empty.
            QVERIFY(px(img, kW / 2, kH / 2).a >= 250);
            int left = kW, right = 0;
            for (int x = 0; x < kW; ++x)
                if (px(img, x, kH / 2).a > 128) {
                    left = std::min(left, x);
                    right = std::max(right, x);
                }
            QVERIFY2(right - left > 100 && right - left < 150, qPrintable(QString::number(right - left)));
            QVERIFY(px(img, 5, 5).a <= 2);
        }
    }

    void everyShapeDrawsSomething()
    {
        for (const QString& shape : {shapes3d::Plane, shapes3d::Cube, shapes3d::Sphere,
                 shapes3d::Cylinder, shapes3d::Cone}) {
            const s3::Mesh m = s3::makeShape(shape, 1.0, 8, 8);
            QVERIFY2(m.indices.size() >= 6, qPrintable(shape));
            QCOMPARE(m.positions.size(), m.normals.size());
            QCOMPARE(m.positions.size(), m.uvs.size());
            for (quint32 i : m.indices)
                QVERIFY(qsizetype(i) < m.positions.size());
            for (const QVector3D& n : m.normals)
                QVERIFY2(closeTo(n.length(), 1.0, 1e-3), qPrintable(shape));
        }
        // Subdivisions change the triangle count.
        QVERIFY(s3::makeShape(shapes3d::Sphere, 1, 24, 24).indices.size()
            > s3::makeShape(shapes3d::Sphere, 1, 8, 8).indices.size());
        QCOMPARE(s3::makeShape(shapes3d::Plane, 1, 4, 4).indices.size(), qsizetype(4 * 4 * 6));
        // A unit sphere has radius 0.5.
        for (const QVector3D& p : s3::makeShape(shapes3d::Sphere, 1, 12, 12).positions)
            QVERIFY(closeTo(p.length(), 0.5, 1e-4));
    }

    void shapeTextureIsMultipliedByTheColour()
    {
        Composition c = co::defaultComposition();
        const quint64 white = addColor(c, QColor(255, 255, 255));
        const quint64 shape = addShape(c, shapes3d::Plane, 1.0, QColor(255, 128, 0));
        co::connect(c, white, 0, shape, nodeports::Texture);
        const quint64 rn = addRenderer(c, shape);
        const TimelineData tl = compTimeline(c);
        for (Compositor* b : backends())
            QVERIFY(near(px(nodeImage(b, tl, rn, 0), kW / 2, kH / 2), 255, 128, 0, 255, 3));
    }

    void merge3dOrdersByDepthNotByInput()
    {
        // A red plane in front (z = +0.3) and a green one behind (z = -0.3), overlapping at the centre.
        Composition c = co::defaultComposition();
        const quint64 red = addColor(c, Qt::red);
        const quint64 green = addColor(c, Qt::green);
        const quint64 pr = addPlane(c, red, 0.6);
        const quint64 pg = addPlane(c, green, 0.6);
        c.node(pr)->params.setBase(k::TranslateZ, 0.3);
        c.node(pg)->params.setBase(k::TranslateZ, -0.3);
        c.node(pg)->params.setBase(k::TranslateX, 0.3);
        for (int order = 0; order < 2; ++order) {
            Composition d = c;
            const quint64 mg = order ? addMerge(d, {pg, pr}) : addMerge(d, {pr, pg});
            const quint64 rn = addRenderer(d, mg);
            const TimelineData tl = compTimeline(d);
            for (Compositor* b : backends()) {
                const QImage img = nodeImage(b, tl, rn, 0);
                const QString w = who(b, m_gpu.get()) + QString::number(order);
                QVERIFY2(near(px(img, kW / 2, kH / 2), 255, 0, 0, 255, 2), qPrintable(w));
                // Right of the red plane only the green one is there.
                QVERIFY2(near(px(img, kW / 2 + 130, kH / 2), 0, 255, 0, 255, 2), qPrintable(w));
            }
        }
    }

    void merge3dTransformMovesEverythingInIt()
    {
        Composition c = co::defaultComposition();
        const quint64 red = addColor(c, Qt::red);
        const quint64 plane = addPlane(c, red, 0.25);
        const quint64 mg = addMerge(c, {plane});
        c.node(mg)->params.setBase(k::TranslateX, 0.25);
        const quint64 rn = addRenderer(c, mg);
        const TimelineData tl = compTimeline(c);
        for (Compositor* b : backends()) {
            const QImage img = nodeImage(b, tl, rn, 0);
            // The 0.25 wide plane sat at 120..200; the Merge3D moves it to 200..280.
            QVERIFY(near(px(img, 240, kH / 2), 255, 0, 0, 255, 2));
            QVERIFY(px(img, 160, kH / 2).a <= 2);
        }
        // Scale 2 on the Merge3D doubles it.
        c.node(mg)->params.setBase(k::TranslateX, 0);
        c.node(mg)->params.setBase(k::ScaleUniform, 2);
        const TimelineData big = compTimeline(c);
        for (Compositor* b : backends()) {
            const QImage img = nodeImage(b, big, rn, 0);
            QVERIFY(near(px(img, kW / 2 + 70, kH / 2), 255, 0, 0, 255, 2)); // 0.5 wide: 80..240
            QVERIFY(px(img, kW / 2 + 90, kH / 2).a <= 2);
        }
    }

    void cameraMovesAndZooms()
    {
        Composition c = co::defaultComposition();
        const quint64 red = addColor(c, Qt::red);
        const quint64 plane = addPlane(c, red, 1.0);
        const quint64 cam = co::addNode(c, nodetypes::Camera3D, {});
        c.node(cam)->params.setBase(k::TranslateZ, 2.5); // further away: the plane is smaller
        const quint64 mg = addMerge(c, {plane, cam});
        const quint64 rn = addRenderer(c, mg);
        TimelineData tl = compTimeline(c);
        // Half width of the plane: 160 * (25/18) / 2.5 = 88.9 px.
        for (Compositor* b : backends()) {
            const QImage img = nodeImage(b, tl, rn, 0);
            QVERIFY(px(img, kW / 2 + 85, kH / 2).a >= 250);
            QVERIFY(px(img, kW / 2 + 93, kH / 2).a <= 2);
        }
        // A focal length of 25 mm sees wider: the plane is smaller again (about 0.72 / 0.36 = half).
        c.node(cam)->params.setBase(k::TranslateZ, 25.0 / 18.0);
        c.node(cam)->params.setStatic(k::ViewBy, cameraviews::FocalLength);
        c.node(cam)->params.setBase(k::FocalLength, 25);
        tl = compTimeline(c);
        for (Compositor* b : backends()) {
            const QImage img = nodeImage(b, tl, rn, 0);
            // tan(35.5 deg) = 0.72; the edge at x = 0.5 / (1.3889 * 0.72) * 160 = 80 px.
            QVERIFY(px(img, kW / 2 + 77, kH / 2).a >= 250);
            QVERIFY(px(img, kW / 2 + 83, kH / 2).a <= 2);
        }
        // The camera is keyframed like every other parameter.
        c.node(cam)->params.setStatic(k::ViewBy, cameraviews::AngleOfView);
        c.node(cam)->params.animated(k::TranslateZ, 1.0).setKey(0, 25.0 / 18.0);
        c.node(cam)->params.animated(k::TranslateZ, 1.0).setKey(10, 5.0);
        tl = compTimeline(c);
        for (Compositor* b : backends()) {
            QVERIFY(px(nodeImage(b, tl, rn, 0), kW / 2 + 150, kH / 2).a >= 250);
            QVERIFY(px(nodeImage(b, tl, rn, 10), kW / 2 + 150, kH / 2).a <= 2);
        }
    }

    void cameraRotationLooksAway()
    {
        Composition c = co::defaultComposition();
        const quint64 red = addColor(c, Qt::red);
        const quint64 plane = addPlane(c, red, 0.5);
        const quint64 cam = co::addNode(c, nodetypes::Camera3D, {});
        c.node(cam)->params.setBase(k::RotateY, 10); // turns to the left (camera -Z towards -X)
        const quint64 mg = addMerge(c, {plane, cam});
        const quint64 rn = addRenderer(c, mg);
        const TimelineData tl = compTimeline(c);
        for (Compositor* b : backends()) {
            const QImage img = nodeImage(b, tl, rn, 0);
            // The plane drifts to the right in the picture when the camera pans left.
            int left = kW, right = 0;
            for (int x = 0; x < kW; ++x)
                if (px(img, x, kH / 2).a > 128) {
                    left = std::min(left, x);
                    right = std::max(right, x);
                }
            QVERIFY2(left > kW / 2 - 30 && (left + right) / 2 > kW / 2 + 40, qPrintable(QString("%1 %2").arg(left).arg(right)));
        }
    }

    void lightsChangeTheBrightness()
    {
        Composition c = co::defaultComposition();
        const quint64 grey = addShape(c, shapes3d::Plane, 1.0, QColor(200, 200, 200));
        const auto centre = [&](Composition& comp, quint64 scene, int samples = 1) {
            const quint64 rn = addRenderer(comp, scene, samples);
            const TimelineData tl = compTimeline(comp);
            QVector<int> out;
            for (Compositor* b : backends())
                out << px(nodeImage(b, tl, rn, 0), kW / 2, kH / 2).r;
            return out;
        };
        // No light in the scene: the surface shows its own colour.
        for (int v : centre(c, grey))
            QCOMPARE(v, 200);
        // Ambient light alone: colour * intensity.
        {
            Composition d = c;
            const quint64 amb = addLight(d, nodetypes::AmbientLight, 0.5);
            for (int v : centre(d, addMerge(d, {grey, amb})))
                QVERIFY2(std::abs(v - 100) <= 2, qPrintable(QString::number(v)));
        }
        // A directional light straight onto the plane, plus ambient.
        {
            Composition d = c;
            const quint64 amb = addLight(d, nodetypes::AmbientLight, 0.25);
            const quint64 dir = addLight(d, nodetypes::DirectionalLight, 0.5);
            for (int v : centre(d, addMerge(d, {grey, amb, dir})))
                QVERIFY2(std::abs(v - 150) <= 2, qPrintable(QString::number(v)));
        }
        // Turned 60 degrees the light hits at cos 60 = 0.5 of the strength.
        {
            Composition d = c;
            const quint64 dir = addLight(d, nodetypes::DirectionalLight, 1.0);
            d.node(dir)->params.setBase(k::RotateX, 60);
            for (int v : centre(d, addMerge(d, {grey, dir})))
                QVERIFY2(std::abs(v - 100) <= 2, qPrintable(QString::number(v)));
        }
        // Turned away (the light shines from behind the plane's back... both sides are lit by a
        // light on the viewer's side only): a light behind the plane lights nothing.
        {
            Composition d = c;
            const quint64 dir = addLight(d, nodetypes::DirectionalLight, 1.0);
            d.node(dir)->params.setBase(k::RotateY, 180);
            for (int v : centre(d, addMerge(d, {grey, dir})))
                QVERIFY2(v <= 2, qPrintable(QString::number(v)));
        }
        // A point light in front: full strength under it, less at the side.
        {
            Composition d = c;
            const quint64 pl = addLight(d, nodetypes::PointLight, 1.0);
            d.node(pl)->params.setBase(k::TranslateZ, 1.0);
            const quint64 rn = addRenderer(d, addMerge(d, {grey, pl}));
            const TimelineData tl = compTimeline(d);
            for (Compositor* b : backends()) {
                const QImage img = nodeImage(b, tl, rn, 0);
                QVERIFY(std::abs(px(img, kW / 2, kH / 2).r - 200) <= 2);
                // At x = 0.5 units (the plane's edge) cos = 1 / sqrt(1.25) = 0.894.
                const int edge = px(img, kW - 2, kH / 2).r;
                QVERIFY2(edge > 150 && edge < 190, qPrintable(QString::number(edge)));
            }
        }
        // A spot light lights only its cone.
        {
            Composition d = c;
            const quint64 sl = addLight(d, nodetypes::SpotLight, 1.0);
            d.node(sl)->params.setBase(k::TranslateZ, 1.0);
            d.node(sl)->params.setBase(k::ConeAngle, 20);
            d.node(sl)->params.setBase(k::Penumbra, 0);
            const quint64 rn = addRenderer(d, addMerge(d, {grey, sl}));
            const TimelineData tl = compTimeline(d);
            for (Compositor* b : backends()) {
                const QImage img = nodeImage(b, tl, rn, 0);
                QVERIFY(px(img, kW / 2, kH / 2).r >= 190);
                // tan(20) * 1 = 0.364 units = 116 px from the centre.
                QVERIFY(px(img, kW / 2 + 100, kH / 2).r >= 100);
                QVERIFY2(px(img, kW / 2 + 130, kH / 2).r <= 2,
                    qPrintable(QString::number(px(img, kW / 2 + 130, kH / 2).r)));
            }
        }
    }

    void lightingSwitchAndBlinnPhong()
    {
        Composition c = co::defaultComposition();
        const quint64 grey = addShape(c, shapes3d::Plane, 1.0, QColor(51, 51, 51));
        c.node(grey)->params.setBase(k::Specular, 0.5);
        c.node(grey)->params.setBase(k::Shininess, 16);
        const quint64 pl = addLight(c, nodetypes::PointLight, 1.0);
        c.node(pl)->params.setBase(k::TranslateZ, 1.0);
        const quint64 mg = addMerge(c, {grey, pl});
        const quint64 rn = addRenderer(c, mg);
        TimelineData tl = compTimeline(c);
        // Lambert: the colour only (the light faces the plane squarely: 51 * 1).
        for (Compositor* b : backends())
            QVERIFY(std::abs(px(nodeImage(b, tl, rn, 0), kW / 2, kH / 2).r - 51) <= 2);
        // Blinn-Phong: the highlight at the centre adds 0.5 * 255 = 128.
        c.node(rn)->params.setStatic(k::Shading, shadings3d::BlinnPhong);
        tl = compTimeline(c);
        for (Compositor* b : backends()) {
            const QImage img = nodeImage(b, tl, rn, 0);
            const int mid = px(img, kW / 2, kH / 2).r;
            QVERIFY2(std::abs(mid - 179) <= 3, qPrintable(QString::number(mid)));
            QVERIFY(px(img, kW - 2, kH / 2).r < mid - 30); // the highlight falls off
        }
        // Lighting off: the plain colour again.
        c.node(rn)->params.setStatic(k::Lighting, false);
        tl = compTimeline(c);
        for (Compositor* b : backends())
            QVERIFY(std::abs(px(nodeImage(b, tl, rn, 0), kW / 2, kH / 2).r - 51) <= 1);
    }

    void sphereIsShadedRound()
    {
        Composition c = co::defaultComposition();
        const quint64 ball = addShape(c, shapes3d::Sphere, 0.5, QColor(255, 255, 255));
        const quint64 dir = addLight(c, nodetypes::DirectionalLight, 1.0);
        c.node(dir)->params.setBase(k::RotateY, 60); // from the right
        const quint64 rn = addRenderer(c, addMerge(c, {ball, dir}));
        const TimelineData tl = compTimeline(c);
        for (Compositor* b : backends()) {
            const QImage img = nodeImage(b, tl, rn, 0);
            const int right = px(img, kW / 2 + 40, kH / 2).r;
            const int left = px(img, kW / 2 - 40, kH / 2).r;
            QVERIFY2(right > left + 80, qPrintable(QString("%1 %2").arg(left).arg(right)));
            QVERIFY(px(img, 5, 5).a <= 2);
        }
    }

    void text3dDrawsTextOnBothRenderers()
    {
        Composition c = co::defaultComposition();
        const quint64 tx = co::addNode(c, nodetypes::Text3D, {});
        c.node(tx)->params.setStatic(k::Text, QStringLiteral("MMMM"));
        c.node(tx)->params.setBase(k::FontSize, 80);
        const quint64 rn = addRenderer(c, tx);
        const TimelineData tl = compTimeline(c);
        QImage cpu;
        QVERIFY(sameOnBoth(tl, rn, 0, &cpu, 2));
        int lit = 0;
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
                if (px(cpu, x, y).a > 128)
                    ++lit;
        QVERIFY2(lit > 500, qPrintable(QString::number(lit)));
        QVERIFY(px(cpu, 2, 2).a <= 2);
        // Moved and turned like any 3D object: tilting it about Y squeezes it.
        c.node(tx)->params.setBase(k::RotateY, 60);
        const TimelineData tilted = compTimeline(c);
        const QImage t = nodeImage(m_cpu.get(), tilted, rn, 0);
        int litTilted = 0;
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
                if (px(t, x, y).a > 128)
                    ++litTilted;
        QVERIFY(litTilted > 100 && litTilted < lit);
    }

    void extrudedBevelledTextRendersWithItsSidesLit()
    {
        Composition c = co::defaultComposition();
        const quint64 tx = co::addNode(c, nodetypes::Text3D, {});
        ParamSet& p = c.node(tx)->params;
        p.setStatic(k::Text, QStringLiteral("MOA"));
        p.setBase(k::FontSize, 90);
        p.setStatic(k::Bold, true);
        p.setBase(k::ExtrudeDepth, 0.08);
        p.setBase(k::BevelDepth, 0.015);
        p.setBase(k::BevelWidth, 0.008);
        p.setStatic(k::SeparateColors, true);
        p.setStatic(k::Color, QColor(255, 0, 0));
        p.setStatic(k::BevelColor, QColor(0, 0, 255));
        p.setStatic(k::SideColor, QColor(0, 255, 0));
        p.setBase(k::RotateY, 38);
        p.setBase(k::RotateX, -14);
        const quint64 sun = addLight(c, nodetypes::DirectionalLight, 1.0);
        c.node(sun)->params.setBase(k::RotateY, 20);
        c.node(sun)->params.setBase(k::RotateX, -25);
        const quint64 amb = addLight(c, nodetypes::AmbientLight, 0.35);
        const quint64 rn = addRenderer(c, addMerge(c, {tx, sun, amb}), 2);
        co::connect(c, rn, 0, c.mediaOutId, nodeports::Input);
        const TimelineData tl = compTimeline(c);
        QImage img;
        QVERIFY(sameOnBoth(tl, rn, 0, &img, 3));
        int red = 0, green = 0, blue = 0;
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                const Px q = px(img, x, y);
                if (q.a < 200)
                    continue;
                red += q.r > 60 && q.g < 40 && q.b < 40;
                green += q.g > 60 && q.r < 40 && q.b < 40;
                blue += q.b > 60 && q.r < 40 && q.g < 40;
            }
        QVERIFY2(red > 400, qPrintable(QString::number(red)));
        QVERIFY2(green > 40, qPrintable(QString::number(green))); // the sides show when turned
        QVERIFY2(blue > 10, qPrintable(QString::number(blue))); // and so do the bevels
        QVERIFY(px(img, 2, 2).a <= 2);
        const QString still = qEnvironmentVariable("KK_TEXT3D_STILL");
        if (!still.isEmpty())
            QVERIFY(img.convertToFormat(QImage::Format_ARGB32).save(still));
        // The same scene again gives the same picture (the mesh cache changes nothing).
        QVERIFY(nodeImage(m_cpu.get(), tl, rn, 0) == nodeImage(m_cpu.get(), tl, rn, 0));
        // With the depth back at 0 it is the flat title again.
        c.node(tx)->params.setBase(k::ExtrudeDepth, 0);
        const QImage flat = nodeImage(m_cpu.get(), compTimeline(c), rn, 0);
        QVERIFY(flat != img);
    }

    void rendererOnBothBackendsAgreeOnACompoundScene()
    {
        Composition c = co::defaultComposition();
        const quint64 bg = co::addNode(c, nodetypes::Background, {});
        c.node(bg)->params.setStatic(k::Mode, backgroundmodes::Gradient);
        const quint64 plane = addPlane(c, bg, 0.8);
        c.node(plane)->params.setBase(k::RotateY, 25);
        const quint64 ball = addShape(c, shapes3d::Sphere, 0.3, QColor(255, 180, 40));
        c.node(ball)->params.setBase(k::TranslateX, 0.2);
        c.node(ball)->params.setBase(k::TranslateZ, 0.3);
        const quint64 cone = addShape(c, shapes3d::Cone, 0.2, QColor(40, 120, 255));
        c.node(cone)->params.setBase(k::TranslateX, -0.3);
        c.node(cone)->params.setBase(k::RotateZ, 20);
        const quint64 sp = addLight(c, nodetypes::SpotLight, 1.0);
        c.node(sp)->params.setBase(k::TranslateZ, 2);
        c.node(sp)->params.setBase(k::TranslateY, 1);
        c.node(sp)->params.setBase(k::RotateX, -20);
        const quint64 amb = addLight(c, nodetypes::AmbientLight, 0.3);
        const quint64 rn = addRenderer(c, addMerge(c, {plane, ball, cone, sp, amb}), 2);
        co::connect(c, rn, 0, c.mediaOutId, nodeports::Input);
        const TimelineData tl = compTimeline(c);
        QImage img;
        QVERIFY(sameOnBoth(tl, rn, 0, &img, 3));
        QVERIFY(px(img, kW / 2 + 64, kH / 2 + 10).a >= 200);
    }

    void viewerOrbitMaths()
    {
        s3::Orbit o;
        o.yaw = 0;
        o.pitch = 0;
        o.distance = 3;
        const s3::Camera front = s3::orbitCamera(o);
        QVERIFY(closeVec(front.model.column(3).toVector3D(), QVector3D(0, 0, 3), 1e-4f));
        o.yaw = 90;
        QVERIFY(closeVec(s3::orbitCamera(o).model.column(3).toVector3D(), QVector3D(3, 0, 0), 1e-4f));
        o.yaw = 0;
        o.pitch = 90;
        QVERIFY(s3::orbitCamera(o).model.column(3).y() > 2.9f);
        // The target is at the middle of the picture whatever the orbit.
        o.yaw = 37;
        o.pitch = 21;
        o.target = QVector3D(1, 2, 3);
        const s3::Camera cam = s3::orbitCamera(o);
        const QVector3D t = s3::viewMatrix(cam).map(o.target);
        QVERIFY(closeTo(t.x(), 0, 1e-4) && closeTo(t.y(), 0, 1e-4) && closeTo(t.z(), -3, 1e-4));
        // Dragging: right turns the yaw, down tilts the pitch, the tilt stops short of the poles.
        const s3::Orbit a = s3::orbited(s3::Orbit(), 20, 10);
        QVERIFY(a.yaw < 30 && a.pitch > 20);
        QVERIFY(s3::orbited(s3::Orbit(), 0, 1e6).pitch <= 89);
        QVERIFY(s3::orbited(s3::Orbit(), 0, -1e6).pitch >= -89);
        // Wheel: a notch in is 10 % nearer; out undoes it; the distance stays bounded.
        QVERIFY(closeTo(s3::zoomed(s3::Orbit(), 1).distance, 2.7, 1e-9));
        QVERIFY(closeTo(s3::zoomed(s3::zoomed(s3::Orbit(), 3), -3).distance, 3.0, 1e-9));
        QVERIFY(s3::zoomed(s3::Orbit(), 1000).distance >= 0.05);
        QVERIFY(s3::zoomed(s3::Orbit(), -1000).distance <= 5000);
        // Pan: dragging right moves the target left (the scene follows the mouse), and the
        // scene moves by what the mouse moved: a point at the target shifts by dx pixels.
        o = s3::Orbit();
        o.yaw = 0;
        o.pitch = 0;
        const s3::Orbit pan = s3::panned(o, 100, 0, 800);
        QVERIFY(pan.target.x() < 0 && closeTo(pan.target.y(), 0, 1e-5));
        const double perPixel = 2 * 3 * std::tan(s3::kDefaultAngleOfView * M_PI / 360) / 800;
        QVERIFY(closeTo(pan.target.x(), -100 * perPixel, 1e-4));
        QVERIFY(s3::panned(o, 0, 50, 800).target.y() > 0);
    }

    void viewerSharedOrbit()
    {
        const s3::Orbit before = s3::viewerOrbit();
        s3::Orbit o;
        o.yaw = 77;
        s3::setViewerOrbit(o);
        QCOMPARE(s3::viewerOrbit().yaw, 77.0);
        s3::setViewerOrbit(before);
    }

    void aSceneNodeInAViewerShowsTheOrbitViewWithAGrid()
    {
        Composition c = co::defaultComposition();
        const quint64 red = addColor(c, Qt::red);
        const quint64 plane = addPlane(c, red, 0.5);
        const quint64 mg = addMerge(c, {plane});
        const TimelineData tl = compTimeline(c);
        const s3::Orbit saved = s3::viewerOrbit();
        s3::Orbit o;
        o.yaw = 0;
        o.pitch = 25;
        o.distance = 3;
        s3::setViewerOrbit(o);
        QImage a;
        QVERIFY(sameOnBoth(tl, mg, 0, &a, 2));
        QVERIFY(!a.isNull());
        // The background is opaque; the plane is red in the middle; the grid shows below.
        QCOMPARE(px(a, 3, 3).a, 255);
        const Px mid = px(a, kW / 2, kH / 2);
        QVERIFY2(mid.r > 200 && mid.g < 60, qPrintable(describe(mid)));
        int gridRows = 0;
        for (int y = kH / 2 + 30; y < kH; ++y) {
            const Px p = px(a, 5, y);
            const Px bg = px(a, 3, 3);
            if (std::abs(p.r - bg.r) + std::abs(p.g - bg.g) + std::abs(p.b - bg.b) > 20)
                ++gridRows;
        }
        QVERIFY2(gridRows > 0, "no grid line found");
        // Orbiting changes the picture.
        o.yaw = 60;
        s3::setViewerOrbit(o);
        const QImage b = nodeImage(m_cpu.get(), tl, mg, 0);
        QVERIFY(a != b);
        s3::setViewerOrbit(saved);
        // The Renderer3D image does not depend on the viewer's orbit.
        const quint64 rn = addRenderer(c, mg);
        const TimelineData tl2 = compTimeline(c);
        const QImage r1 = nodeImage(m_cpu.get(), tl2, rn, 0);
        s3::setViewerOrbit(o);
        const QImage r2 = nodeImage(m_cpu.get(), tl2, rn, 0);
        s3::setViewerOrbit(saved);
        QVERIFY(r1 == r2);
    }

    void aCameraInTheViewerOrbitStillShowsTheScene()
    {
        Composition c = co::defaultComposition();
        const quint64 cube = addShape(c, shapes3d::Cube, 0.5, QColor(0, 200, 0));
        const quint64 cam = co::addNode(c, nodetypes::Camera3D, {});
        const quint64 mg = addMerge(c, {cube, cam});
        const TimelineData tl = compTimeline(c);
        for (Compositor* b : backends()) {
            const QImage img = nodeImage(b, tl, mg, 0);
            const Px mid = px(img, kW / 2, kH / 2);
            QVERIFY2(mid.g > 100, qPrintable(describe(mid)));
        }
    }

    // ---- Project ----

    void sceneGraphSurvivesSaveAndLoad()
    {
        Composition c = co::defaultComposition();
        const quint64 red = addColor(c, Qt::red);
        const quint64 plane = addPlane(c, red, 0.4);
        c.node(plane)->params.setBase(k::RotateY, 33);
        c.node(plane)->params.animated(k::TranslateX, 0).setKey(0, 0.0);
        c.node(plane)->params.animated(k::TranslateX, 0).setKey(10, 0.5);
        const quint64 cube = addShape(c, shapes3d::Cone, 0.3, QColor(10, 200, 30));
        const quint64 cam = co::addNode(c, nodetypes::Camera3D, {});
        c.node(cam)->params.setStatic(k::ViewBy, cameraviews::FocalLength);
        c.node(cam)->params.setBase(k::FocalLength, 35);
        const quint64 sl = addLight(c, nodetypes::SpotLight, 0.9);
        const quint64 mg = addMerge(c, {plane, cube, cam, sl});
        const quint64 rn = addRenderer(c, mg, 4);
        c.node(rn)->params.setStatic(k::Shading, shadings3d::BlinnPhong);
        co::connect(c, rn, 0, c.mediaOutId, nodeports::Input);
        const Composition back
            = serialization::compositionFromJson(serialization::compositionToJson(c));
        QCOMPARE(back.nodes.size(), c.nodes.size());
        QCOMPARE(back.node(mg)->inputs.value(nodeports::Scene3).nodeId, cam);
        QCOMPARE(back.node(mg)->inputs.value(nodeports::Scene4).nodeId, sl);
        QCOMPARE(back.node(rn)->inputs.value(nodeports::Input).nodeId, mg);
        QCOMPARE(back.node(plane)->inputs.value(nodeports::Texture).nodeId, red);
        QCOMPARE(back.node(cube)->params.staticValue(k::Shape).toString(), shapes3d::Cone);
        QCOMPARE(back.node(rn)->params.staticValue(k::AntiAlias).toString(), QStringLiteral("4"));
        QCOMPARE(back.node(rn)->params.staticValue(k::Shading).toString(), shadings3d::BlinnPhong);
        QCOMPARE(back.node(cam)->params.staticValue(k::ViewBy).toString(), cameraviews::FocalLength);
        QVERIFY(closeTo(back.node(plane)->params.value(k::TranslateX, 5, 0), 0.25, 0.1));
        const s3::Scene a = s3::buildScene(c, mg, 5, {kW, kH}, QSizeF(kW, kH), 1.0);
        const s3::Scene b = s3::buildScene(back, mg, 5, {kW, kH}, QSizeF(kW, kH), 1.0);
        QCOMPARE(a.meshes.size(), 2);
        QCOMPARE(b.meshes.size(), 2);
        QCOMPARE(b.cameras.size(), 1);
        QCOMPARE(b.lights.size(), 1);
        QVERIFY(a.meshes[0].model == b.meshes[0].model);
        QVERIFY(closeTo(b.cameras[0].angleOfView, a.cameras[0].angleOfView, 1e-9));
        const TimelineData t1 = compTimeline(c), t2 = compTimeline(back);
        QVERIFY(nodeImage(m_cpu.get(), t1, rn, 5) == nodeImage(m_cpu.get(), t2, rn, 5));
    }

    void textureNodesAreFoundThroughMerges()
    {
        Composition c = co::defaultComposition();
        const quint64 red = addColor(c, Qt::red);
        const quint64 plane = addPlane(c, red);
        const quint64 shape = addShape(c, shapes3d::Cube, 1, Qt::white);
        const quint64 inner = addMerge(c, {plane});
        const quint64 mg = addMerge(c, {inner, shape});
        const QVector<quint64> found = s3::textureNodes(c, mg);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found[0], plane);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestScene3D>(argc, argv);
}
#include "test_scene3d.moc"
