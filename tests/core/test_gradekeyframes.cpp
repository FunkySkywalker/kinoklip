// SPDX-License-Identifier: GPL-3.0-or-later
// Color page Keyframes palette and Sizing palette, core part (docs/color-page.md): keyframe rows
// (add, delete, move, Static and Dynamic keys, dissolve types), evaluation of Static against
// Dynamic keys, the Sizing maths (Zoom, Output Sizing transform), saving and loading (grade keys
// with hold flags, Output Sizing, Input Sizing) and undo through Project.
#include "CoreTestUtils.h"
#include "core/EditOps.h"
#include "core/GradeKeyframes.h"
#include "core/Project.h"
#include "core/Serialization.h"
#include "core/Sizing.h"
#include "core/Timeline.h"

#include <QTest>
#include <QUndoStack>

#include <cmath>

using namespace kk;
using namespace kk::test;
namespace go = kk::gradeops;
namespace gk = kk::gradekeys;
namespace gkf = kk::gradekeyframes;
namespace kr = kk::keyrows;

namespace {

bool same(double a, double b, double eps = 1e-6)
{
    return std::abs(a - b) <= eps;
}

} // namespace

class TestGradeKeyframes : public QObject
{
    Q_OBJECT
private slots:
    void addDeleteMoveKeys()
    {
        ClipGrade g;
        const quint64 id = go::initialize(g);
        GradeNode* n = g.node(id);
        n->params.setBase(gk::GainY, 1.5);
        QVERIFY(gkf::frames(*n).isEmpty());
        // Keys every parameter, with the value it has.
        QVERIFY(gkf::addKeys(*n, 10) >= int(gk::allKeys().size()));
        QCOMPARE(gkf::frames(*n), QVector<int64_t> {10});
        QVERIFY(same(n->value(gk::GainY, 10), 1.5));
        QVERIFY(same(n->value(gk::GainY, 0), 1.5)); // one key: constant
        QCOMPARE(gkf::addKeys(*n, 10), 0); // already keyed
        n->params.animated(gk::GainY, 1.0).setKey(30, 2.5);
        QCOMPARE(gkf::frames(*n), (QVector<int64_t> {10, 30}));
        // Move 10 -> 20: every parameter moves, the value follows.
        QVERIFY(gkf::moveKeys(*n, 10, 20) > 0);
        QCOMPARE(gkf::frames(*n), (QVector<int64_t> {20, 30}));
        QVERIFY(!gkf::hasKeyAt(*n, 10));
        QVERIFY(same(n->value(gk::GainY, 20), 1.5));
        // Moving onto another key replaces it.
        QVERIFY(gkf::moveKeys(*n, 20, 30) > 0);
        QCOMPARE(gkf::frames(*n), QVector<int64_t> {30});
        QVERIFY(same(n->value(gk::GainY, 30), 1.5));
        // Delete: the parameter keeps the value it had.
        QVERIFY(gkf::deleteKeys(*n, 30) > 0);
        QVERIFY(gkf::frames(*n).isEmpty());
        QVERIFY(!n->isAnimated());
        QVERIFY(same(n->value(gk::GainY, 5), 1.5));
    }

    void windowKeysAreInTheRow()
    {
        ClipGrade g;
        const quint64 id = go::initialize(g);
        const quint64 win = go::addWindow(g, id, GradeWindowShape::Polygon);
        GradeNode* n = g.node(id);
        GradeWindow* w = g.window(win);
        w->shapeKeys.insert(0, w->points);
        QCOMPARE(gkf::frames(*n), QVector<int64_t> {0});
        QVERIFY(gkf::addKeys(*n, 12) > 0);
        QVERIFY(w->shapeKeys.contains(12)); // the shape shown there is keyed too
        QVERIFY(w->params.isAnimated());
        QVERIFY(gkf::moveKeys(*n, 12, 15) > 0);
        QVERIFY(w->shapeKeys.contains(15) && !w->shapeKeys.contains(12));
        QVERIFY(gkf::deleteKeys(*n, 15) > 0);
        QVERIFY(!w->shapeKeys.contains(15));
        QVERIFY(gkf::hasKeyAt(*n, 0));
    }

