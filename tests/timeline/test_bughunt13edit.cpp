// SPDX-License-Identifier: GPL-3.0-or-later
// Bug hunt of the 1.12 round, Edit and Cut page timeline (docs/bughunt-1-12.md): typical editing
// workflows (blade, ripple, roll, slip, slide, linked audio, transitions, markers) are driven
// through the controller and the widget like a user does, and after every step the same
// properties must hold: the timeline is valid, the step is one undo step, undo restores the
// project exactly (compared as saved JSON) and redo brings the result back.
#include "ParityTestUtils.h"
#include "TimelineTestUtils.h"
#include "core/Serialization.h"

#include <QAction>
#include <QJsonDocument>
#include <QSettings>
#include <QTest>
#include <QUndoStack>

#include <functional>

using namespace kk;
using namespace kk::test;
using namespace kk::test::parity;
using namespace kk::timelinemetrics;

namespace {

constexpr TrackRef kA2 {TrackType::Audio, 1};

struct Bench : View
{
    QStringList problems;
    int steps = 0;

    QByteArray json() const
    {
        return QJsonDocument(serialization::timelineToJson(f.view()))
            .toJson(QJsonDocument::Compact);
    }

    /// Runs @p fn and checks the properties above. Returns whether the timeline changed.
    bool step(const char* name, const std::function<void()>& fn)
    {
        ++steps;
        const QByteArray before = json();
        const int undoBefore = undoCount();
        fn();
        const QByteArray after = json();
        QString why;
        if (!kk::editops::checkInvariants(f.view(), &why))
            problems << QStringLiteral("%1: invalid timeline: %2").arg(QLatin1String(name), why);
        const int added = undoCount() - undoBefore;
        if (after == before) {
            if (added != 0)
                problems << QStringLiteral("%1: changed nothing but left %2 undo step(s)")
                                .arg(QLatin1String(name))
                                .arg(added);
            return false;
        }
        if (added != 1) {
            problems << QStringLiteral("%1: %2 undo steps").arg(QLatin1String(name)).arg(added);
            return true; // undo / redo below would not be one step
        }
        f.project.undoStack()->undo();
        if (json() != before)
            problems << QStringLiteral("%1: undo does not restore the timeline")
                            .arg(QLatin1String(name));
        f.project.undoStack()->redo();
        if (json() != after)
            problems << QStringLiteral("%1: redo does not give the same timeline")
                            .arg(QLatin1String(name));
        return true;
    }
};

} // namespace

