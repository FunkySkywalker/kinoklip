// SPDX-License-Identifier: GPL-3.0-or-later
// Several edit points selected and trimmed together (docs/behavior-spec.md section 104): the
// core function (editops::multiTrimTogether / applyMultiTrim), Ctrl+click selection, mouse
// drags, keyboard nudges, typed offsets, E, undo, and Paste Attributes > Speed with Ripple.
#include "../core/CoreTestUtils.h"
#include "ParityTestUtils.h"
#include "TimelineTestUtils.h"
#include "core/EditOps.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using kk::test::initTimelineTestCase;

namespace ops = kk::editops;
using namespace kk::test::parity;

namespace {

ops::TrimGroup group(std::initializer_list<ops::TrimEdge> edges)
{
    ops::TrimGroup g;
    for (const ops::TrimEdge& e : edges)
        g.edges.append(e);
    return g;
}

ops::MultiTrim plan(std::initializer_list<ops::TrimGroup> groups)
{
    ops::MultiTrim m;
    for (const ops::TrimGroup& g : groups)
        m.trims.append(g);
    return m;
}

/// A timeline with the default tracks and the clips given as (track, start, duration, source
/// frame); returns their ids.
struct Core
{
    TimelineData d = makeDefaultTimeline(QStringLiteral("T"), Rational {25, 1}, 1920, 1080, 3, 3);
    quint64 put(TrackRef ref, int64_t start, int64_t duration, int64_t src = 50)
    {
        return ops::overwriteClip(d, ref, mediaClip(start, duration, src));
    }
};

} // namespace