    void staticHoldsDynamicInterpolates()
    {
        ClipGrade g;
        const quint64 id = go::initialize(g);
        GradeNode* n = g.node(id);
        AnimatedValue& gain = n->params.animated(gk::GainY, 1.0);
        gain.setKey(0, 1.0, Interp::Linear);
        gain.setKey(20, 2.0, Interp::Linear);
        QCOMPARE(int(gkf::modeAt(*n, 0)), int(kr::Mode::Dynamic));
        QVERIFY(same(n->value(gk::GainY, 10), 1.5));
        // Static: the value is held until the next key, then jumps.
        QVERIFY(gkf::setMode(*n, 0, kr::Mode::Static) >= 1);
        QCOMPARE(int(gkf::modeAt(*n, 0)), int(kr::Mode::Static));
        QVERIFY(same(n->value(gk::GainY, 0), 1.0));
        QVERIFY(same(n->value(gk::GainY, 10), 1.0));
        QVERIFY(same(n->value(gk::GainY, 19), 1.0));
        QVERIFY(same(n->value(gk::GainY, 20), 2.0));
        QVERIFY(same(n->value(gk::GainY, 40), 2.0));
        // Back to Dynamic: interpolates again.
        QVERIFY(gkf::setMode(*n, 0, kr::Mode::Dynamic) >= 1);
        QVERIFY(same(n->value(gk::GainY, 10), 1.5));
        QCOMPARE(gkf::setMode(*n, 0, kr::Mode::Dynamic), 0); // no change
    }

    void dissolveTypes()
    {
        ClipGrade g;
        const quint64 id = go::initialize(g);
        GradeNode* n = g.node(id);
        AnimatedValue& gain = n->params.animated(gk::GainY, 1.0);
        gain.setKey(0, 0.0, Interp::Linear);
        gain.setKey(20, 1.0, Interp::Linear);
        QCOMPARE(int(gkf::interpAt(*n, 0)), int(Interp::Linear));
        // Ease Out leaves the key slowly: below the straight line early on.
        QVERIFY(gkf::setInterp(*n, 0, Interp::EaseOut) >= 1);
        QCOMPARE(int(gkf::interpAt(*n, 0)), int(Interp::EaseOut));
        QVERIFY(n->value(gk::GainY, 5) < 0.2);
        QVERIFY(same(n->value(gk::GainY, 0), 0.0));
        QVERIFY(same(n->value(gk::GainY, 20), 1.0));
        // Smooth, Ease In / Out are accepted, Hold / Custom are not a dissolve type.
        QVERIFY(gkf::setInterp(*n, 0, Interp::Smooth) >= 1);
        QCOMPARE(int(gkf::interpAt(*n, 0)), int(Interp::Smooth));
        QCOMPARE(gkf::setInterp(*n, 0, Interp::Hold), 0);
        QCOMPARE(gkf::setInterp(*n, 0, Interp::Custom), 0);
        // A dissolve type makes a Static key Dynamic.
        gkf::setMode(*n, 0, kr::Mode::Static);
        QVERIFY(gkf::setInterp(*n, 0, Interp::Linear) >= 1);
        QCOMPARE(int(gkf::modeAt(*n, 0)), int(kr::Mode::Dynamic));
        QVERIFY(same(n->value(gk::GainY, 10), 0.5));
    }

    void everyGradeParameterCanBeKeyed()
    {
        ClipGrade g;
        const quint64 id = go::initialize(g);
        GradeNode* n = g.node(id);
        gkf::addKeys(*n, 0);
        for (const QString& key : gk::allKeys()) {
            QVERIFY2(n->params.values.contains(key), qPrintable(key));
            QVERIFY2(n->params.values.value(key).hasKeyAt(0), qPrintable(key));
        }
    }