class TestBughunt13Edit : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_bughunt13edit"); }
    void init() { QSettings().clear(); }

    void keyboardWorkflows()
    {
        Bench b;
        const quint64 a = b.put(kV1, mediaClip(0, 50, 10));
        const quint64 aa = b.put(kA1, mediaClip(0, 50, 10));
        b.link({a, aa});
        const quint64 c = b.put(kV1, mediaClip(50, 40, 20));
        const quint64 ca = b.put(kA1, mediaClip(50, 40, 20));
        b.link({c, ca});
        const quint64 d = b.put(kV1, mediaClip(90, 60, 30));
        const quint64 da = b.put(kA1, mediaClip(90, 60, 30));
        b.link({d, da});
        b.put(kV2, mediaClip(20, 70, 5));
        b.put(kA2, mediaClip(20, 70, 5));
        b.f.project.undoStack()->clear();

        // Blade: playhead, with auto select; then with a selection.
        b.tl()->setPlayhead(25);
        b.step("blade at the playhead", [&] { b.action("edit.split")->trigger(); });
        b.tl()->setPlayhead(70);
        b.f.selection.setClips({c});
        b.step("blade of a selected clip", [&] { b.action("edit.split")->trigger(); });
        // Join back.
        b.f.selection.setClips({c});
        b.step("join through edit", [&] { b.c.joinSelection(); });

        // Markers.
        b.tl()->setPlayhead(10);
        b.step("add marker", [&] { b.c.addMarker(); });
        b.step("delete marker", [&] { b.c.deleteMarkerAtPlayhead(); });
        b.f.selection.setClips({d});
        b.tl()->setPlayhead(100);
        b.step("clip marker", [&] { b.c.addMarkerAtPlayhead(); });
        b.step("clip marker removed", [&] { b.c.addMarkerAtPlayhead(); });

        // Transitions.
        b.f.selection.setClips({c});
        b.step("add transition on a selected clip", [&] { b.c.addDefaultTransition(); });
        b.step("set transition duration", [&] { b.c.setTransitionDuration(c, true, 6); });
        b.step("set transition alignment",
            [&] { b.c.setTransitionAlignment(c, true, TransitionAlignment::StartOnCut); });
        b.step("copy and paste transition", [&] {
            b.c.selectTransition(TransitionRef {c, true});
            b.c.copyTransition();
            b.f.selection.setClips({d});
            b.c.pasteTransition();
        });
        b.step("delete transition", [&] { b.c.deleteTransition(TransitionRef {c, true}); });

        // Ripple, slip, slide with the keys.
        b.c.setTool(TimelineTool::Trim);
        b.f.selection.setClips({d});
        b.step("slip with the keys", [&] { b.c.nudge(3); });
        b.step("slide with the keys", [&] {
            b.c.setSlideClip(d);
            b.c.nudge(-2);
        });
        b.c.setSelectedEdit({a, 0});
        b.step("ripple trim tail with the keys", [&] { b.c.nudge(-4); });
        b.c.setSelectedEdit({0, d});
        b.step("ripple trim head with the keys", [&] { b.c.nudge(5); });
        b.c.setSelectedEdit({a, c});
        b.step("roll with the keys", [&] { b.c.nudge(3); });
        b.c.setSelectedEdit({a, c});
        b.tl()->setPlayhead(40);
        b.step("extend edit", [&] { b.c.extendEditToPlayhead(); });
        b.c.setTool(TimelineTool::Select);

        // Swap, move, speed, duration, freeze, delete, ripple delete, gaps.
        b.f.selection.setClips({c});
        b.step("swap with next", [&] { b.c.swapSelection(1); });
        b.step("move up a track", [&] { b.c.moveSelectionVertically(1); });
        b.step("move down a track", [&] { b.c.moveSelectionVertically(-1); });
        b.f.selection.setClips({d});
        b.step("speed", [&] { b.c.setSelectionSpeed(2.0); });
        b.step("duration", [&] { b.c.setSelectionDuration(20); });
        b.tl()->setPlayhead(95);
        b.step("freeze frame", [&] { b.c.freezeFrame(); });
        b.step("unlink and link", [&] {
            b.c.unlinkSelection();
            b.c.linkSelection();
        });
        b.f.selection.setClips({a});
        b.step("lift", [&] { b.action("edit.delete")->trigger(); });
        b.step("delete gaps", [&] { b.c.deleteGaps(); });
        b.f.selection.setClips({d});
        b.step("ripple delete", [&] { b.action("edit.ripple_delete")->trigger(); });
        b.f.selection.clear();
        b.setMarks(5, 30);
        b.step("ripple delete of a range", [&] { b.action("edit.ripple_delete")->trigger(); });

        QVERIFY2(b.problems.isEmpty(), qPrintable(b.problems.join(QLatin1Char('\n'))));
        QVERIFY(b.steps >= 25);
    }

    // Owner report: a bladed clip with keys on the left half, linked audio: Join Clips works
    // on the selected halves and the edit, as one undo step.
    void joinLinkedHalvesWithDifferentKeys()
    {
        Bench b;
        ClipData keyed = mediaClip(0, 60, 10);
        keyed.transform.animated(keys::Opacity, 1.0).setKey(5, 0.5); // the left half keeps it
        const quint64 v = b.put(kV1, keyed);
        const quint64 a = b.put(kA1, mediaClip(0, 60, 10));
        b.link({v, a});
        b.f.project.undoStack()->clear();
        b.tl()->setPlayhead(25);
        b.step("blade", [&] { b.action("edit.split")->trigger(); });
        const TimelineData& d = b.f.view();
        QCOMPARE(d.videoTracks[0].clips.size(), size_t(2));
        const quint64 v2 = d.videoTracks[0].clips[1].id;
        const quint64 a2 = d.audioTracks[0].clips[1].id;
        b.f.selection.setClips({v, a, v2, a2});
        const int undoBefore = b.undoCount();
        b.c.joinSelection();
        QCOMPARE(d.videoTracks[0].clips.size(), size_t(1));
        QCOMPARE(d.audioTracks[0].clips.size(), size_t(1));
        QCOMPARE(b.undoCount(), undoBefore + 1);
        b.f.project.undoStack()->undo();
        QCOMPARE(d.videoTracks[0].clips.size(), size_t(2));
        QCOMPARE(d.audioTracks[0].clips.size(), size_t(2));
    }

    void mouseWorkflows()
    {
        Bench b;
        const quint64 a = b.put(kV1, mediaClip(0, 40, 20));
        const quint64 aa = b.put(kA1, mediaClip(0, 40, 20));
        b.link({a, aa});
        const quint64 c = b.put(kV1, mediaClip(40, 40, 30));
        const quint64 ca = b.put(kA1, mediaClip(40, 40, 30));
        b.link({c, ca});
        b.put(kV1, mediaClip(80, 40, 40));
        b.put(kV2, mediaClip(30, 40, 5));
        b.f.project.undoStack()->clear();

        // Selection tool: move a clip (V1 -> V2), trim an edge, Alt-drag copy.
        b.step("mouse move to another track", [&] { b.drag(b.at(100, kV1), b.at(100, kV2)); });
        b.step("mouse trim of a tail", [&] {
            const QRectF r = b.clipRect(c);
            const QPoint p(int(r.right()) - 2, int(r.center().y()));
            b.drag(p, p - QPoint(30, 0));
        });
        b.step("alt drag copy", [&] { b.drag(b.at(10, kV1), b.at(60, kV1), Qt::AltModifier); });

        // Trim tool: ripple trim, roll, slip, slide.
        b.c.setTool(TimelineTool::Trim);
        b.step("trim tool ripple", [&] {
            const QRectF r = b.clipRect(a);
            const QPoint p(int(r.right()) - 2, int(r.center().y()));
            b.drag(p, p - QPoint(20, 0));
        });
        b.step("trim tool roll", [&] {
            const QRectF r = b.clipRect(c);
            const QPoint p(int(r.left()), int(r.center().y()));
            b.drag(p, p + QPoint(20, 0));
        });
        b.step("trim tool slide", [&] {
            const QRectF r = b.clipRect(c);
            const QPoint band(int(r.center().x()), int(r.top() + 5));
            b.drag(band, band + QPoint(30, 0));
        });
        b.step("trim tool slip", [&] {
            const QRectF r = b.clipRect(c);
            const QPoint body(int(r.center().x()), int(r.bottom() - 5));
            b.drag(body, body + QPoint(30, 0));
        });
        b.c.setTool(TimelineTool::Blade);
        b.step("blade click", [&] { b.click(b.at(60, kV1)); });
        b.c.setTool(TimelineTool::Select);

        QVERIFY2(b.problems.isEmpty(), qPrintable(b.problems.join(QLatin1Char('\n'))));
        QVERIFY(b.steps >= 8);
    }
};

QTEST_MAIN(TestBughunt13Edit)
#include "test_bughunt13edit.moc"
