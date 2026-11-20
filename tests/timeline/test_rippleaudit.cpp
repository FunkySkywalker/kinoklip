// SPDX-License-Identifier: GPL-3.0-or-later
// Ripple audit (roadmap 1.12, docs/behavior-spec.md 97.15 and 105): every entry point of a ripple
// delete, ripple cut and ripple trim (keys and menus = the same actions, mouse drags in the Trim
// tool and in the Cut page's Selection tool, the Cut page smart edits) is driven on the same
// timeline and must shift every unlocked sync-locked track by the same time. Tracks with Sync
// Lock off and locked tracks must stay.
//
// Timeline: V1 / A1 hold the edited clips A [0,40) B [40,80) C [80,120) and D [150,170) (video
// and audio linked); every other track holds one "sentinel" clip at 200 (V2, A2: sync-locked,
// V3: Sync Lock off, A3: locked). V2 also holds a clip [30,90) spanning the edit.
#include "ParityTestUtils.h"
#include "TimelineTestUtils.h"
#include "core/CutEdits.h"
#include "core/EditOps.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QSettings>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
using namespace kk::test::parity;
using namespace kk::timelinemetrics;
namespace ops = kk::editops;

namespace {

constexpr TrackRef kV3 {TrackType::Video, 2};
constexpr TrackRef kA2 {TrackType::Audio, 1};
constexpr TrackRef kA3 {TrackType::Audio, 2};
constexpr int64_t kSentinel = 200;

QString trackName(TrackRef ref)
{
    return QStringLiteral("%1%2").arg(ref.type == TrackType::Video ? "V" : "A").arg(ref.index + 1);
}

struct Rig : View
{
    quint64 a = 0, b = 0, c1 = 0, d = 0; ///< the video clips of V1
    quint64 av = 0, bv = 0, cv = 0, dv = 0; ///< their linked audio clips
    Rig()
    {
        f.project.beginEdit(QStringLiteral("setup"));
        ops::addTrack(f.data(), TrackType::Video);
        ops::addTrack(f.data(), TrackType::Audio);
        f.project.endEdit();
        quint64* vids[4] = {&a, &b, &c1, &d};
        quint64* auds[4] = {&av, &bv, &cv, &dv};
        const int64_t starts[4] = {0, 40, 80, 150};
        const int64_t lengths[4] = {40, 40, 40, 20};
        for (int i = 0; i < 4; ++i) {
            *vids[i] = put(kV1, mediaClip(starts[i], lengths[i], 20));
            *auds[i] = put(kA1, mediaClip(starts[i], lengths[i], 20));
            link({*vids[i], *auds[i]});
        }
        put(kV2, mediaClip(30, 60, 20));
        for (const TrackRef ref : {kV2, kV3, kA2, kA3})
            put(ref, mediaClip(kSentinel, 10, 20));
        f.project.beginEdit(QStringLiteral("setup"));
        f.data().videoTracks[2].syncLock = false;
        f.data().videoTracks[2].autoSelect
            = false; // range deletes act on Auto Select tracks (spec 97.16)
        f.data().audioTracks[2].autoSelect = false;
        f.data().audioTracks[2].locked = true;
        f.project.endEdit();
        f.project.undoStack()->clear();
        c.setLinkedSelection(true);
    }
    int64_t sentinel(TrackRef ref) const
    {
        for (const ClipData& clip : f.view().track(ref)->clips)
            if (clip.duration == 10 && clip.start >= kSentinel - 100)
                return clip.start;
        return -1;
    }
    int64_t shiftOfD() const { return ops::clipById(f.view(), d)->start - 150; }
    /// The edit shifted D (and its audio) by @p expected, every sync-locked track follows and the
    /// others stay.
    void verify(int64_t expected, const QString& what)
    {
        QString why;
        QVERIFY2(ops::checkInvariants(f.view(), &why), qPrintable(what + ": " + why));
        const ClipData* dd = ops::clipById(f.view(), d);
        const ClipData* da = ops::clipById(f.view(), dv);
        QVERIFY2(dd && da, qPrintable(what));
        QVERIFY2(dd->start - 150 == expected,
            qPrintable(QStringLiteral("%1: D moved by %2, expected %3")
                           .arg(what)
                           .arg(dd->start - 150)
                           .arg(expected)));
        QVERIFY2(da->start - 150 == expected,
            qPrintable(QStringLiteral("%1: linked audio of D moved by %2, expected %3")
                           .arg(what)
                           .arg(da->start - 150)
                           .arg(expected)));
        for (const TrackRef ref : {kV2, kA2})
            QVERIFY2(sentinel(ref) == kSentinel + expected,
                qPrintable(QStringLiteral("%1: the clip on %2 is at %3, expected %4")
                               .arg(what, trackName(ref))
                               .arg(sentinel(ref))
                               .arg(kSentinel + expected)));
        QVERIFY2(sentinel(kV3) == kSentinel, qPrintable(what + ": V3 (Sync Lock off) moved"));
        QVERIFY2(sentinel(kA3) == kSentinel, qPrintable(what + ": A3 (locked) moved"));
    }
    void select(std::initializer_list<quint64> ids) { f.selection.setClips(QVector<quint64>(ids)); }
    QPoint tailOf(quint64 id) { return edge(id, true); }
    QPoint headOf(quint64 id) { return edge(id, false); }
    QPoint edge(quint64 id, bool tail)
    {
        const QRectF rect = clipRect(id);
        return QPoint(int(tail ? rect.right() - 5 : rect.left() + 5), int(rect.center().y()));
    }
};

} // namespace