    void sizingMaths()
    {
        ParamSet p;
        QVERIFY(sizing::isEmpty(p));
        QVERIFY(sizing::outputTransform(p, 0, QSizeF(1920, 1080), QSizeF(1920, 1080)).isIdentity());
        QVERIFY(same(sizing::zoomOf(p, 0), 1.0));
        // Zoom scales Width and Height by one factor and keeps their ratio.
        sizing::setAt(p, keys::ZoomX, 0, 2.0, false);
        sizing::setAt(p, keys::ZoomY, 0, 1.0, false);
        sizing::setZoomAt(p, 0, 4.0, false); // geometric mean 1.414 -> 4
        QVERIFY(same(p.value(keys::ZoomX, 0) / p.value(keys::ZoomY, 0), 2.0));
        QVERIFY(same(sizing::zoomOf(p, 0), 4.0));
        // Output transform: zoom 2 about the centre, then a pan.
        ParamSet q;
        q.setBase(keys::ZoomX, 2.0);
        q.setBase(keys::ZoomY, 2.0);
        q.setBase(keys::PosX, 100.0);
        const QTransform t = sizing::outputTransform(q, 0, QSizeF(1000, 500), QSizeF(1000, 500));
        const QPointF centre = t.map(QPointF(500, 250));
        QVERIFY(same(centre.x(), 600.0) && same(centre.y(), 250.0));
        const QPointF corner = t.map(QPointF(0, 0));
        QVERIFY(same(corner.x(), -400.0) && same(corner.y(), -250.0));
        // The pan scales with the output size (a half size render pans half as far).
        const QTransform half = sizing::outputTransform(q, 0, QSizeF(500, 250), QSizeF(1000, 500));
        QVERIFY(same(half.map(QPointF(250, 125)).x(), 300.0));
        // Rotation: 90 degrees clockwise moves the right edge to the bottom.
        ParamSet r;
        r.setBase(keys::Rotation, 90.0);
        const QPointF right = sizing::outputTransform(r, 0, QSizeF(1000, 1000), QSizeF(1000, 1000))
                                  .map(QPointF(1000, 500));
        QVERIFY(same(right.x(), 500.0, 1e-6) && same(right.y(), 1000.0, 1e-6));
        // Flip H mirrors about the centre.
        ParamSet f;
        f.setStatic(keys::FlipH, true);
        const QPointF mirrored = sizing::outputTransform(f, 0, QSizeF(1000, 500), QSizeF(1000, 500))
                                     .map(QPointF(100, 100));
        QVERIFY(same(mirrored.x(), 900.0) && same(mirrored.y(), 100.0));
    }

    void sizingKeysAnimate()
    {
        ParamSet p;
        sizing::setAt(p, keys::PosX, 0, 0.0, true);
        sizing::setAt(p, keys::PosX, 20, 100.0, true);
        QVERIFY(same(p.value(keys::PosX, 10), 50.0));
        QCOMPARE(keyrows::frames(sizing::rowOf(std::as_const(p))), (QVector<int64_t> {0, 20}));
        // The whole row at once: Zoom, Rotation and the rest get keys too.
        QVERIFY(sizing::addKeys(p, 10) > 0);
        QCOMPARE(keyrows::frames(sizing::rowOf(std::as_const(p))), (QVector<int64_t> {0, 10, 20}));
        QVERIFY(p.values.value(keys::ZoomX).hasKeyAt(10));
        QVERIFY(keyrows::setMode(sizing::rowOf(p), 0, kr::Mode::Static) >= 1);
        QVERIFY(same(p.value(keys::PosX, 5), 0.0)); // held
        QVERIFY(keyrows::moveKeys(sizing::rowOf(p), 10, 12) > 0);
        QVERIFY(keyrows::deleteKeys(sizing::rowOf(p), 12) > 0);
        QCOMPARE(keyrows::frames(sizing::rowOf(std::as_const(p))), (QVector<int64_t> {0, 20}));
        // Without Auto Keyframe a parameter without keys just changes.
        ParamSet plain;
        sizing::setAt(plain, keys::PosY, 7, 30.0, false);
        QVERIFY(!plain.values.value(keys::PosY).isAnimated());
        QVERIFY(same(plain.value(keys::PosY, 99), 30.0));
        // ... and one with keys gets a key at the frame.
        sizing::setAt(p, keys::PosX, 7, 33.0, false);
        QVERIFY(p.values.value(keys::PosX).hasKeyAt(7));
    }