class TestMultiEdit : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_multiedit"); }

    // 104.2: heads of clips on one track ripple by the same amount; each later clip follows
    // the trims before it.
    void headsOnOneTrackRippleTogether()
    {
        Core t;
        t.put(kV1, 0, 20);
        const quint64 b = t.put(kV1, 30, 20);
        const quint64 c = t.put(kV1, 60, 20);
        const int64_t got = ops::multiTrimTogether(
            t.d, plan({group({{b, true}}), group({{c, true}})}), 3, true);
        QCOMPARE(got, int64_t(3));
        QCOMPARE(layout(t.d, kV1), QStringLiteral("0:20 30:17 57:17"));
        KK_VERIFY_INVARIANTS(t.d);
    }

    // 104.3: the whole set stops at the first limit (a head with only 2 frames of handle).
    void setStopsAtTheFirstLimit()
    {
        Core t;
        const quint64 a = t.put(kV1, 20, 20, 2); // 2 frames of media before the in point
        const quint64 b = t.put(kV2, 20, 20, 40);
        const int64_t got = ops::multiTrimTogether(
            t.d, plan({group({{a, true}}), group({{b, true}})}), -9, false);
        QCOMPARE(got, int64_t(-2));
        QCOMPARE(layout(t.d, kV1), QStringLiteral("18:22"));
        QCOMPARE(layout(t.d, kV2), QStringLiteral("18:22"));
    }

    // 104.2: a tail and a head move opposite ways when dragged together (asymmetric trim).
    void tailAndHeadMoveOppositeWays()
    {
        Core t;
        const quint64 a = t.put(kV1, 0, 20);
        const quint64 b = t.put(kV2, 40, 20);
        QCOMPARE(ops::multiTrimTogether(
                     t.d, plan({group({{a, false}}), group({{b, true}})}), 5, false),
            int64_t(5));
        QCOMPARE(layout(t.d, kV1), QStringLiteral("0:25")); // tail 5 to the right
        QCOMPARE(layout(t.d, kV2), QStringLiteral("35:25")); // head 5 to the left
        // The leading edge decides the direction: head first, the tail goes the other way.
        Core u;
        const quint64 a2 = u.put(kV1, 0, 20);
        const quint64 b2 = u.put(kV2, 40, 20);
        ops::multiTrimTogether(u.d, plan({group({{b2, true}}), group({{a2, false}})}), 5, false);
        QCOMPARE(layout(u.d, kV2), QStringLiteral("45:15"));
        QCOMPARE(layout(u.d, kV1), QStringLiteral("0:15"));
    }

    // Both halves of one cut selected separately, ripple: both clips get shorter, the track closes
    // up (Resolve manual, asymmetric trimming in one track).
    void bothHalvesOfACutRippleShorter()
    {
        Core t;
        const quint64 a = t.put(kV1, 0, 20);
        const quint64 b = t.put(kV1, 20, 20);
        t.put(kV1, 40, 10);
        QCOMPARE(ops::multiTrimTogether(
                     t.d, plan({group({{a, false}}), group({{b, true}})}), -3, true),
            int64_t(-3));
        // tail 3 back (a: 17), head 3 forward on the same side of the cut (b: 17)
        QCOMPARE(layout(t.d, kV1), QStringLiteral("0:17 17:17 34:10"));
        KK_VERIFY_INVARIANTS(t.d);
    }

    // 104.5: the In and Out of one clip ripple it towards its middle; the tracks that follow
    // lose both removed pieces.
    void inAndOutOfOneClipRippleTowardsTheMiddle()
    {
        Core t;
        const quint64 a = t.put(kV1, 10, 30);
        t.put(kV1, 50, 10);
        t.put(kV2, 0, 100, 0);
        // Tail first (the lead): tail 4 back, head 4 forward.
        QCOMPARE(ops::multiTrimTogether(
                     t.d, plan({group({{a, false}}), group({{a, true}})}), -4, true),
            int64_t(-4));
        QCOMPARE(layout(t.d, kV1), QStringLiteral("10:22 42:10"));
        // V2 lost [10,14) (the head) and [36,40) (the tail).
        QCOMPARE(layout(t.d, kV2), QStringLiteral("0:10 10:22 32:60"));
        KK_VERIFY_INVARIANTS(t.d);
    }

    // 104.4: a dragged total is handed out one frame at a time, left to right.
    void roundRobinSharesTheTotal()
    {
        Core t;
        const quint64 a = t.put(kV1, 0, 20);
        const quint64 b = t.put(kV1, 30, 20);
        const quint64 c = t.put(kV1, 60, 20);
        QCOMPARE(ops::multiTrimTogether(t.d,
                     plan({group({{c, false}}), group({{a, false}}), group({{b, false}})}), -5,
                     true, ops::TrimShare::RoundRobin),
            int64_t(-5));
        // two frames from the first two, one from the last
        QCOMPARE(layout(t.d, kV1), QStringLiteral("0:18 28:18 56:19"));
        // The same amount for each instead.
        Core u;
        const quint64 a2 = u.put(kV1, 0, 20);
        const quint64 b2 = u.put(kV1, 30, 20);
        ops::multiTrimTogether(
            u.d, plan({group({{a2, false}}), group({{b2, false}})}), -5, true);
        QCOMPARE(layout(u.d, kV1), QStringLiteral("0:15 25:15"));
    }

    // 104.5: ripple and roll cannot be combined: nothing happens.
    void rippleWithARollInTheSetDoesNothing()
    {
        Core t;
        const quint64 a = t.put(kV1, 0, 20);
        const quint64 b = t.put(kV1, 20, 20);
        const quint64 c = t.put(kV2, 0, 20);
        ops::MultiTrim m = plan({group({{c, false}})});
        ops::RollGroup r;
        r.pairs.append({a, b});
        m.rolls.append(r);
        const TimelineData before = t.d;
        QCOMPARE(ops::multiTrimTogether(t.d, m, 3, true), int64_t(0));
        QVERIFY(t.d == before);
        // Without ripple: the roll and the resize go together.
        QCOMPARE(ops::multiTrimTogether(t.d, m, 3, false), int64_t(3));
        QCOMPARE(layout(t.d, kV1), QStringLiteral("0:23 23:17"));
        QCOMPARE(layout(t.d, kV2), QStringLiteral("0:23"));
    }

    // 104.7: two edits on one track, a third track follows by the sum; a track holding a trimmed
    // clip only by its own trims. A linked clip on the same edge counts once.
    void otherTracksShiftOnceBySumOfTrims()
    {
        Core t;
        const quint64 a = t.put(kV1, 0, 10);
        const quint64 b = t.put(kV1, 20, 10);
        const quint64 x = t.put(kV2, 0, 100);
        t.d.videoTracks[1].syncLock = true;
        Q_UNUSED(x);
        QCOMPARE(ops::multiTrimTogether(
                     t.d, plan({group({{a, false}}), group({{b, false}})}), -2, true),
            int64_t(-2));
        QCOMPARE(layout(t.d, kV1), QStringLiteral("0:8 18:8"));
        // V2 loses the frames [8,10) and [28,30) and closes up: one 8-frame piece, then 18 and 70.
        QCOMPARE(layout(t.d, kV2), QStringLiteral("0:8 8:18 26:70"));
        KK_VERIFY_INVARIANTS(t.d);
    }

    void linkedEdgesOfOneEditShiftOtherTracksOnce()
    {
        Core t;
        const quint64 v = t.put(kV1, 0, 10);
        const quint64 a = t.put(kA1, 0, 10);
        ops::linkClips(t.d, {v, a});
        const quint64 v2 = t.put(kV2, 40, 10); // later, on a track of its own
        const quint64 w = t.put(kV1, 30, 10);
        Q_UNUSED(v2);
        QCOMPARE(ops::multiTrimTogether(
                     t.d, plan({group({{v, false}, {a, false}}), group({{w, false}})}), 4, true),
            int64_t(4));
        QCOMPARE(layout(t.d, kV1), QStringLiteral("0:14 34:14"));
        QCOMPARE(layout(t.d, kA1), QStringLiteral("0:14"));
        // V2 moves by 4 (v and a together) + 4 (w), not by 12.
        QCOMPARE(layout(t.d, kV2), QStringLiteral("48:10"));
    }

    // ---- Controller and widget ----

    // 104.1: Ctrl+click adds edit points (and takes them out again); a plain click goes back to one.
    void ctrlClickSelectsSeveralEditPoints()
    {
        View v;
        const quint64 a = v.put(kV1, mediaClip(10, 20));
        const quint64 b = v.put(kV2, mediaClip(40, 20));
        v.click(v.at(30, kV1) - QPoint(5, 0)); // tail of a
        v.click(v.at(40, kV2) + QPoint(5, 0), Qt::ControlModifier); // head of b
        QCOMPARE(v.c.selectedEdits(),
            (QVector<EditPointSelection> {{a, 0}, {0, b}}));
        v.click(v.at(30, kV1) - QPoint(5, 0), Qt::ControlModifier); // takes the tail out
        QCOMPARE(v.c.selectedEdits(), (QVector<EditPointSelection> {{0, b}}));
        v.click(v.at(30, kV1) - QPoint(5, 0), Qt::ControlModifier);
        QCOMPARE(v.c.selectedEdits().size(), 2);
        v.click(v.at(60, kV2) - QPoint(5, 0)); // plain click: just that one
        QCOMPARE(v.c.selectedEdits(), (QVector<EditPointSelection> {{b, 0}}));
        // Clicking a clip clears them all.
        v.click(v.at(15, kV1));
        QVERIFY(v.c.selectedEdits().isEmpty());
        QVERIFY(v.c.selectedEdit().isEmpty());
    }

    // A plain click without drag on one of several selected edit points keeps just that one.
    void plainClickOnSelectedEditPointCollapses()
    {
        View v;
        const quint64 a = v.put(kV1, mediaClip(10, 20));
        v.put(kV2, mediaClip(40, 20));
        v.click(v.at(30, kV1) - QPoint(5, 0));
        v.click(v.at(40, kV2) + QPoint(5, 0), Qt::ControlModifier);
        QCOMPARE(v.c.selectedEdits().size(), 2);
        v.click(v.at(30, kV1) - QPoint(5, 0));
        QCOMPARE(v.c.selectedEdits(), (QVector<EditPointSelection> {{a, 0}}));
    }

    // 104.2: dragging one of the selected edit points moves all of them, one undo step.
    void dragMovesAllSelectedEditPoints()
    {
        View v;
        v.put(kV1, mediaClip(10, 20));
        v.put(kV2, mediaClip(10, 30, 160)); // 10 frames of media after its end
        v.click(v.at(30, kV1) - QPoint(5, 0));
        v.click(v.at(40, kV2) - QPoint(5, 0), Qt::ControlModifier);
        const int undo0 = v.undoCount();
        v.drag(v.at(30, kV1) - QPoint(5, 0), v.at(33, kV1) - QPoint(5, 0));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:23"));
        QCOMPARE(layout(v.f.view(), kV2), QStringLiteral("10:33"));
        QCOMPARE(v.undoCount(), undo0 + 1);
        QCOMPARE(v.c.selectedEdits().size(), 2); // still selected
        v.f.project.undo();
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:20"));
        QCOMPARE(layout(v.f.view(), kV2), QStringLiteral("10:30"));
        // A limit stops them all: the second clip has only 10 frames of media after its end.
        v.drag(v.at(30, kV1) - QPoint(5, 0), v.at(60, kV1) - QPoint(5, 0));
        QCOMPARE(v.f.view().track(kV1)->clips[0].duration, int64_t(30));
        QCOMPARE(v.f.view().track(kV2)->clips[0].duration, int64_t(40));
    }

    // Trim tool, Ripple: heads of two clips on one track.
    void trimToolRipplesSeveralEditPoints()
    {
        View v;
        v.put(kV1, mediaClip(0, 20));
        const quint64 b = v.put(kV1, mediaClip(30, 20));
        const quint64 c = v.put(kV1, mediaClip(60, 20));
        v.c.setTool(TimelineTool::Trim);
        v.c.setSelectedEdits({{0, b}, {0, c}});
        v.c.nudge(2);
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:20 30:18 58:18"));
        v.c.nudge(-2);
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:20 30:20 60:20"));
        // Dragging a total of 4 frames shortens two edits by 2 each (round robin, left to right).
        v.drag(v.at(30, kV1) + QPoint(5, 0), v.at(34, kV1) + QPoint(5, 0));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:20 30:18 58:18"));
    }

    // 104.6: keyboard nudges and typed offsets apply to every edit point; one undo step each.
    void keyboardAndTypedOffsetsTrimAll()
    {
        Ctl t;
        const quint64 a = t.put(kV1, mediaClip(10, 20));
        const quint64 b = t.put(kV2, mediaClip(10, 30));
        t.c.setSelectedEdits({{a, 0}, {b, 0}});
        const int undo0 = t.undoCount();
        t.c.nudge(3);
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:23"));
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("10:33"));
        QCOMPARE(t.undoCount(), undo0 + 1);
        t.c.nudge(-5);
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:18"));
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("10:28"));
        t.f.project.undo();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:23"));
    }

    // 104.6: E extends every selected edit point to the playhead.
    void extendEditGoesToPlayheadForEach()
    {
        Ctl t;
        const quint64 a = t.put(kV1, mediaClip(10, 20));
        const quint64 b = t.put(kV2, mediaClip(10, 30));
        t.c.setSelectedEdits({{a, 0}, {b, 0}});
        t.tl()->setPlayhead(50);
        const int undo0 = t.undoCount();
        t.action("trim.extend_edit")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:40"));
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("10:40"));
        QCOMPARE(t.undoCount(), undo0 + 1);
        t.f.project.undo();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:20"));
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("10:30"));
    }

    // U (cycle) acts on every selected edit point: both halves of each cut.
    void cycleActsOnEverySelectedEdit()
    {
        Ctl t;
        const quint64 a = t.put(kV1, mediaClip(0, 20));
        const quint64 b = t.put(kV1, mediaClip(20, 20));
        const quint64 c = t.put(kV2, mediaClip(0, 20));
        const quint64 d = t.put(kV2, mediaClip(20, 20));
        t.c.setSelectedEdits({{a, 0}, {c, 0}});
        t.c.cycleEditPoint();
        QCOMPARE(t.c.selectedEdits(), (QVector<EditPointSelection> {{a, b}, {c, d}}));
        t.c.cycleEditPoint();
        QCOMPARE(t.c.selectedEdits(), (QVector<EditPointSelection> {{0, b}, {0, d}}));
    }

    // Ripple with several edits, sync lock: the track that is not trimmed follows by the sum.
    void trimToolKeepsSyncLockedTracksInSync()
    {
        Ctl t;
        const quint64 a = t.put(kV1, mediaClip(0, 10));
        const quint64 b = t.put(kV1, mediaClip(20, 10));
        t.put(kV2, mediaClip(0, 100, 0));
        t.c.setTool(TimelineTool::Trim);
        t.c.setSelectedEdits({{a, 0}, {b, 0}});
        t.c.nudge(-2);
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:8 18:8"));
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("0:8 8:18 26:70"));
        KK_VERIFY_INVARIANTS(t.f.view());
    }

    // Paste Attributes > Speed with Ripple: a linked pair shifts the other tracks once.
    void pasteSpeedRipplesLinkedGroupOnce()
    {
        Ctl t;
        const quint64 v = t.put(kV1, mediaClip(0, 20));
        const quint64 a = t.put(kA1, mediaClip(0, 20));
        t.link({v, a});
        const quint64 vSrc = t.put(kV2, mediaClip(100, 20));
        t.put(kV2, mediaClip(150, 10));
        TrackRef a2 {TrackType::Audio, 1};
        t.put(a2, mediaClip(30, 40));
        // Copy a clip at 200 % speed, paste its speed onto the linked pair.
        t.f.project.beginEdit(QStringLiteral("speed"));
        ops::clipById(t.f.data(), vSrc)->speed = 2.0;
        t.f.project.endEdit();
        t.f.selection.setClips({vSrc});
        t.action("edit.copy")->trigger();
        t.f.selection.setClips({v, a});
        PasteAttributeSet set;
        set.speed = true;
        QVERIFY(t.c.pasteAttributes(set));
        const int64_t newDur = ops::clipById(t.f.view(), v)->duration;
        QCOMPARE(newDur, int64_t(10));
        QCOMPARE(ops::clipById(t.f.view(), a)->duration, int64_t(10));
        // The clip on the other audio track (not linked, sync-locked) moved once by -10.
        QCOMPARE(layout(t.f.view(), a2), QStringLiteral("20:40"));
        KK_VERIFY_INVARIANTS(t.f.view());
    }
};

QTEST_MAIN(TestMultiEdit)
#include "test_multiedit.moc"