class TestRippleAudit : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_rippleaudit"); }
    void init() { QSettings().clear(); }

    // ---- delete and cut (Delete, Shift+Backspace, Ctrl+Shift+X and their menu items) ----
    void rippleDeleteKeyAndMenu()
    {
        Rig r;
        r.select({r.b});
        r.action("edit.ripple_delete")->trigger();
        r.verify(-40, QStringLiteral("Ripple Delete"));
        r.f.project.undoStack()->undo();
        r.verify(0, QStringLiteral("undo of Ripple Delete"));
    }
    void rippleCutKeyAndMenu()
    {
        Rig r;
        r.select({r.b});
        r.action("edit.ripple_cut")->trigger();
        r.verify(-40, QStringLiteral("Ripple Cut"));
    }
    void rippleDeleteSeveralClips()
    {
        Rig r;
        r.select({r.a, r.c1});
        r.action("edit.ripple_delete")->trigger();
        r.verify(-80, QStringLiteral("Ripple Delete of two clips"));
    }
    void rippleDeleteOnlyTheAudioHalf()
    {
        Rig r;
        r.c.setLinkedSelection(false);
        r.select({r.bv});
        r.action("edit.ripple_delete")->trigger();
        // The video clip is just another track: it loses the same frames.
        r.verify(-40, QStringLiteral("Ripple Delete of audio only"));
    }
    void rippleDeleteWithMarks()
    {
        Rig r;
        r.setMarks(40, 80);
        r.f.selection.clear();
        r.action("edit.ripple_delete")->trigger();
        r.verify(-40, QStringLiteral("Ripple Delete of an In/Out range"));
    }
    void rippleCutWithMarks()
    {
        Rig r;
        r.setMarks(40, 80);
        r.f.selection.clear();
        r.action("edit.ripple_cut")->trigger();
        r.verify(-40, QStringLiteral("Ripple Cut of an In/Out range"));
    }
    void rippleDeleteClipUnderPlayheadOption()
    {
        Rig r;
        QSettings().setValue(QStringLiteral("edit/rippleDeleteClipAtPlayhead"), true);
        r.c.toggleTrackFlag(kV2,
            TimelineController::FlagAutoSelect); // its own clip is not under the playhead's targets
        r.f.selection.clear();
        r.tl()->setPlayhead(50);
        r.action("edit.ripple_delete")->trigger();
        r.verify(-40, QStringLiteral("Ripple Delete under the playhead"));
    }

    // ---- keyboard trims ----
    void rippleTrimToPlayhead()
    {
        for (const bool head : {true, false}) {
            Rig r;
            r.select({r.b});
            r.tl()->setPlayhead(head ? 50 : 70);
            r.action(head ? "trim.ripple_trim_start" : "trim.ripple_trim_end")->trigger();
            r.verify(-10,
                head ? QStringLiteral("Ripple Trim Start to Playhead")
                     : QStringLiteral("Ripple Trim End to Playhead"));
        }
    }
    void nudgeEditPointsInTheTrimTool()
    {
        struct Case
        {
            bool head;
            int frames;
            int64_t shift;
        };
        for (const Case k :
            {Case {false, -3, -3}, Case {false, 4, 4}, Case {true, 3, -3}, Case {true, -2, 2}}) {
            Rig r;
            r.c.setTool(TimelineTool::Trim);
            r.c.setSelectedEdit(k.head ? EditPointSelection {0, r.b} : EditPointSelection {r.b, 0});
            r.c.nudge(k.frames);
            r.verify(k.shift,
                QStringLiteral("nudge %1 of a %2").arg(k.frames).arg(k.head ? "head" : "tail"));
        }
    }
    void nudgeKeysTriggerTheSameWay()
    {
        Rig r;
        r.c.setTool(TimelineTool::Trim);
        r.c.setSelectedEdit({r.b, 0});
        r.action("trim.nudge_left")->trigger();
        r.verify(-1, QStringLiteral("trim.nudge_left"));
        r.action("trim.nudge_right_multi")->trigger();
        r.verify(r.shiftOfD(), QStringLiteral("trim.nudge_right_multi"));
        QVERIFY(r.shiftOfD() > 0);
    }
    void extendEditToPlayhead()
    {
        Rig r;
        r.c.setTool(TimelineTool::Trim);
        r.c.setSelectedEdit({r.b, 0});
        r.tl()->setPlayhead(85);
        r.action("trim.extend_edit")->trigger();
        r.verify(5, QStringLiteral("Extend Edit (ripple in the Trim tool)"));
    }
    void severalEditPointsTogether()
    {
        Rig r;
        r.c.setTool(TimelineTool::Trim);
        r.c.setSelectedEdits({EditPointSelection {r.a, 0}, EditPointSelection {r.b, 0}});
        r.c.nudge(-2);
        r.verify(-4, QStringLiteral("two tails trimmed together"));
    }

    // ---- mouse ----
    void trimToolDragOfATail()
    {
        for (const int dx : {-30, 40}) {
            Rig r;
            r.c.setTool(TimelineTool::Trim);
            const QPoint p = r.tailOf(r.b);
            r.drag(p, p + QPoint(dx, 0));
            r.verify(dx / 10, QStringLiteral("Trim tool drag of a tail by %1").arg(dx / 10));
        }
    }
    void trimToolDragOfAHead()
    {
        Rig r;
        r.c.setTool(TimelineTool::Trim);
        const QPoint p = r.headOf(r.b);
        r.drag(p, p + QPoint(30, 0));
        r.verify(-3, QStringLiteral("Trim tool drag of a head"));
    }
    void cutPageSelectionToolEdgeDrag()
    {
        Rig r;
        TimelineViewOptions options;
        options.rippleEdgeTrims = true;
        r.w.setViewOptions(options);
        QPoint p = r.tailOf(r.b);
        r.drag(p, p - QPoint(30, 0));
        r.verify(-3, QStringLiteral("Cut page tail drag"));
        p = r.headOf(r.c1);
        r.drag(p, p + QPoint(20, 0));
        r.verify(-5, QStringLiteral("Cut page head drag"));
    }
    void selectionToolEdgeDragLeavesOtherTracksAlone()
    {
        Rig r; // no ripple: nothing moves (a gap opens on the clip's own track)
        const QPoint p = r.tailOf(r.b);
        r.drag(p, p - QPoint(30, 0));
        r.verify(0, QStringLiteral("Selection tool trim (no ripple)"));
    }
    void multiEditMouseDrag()
    {
        Rig r;
        r.c.setTool(TimelineTool::Trim);
        r.c.setSelectedEdits({EditPointSelection {r.a, 0}, EditPointSelection {r.b, 0}});
        const QPoint p = r.tailOf(r.b);
        r.drag(p, p - QPoint(20, 0));
        r.verify(-2,
            QStringLiteral(
                "mouse drag of two edit points")); // 2 frames handed out one at a time (spec 104.4)
    }

    // ---- other edits that change a length ----
    void changeClipDuration()
    {
        Rig r;
        r.select({r.b});
        QVERIFY(r.c.setSelectionDuration(30));
        r.verify(-10, QStringLiteral("Change Clip Duration"));
    }
    void changeClipSpeed()
    {
        Rig r;
        r.select({r.b});
        QVERIFY(r.c.setSelectionSpeed(2.0));
        r.verify(-20, QStringLiteral("Change Clip Speed"));
    }
    void pasteInsert()
    {
        Rig r;
        r.select({r.b});
        r.c.copy();
        r.tl()->setPlayhead(100);
        r.c.pasteInsert();
        r.verify(40, QStringLiteral("Paste Insert"));
    }

    // ---- Cut page smart edits (core functions behind the buttons) ----
    void cutPageSmartInsertAndRippleOverwrite()
    {
        Rig r;
        const ClipData clip = mediaClip(0, 12, 0);
        const QVector<ops::PlacedClip> group {{kV1, clip}, {kA1, clip}};
        TimelineData d = r.f.view();
        QVERIFY(cutedits::smartInsert(d, {group}, 41).ok());
        QCOMPARE(ops::clipById(d, r.d)->start, int64_t(162));
        QCOMPARE(d.track(kV2)->clips.back().start, kSentinel + 12);
        QCOMPARE(d.track(kA2)->clips.back().start, kSentinel + 12);
        QCOMPARE(d.track(kV3)->clips.back().start, kSentinel);
        QCOMPARE(d.track(kA3)->clips.back().start, kSentinel);
        TimelineData e = r.f.view();
        QVERIFY(cutedits::rippleOverwrite(e, {group}, 50).ok()); // B (40 frames) becomes 12
        QCOMPARE(ops::clipById(e, r.d)->start, int64_t(122));
        QCOMPARE(e.track(kV2)->clips.back().start, kSentinel - 28);
        QCOMPARE(e.track(kA2)->clips.back().start, kSentinel - 28);
        QCOMPARE(e.track(kV3)->clips.back().start, kSentinel);
        QCOMPARE(e.track(kA3)->clips.back().start, kSentinel);
    }
};

QTEST_MAIN(TestRippleAudit)
#include "test_rippleaudit.moc"