    void savingAndLoading()
    {
        TimelineData tl = makeDefaultTimeline(QStringLiteral("T"), Rational {24, 1});
        // Grade keys with a Static key and a dissolve type.
        ClipData clip;
        clip.kind = ClipKind::Solid;
        clip.start = 0;
        clip.duration = 40;
        ClipGrade g;
        const quint64 id = go::initialize(g);
        GradeNode* n = g.node(id);
        AnimatedValue& gain = n->params.animated(gk::GainY, 1.0);
        gain.setKey(0, 1.0);
        gain.setKey(30, 2.0);
        gkf::setMode(*n, 0, kr::Mode::Static);
        gkf::setInterp(*n, 30, Interp::Smooth);
        clip.grade = g;
        sizing::setAt(clip.transform, keys::PosX, 0, 5.0, true);
        sizing::setAt(clip.transform, keys::PosX, 10, 15.0, true);
        clip.transform.setStatic(keys::FlipV, true);
        tl.outputSizing.setBase(keys::ZoomX, 1.25);
        sizing::setAt(tl.outputSizing, keys::Rotation, 3, 4.0, true);
        sizing::setAt(tl.outputSizing, keys::Rotation, 9, 8.0, true);
        tl.outputSizing.setStatic(keys::FlipH, true);
        editops::overwriteClip(tl, TrackRef {TrackType::Video, 0}, clip);

        const QJsonObject json = serialization::timelineToJson(tl);
        QVERIFY(json.contains(QLatin1String("outputSizing")));
        TimelineData back;
        QString error;
        QVERIFY2(serialization::timelineFromJson(json, back, &error, nullptr), qPrintable(error));
        QCOMPARE(back.outputSizing, tl.outputSizing);
        QVERIFY(back.outputSizing.staticValue(keys::FlipH, false).toBool());
        QVERIFY(same(back.outputSizing.value(keys::Rotation, 6), 6.0));
        const ClipData& c = back.videoTracks[0].clips[0];
        QCOMPARE(c.grade, g);
        QCOMPARE(c.transform, clip.transform);
        const GradeNode* bn = c.grade.node(id);
        QVERIFY(bn);
        QCOMPARE(int(gkf::modeAt(*bn, 0)), int(kr::Mode::Static));
        QCOMPARE(int(gkf::interpAt(*bn, 30)), int(Interp::Smooth));
        QVERIFY(same(bn->value(gk::GainY, 15), 1.0)); // still held after the round trip
        // An empty Output Sizing is not written (older builds and files stay identical).
        TimelineData plain = makeDefaultTimeline(QStringLiteral("P"), Rational {24, 1});
        QVERIFY(!serialization::timelineToJson(plain).contains(QLatin1String("outputSizing")));
    }

    void undoThroughProject()
    {
        Project project;
        project.beginEdit(QStringLiteral("Add"));
        ClipData clip;
        clip.kind = ClipKind::Solid;
        clip.start = 0;
        clip.duration = 40;
        const quint64 clipId
            = editops::overwriteClip(project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
        project.endEdit();
        const quint64 uid = project.activeTimelineUid();
        const int steps = project.undoStack()->index();
        // Input Sizing is the clip's transform: the same data the Inspector edits.
        ParamSet t = project.clipTransform(uid, clipId);
        sizing::setAt(t, keys::PosX, 0, 12.0, false);
        QVERIFY(project.setClipTransform(uid, clipId, t, QStringLiteral("Pan")));
        QCOMPARE(project.undoStack()->index(), steps + 1);
        const ClipData* stored = editops::clipById(project.timeline()->data(), clipId);
        QVERIFY(stored && same(stored->transform.value(keys::PosX, 0), 12.0));
        // Output Sizing.
        ParamSet o;
        o.setBase(keys::ZoomX, 2.0);
        QVERIFY(project.setOutputSizing(uid, o, QStringLiteral("Zoom")));
        QCOMPARE(project.undoStack()->index(), steps + 2);
        QCOMPARE(project.outputSizing(uid), o);
        // The same value is no new undo step.
        QVERIFY(project.setOutputSizing(uid, o, QStringLiteral("Zoom")));
        QCOMPARE(project.undoStack()->index(), steps + 2);
        project.undoStack()->undo();
        QVERIFY(sizing::isEmpty(project.outputSizing(uid)));
        project.undoStack()->undo();
        QVERIFY(same(project.clipTransform(uid, clipId).value(keys::PosX, 0), 0.0));
        project.undoStack()->redo();
        QVERIFY(same(project.clipTransform(uid, clipId).value(keys::PosX, 0), 12.0));
    }
};

QTEST_GUILESS_MAIN(TestGradeKeyframes)
#include "test_gradekeyframes.moc"
