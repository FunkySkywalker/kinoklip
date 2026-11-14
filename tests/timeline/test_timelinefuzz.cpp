// SPDX-License-Identifier: GPL-3.0-or-later
// Owner report 1.8.1 "damaged timeline" (clip 458 on V1 effect id 314 is not unique): random
// edit sequences must never break editops::checkInvariants(); a damaged file loads repaired.
// Spec: docs/behavior-spec.md, project load (repair) and section 97 (Sync Lock).
#include "TimelineTestUtils.h"
#include "core/Serialization.h"
#include "ui/effects/EffectActions.h"
#include "ui/timeline/TimelineController.h"

#include <QJsonArray>
#include <QRandomGenerator>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

namespace {

ClipData withEffects(ClipData c, TimelineData& d, int effects, int masks)
{
    for (int i = 0; i < effects; ++i)
        c.effects.append(EffectRegistry::instance().createEffect(
            QStringLiteral("gaussian_blur"), d.allocateId()));
    for (int i = 0; i < masks; ++i) {
        MaskData m;
        m.id = d.allocateId();
        c.masks.append(m);
    }
    return c;
}

/// A random multi-track timeline: linked V+A pairs, lone clips, effects, masks, transitions,
/// sync lock on/off, a compound (nested) clip.
TimelineData randomTimeline(QRandomGenerator& rng)
{
    TimelineData d = makeDefaultTimeline(
        QStringLiteral("Fuzz"), Rational {25, 1}, 1920, 1080, 2 + rng.bounded(2), 2 + rng.bounded(2));
    for (TrackType type : {TrackType::Video, TrackType::Audio})
        for (TrackData& t : d.tracks(type))
            t.syncLock = rng.bounded(4) != 0;
    int64_t at = rng.bounded(5);
    for (int i = 0; i < 6; ++i) {
        const int64_t len = 5 + rng.bounded(30);
        ClipData v = withEffects(mediaClip(at, len), d, rng.bounded(3), rng.bounded(2));
        const int vt = rng.bounded(int(d.videoTracks.size()));
        const quint64 vid = ops::overwriteClip(d, TrackRef {TrackType::Video, vt}, v);
        if (rng.bounded(3) != 0) {
            ClipData a = withEffects(mediaClip(at, len), d, rng.bounded(2), 0);
            const int at2 = rng.bounded(int(d.audioTracks.size()));
            const quint64 aid = ops::overwriteClip(d, TrackRef {TrackType::Audio, at2}, a);
            if (vid && aid)
                ops::linkClips(d, {vid, aid});
        }
        if (rng.bounded(3) == 0 && vid)
            ops::placeTransition(d, vid, rng.bounded(2) == 0,
                EffectRegistry::instance().createTransition(
                    QStringLiteral("cross_dissolve"), 1 + rng.bounded(6)));
        at += len + (rng.bounded(3) == 0 ? rng.bounded(8) : 0);
        if (rng.bounded(4) == 0)
            at = rng.bounded(40);
    }
    if (rng.bounded(3) == 0) { // a compound clip (nested timeline reference)
        ClipData comp = mediaClip(10 + rng.bounded(30), 30 + rng.bounded(60));
        comp.kind = ClipKind::Nested;
        comp.mediaId.clear();
        comp.nestedUid = 99;
        ops::overwriteClip(d, TrackRef {TrackType::Video, int(d.videoTracks.size()) - 1}, comp);
    }
    return d;
}

QVector<quint64> allIds(const TimelineData& d)
{
    QVector<quint64> ids;
    for (TrackType type : {TrackType::Video, TrackType::Audio})
        for (const TrackData& t : d.tracks(type))
            for (const ClipData& c : t.clips)
                ids.append(c.id);
    return ids;
}

quint64 pick(QRandomGenerator& rng, const TimelineData& d)
{
    const QVector<quint64> ids = allIds(d);
    return ids.isEmpty() ? 0 : ids[rng.bounded(int(ids.size()))];
}

/// One random core operation; returns its name.
QString randomOp(QRandomGenerator& rng, TimelineData& d)
{
    const int64_t delta = int64_t(rng.bounded(41)) - 20;
    const bool head = rng.bounded(2) == 0;
    const quint64 id = pick(rng, d);
    switch (rng.bounded(22)) {
    case 0:
    case 1: {
        QVector<quint64> group {id};
        for (quint64 l : ops::linkedClips(d, id))
            if (l != id && rng.bounded(4) != 0)
                group.append(l);
        ops::rippleTrimSynced(d, group, head, delta);
        return QStringLiteral("rippleTrimSynced");
    }
    case 2:
        ops::splitAt(d, rng.bounded(120));
        return QStringLiteral("splitAt");
    case 3:
        ops::splitClips(d, {id}, rng.bounded(120));
        return QStringLiteral("splitClips");
    case 4:
        head ? ops::trimHead(d, id, delta, rng.bounded(2)) : ops::trimTail(d, id, delta, rng.bounded(2));
        return QStringLiteral("trim");
    case 5: {
        const ops::RippleScope scope
            = rng.bounded(2) ? ops::RippleScope::AllTracks : ops::RippleScope::TargetTracks;
        ops::rippleDeleteClips(d, ops::withLinkedClips(d, {id}), scope);
        return QStringLiteral("rippleDelete");
    }
    case 6:
        ops::liftClips(d, {id});
        return QStringLiteral("lift");
    case 7:
        ops::moveClips(d, {id}, delta, int(rng.bounded(3)) - 1);
        return QStringLiteral("move");
    case 8: {
        ClipData c = withEffects(mediaClip(rng.bounded(100), 1 + rng.bounded(30)), d, 1, 1);
        const TrackType type = rng.bounded(2) ? TrackType::Video : TrackType::Audio;
        const TrackRef ref {type, int(rng.bounded(int(d.tracks(type).size())))};
        if (rng.bounded(2))
            ops::overwriteClip(d, ref, c);
        else
            ops::insertClip(d, ref, c, ops::RippleScope::AllTracks);
        return QStringLiteral("overwrite/insert");
    }
    case 9: {
        const ops::Clipboard cb = ops::copyClips(d, ops::withLinkedClips(d, {id}));
        ops::pasteClips(d, cb, rng.bounded(100), -1, -1, rng.bounded(2));
        return QStringLiteral("paste");
    }
    case 10:
        if (const quint64 n = ops::cutNeighbour(d, id, false))
            ops::rollEdit(d, id, n, delta);
        return QStringLiteral("roll");
    case 11:
        ops::slipClip(d, id, delta);
        return QStringLiteral("slip");
    case 12:
        ops::slideClip(d, id, delta);
        return QStringLiteral("slide");
    case 13:
        if (const quint64 n = ops::cutNeighbour(d, id, false); n && ops::canJoin(d, id, n))
            ops::joinClips(d, id, n);
        return QStringLiteral("join");
    case 14:
        ops::swapWithNeighbour(d, id, rng.bounded(2) ? 1 : -1);
        return QStringLiteral("swap");
    case 15:
        ops::deleteGaps(d);
        return QStringLiteral("deleteGaps");
    case 16:
        ops::setTransitionDuration(d, id, head, 1 + rng.bounded(30));
        return QStringLiteral("transitionDuration");
    case 17:
        ops::placeTransition(d, id, head,
            EffectRegistry::instance().createTransition(
                QStringLiteral("cross_dissolve"), 1 + rng.bounded(20)));
        return QStringLiteral("placeTransition");
    case 18: {
        const int64_t a = rng.bounded(80);
        ops::extractRange(d, a, a + 1 + rng.bounded(30));
        return QStringLiteral("extractRange");
    }
    case 19: {
        const int64_t a = rng.bounded(80);
        ops::liftRange(d, a, a + 1 + rng.bounded(30));
        return QStringLiteral("liftRange");
    }
    case 20:
        if (rng.bounded(2))
            ops::addTrack(d, rng.bounded(2) ? TrackType::Video : TrackType::Audio);
        return QStringLiteral("addTrack");
    default: {
        const ops::Clipboard cb = ops::copyClips(d, {id});
        ops::pasteClips(d, cb, rng.bounded(100));
        return QStringLiteral("copyPaste");
    }
    }
}

} // namespace

class TestTimelineFuzz : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_timelinefuzz"); }

    // The owner's file: cutting a clip that carries an effect copied the effect id.
    void splitGivesTheRightHalfItsOwnEffectIds()
    {
        TimelineData d = timeline();
        ClipData c = withEffects(mediaClip(0, 40), d, 2, 1);
        ops::overwriteClip(d, kV1, c);
        ops::splitAt(d, 20);
        QCOMPARE(d.videoTracks[0].clips.size(), size_t(2));
        KK_VERIFY_INVARIANTS(d);
        // clearRange through the middle keeps a left and a right piece
        TimelineData e = timeline();
        ops::overwriteClip(e, kV1, withEffects(mediaClip(0, 40), e, 2, 1));
        ops::clearRange(e, kV1, 10, 20);
        QCOMPARE(e.videoTracks[0].clips.size(), size_t(2));
        KK_VERIFY_INVARIANTS(e);
    }

    // Effect applied to 4 selected clips: fresh ids, still valid after save and load.
    void effectOnSeveralClipsRoundTrips()
    {
        TimelineFixture f;
        QVector<quint64> ids;
        f.project.beginEdit(QStringLiteral("setup"));
        for (int i = 0; i < 4; ++i)
            ids.append(ops::overwriteClip(f.data(), kV1, mediaClip(i * 20, 20)));
        f.project.endEdit();
        QCOMPARE(effectactions::applyEffect(f.project, ids, QStringLiteral("gaussian_blur")), 4);
        KK_VERIFY_INVARIANTS(f.view());
        QSet<quint64> fx;
        for (const ClipData& c : f.view().videoTracks[0].clips)
            for (const EffectInstance& e : c.effects)
                fx.insert(e.id);
        QCOMPARE(fx.size(), 4);
        TimelineData back;
        QStringList repairs;
        QString error;
        QVERIFY2(serialization::timelineFromJson(
                     serialization::timelineToJson(f.view()), back, &error, &repairs),
            qPrintable(error));
        QVERIFY(repairs.isEmpty());
        QVERIFY(back == f.view());
    }

    // The owner's damaged file: the same effect id on two clips (and more) loads, repaired.
    void damagedTimelineLoadsRepaired()
    {
        TimelineData d = timeline();
        d.name = QStringLiteral("Damaged");
        ClipData a = withEffects(mediaClip(0, 20), d, 1, 1);
        ClipData b = a; // same clip id, effect id and mask id
        b.start = 30;
        d.videoTracks[0].clips = {a, b};
        ClipData over = mediaClip(5, 10); // overlaps a
        over.id = d.allocateId();
        d.videoTracks[0].clips.insert(d.videoTracks[0].clips.begin() + 1, over);
        ClipData none = mediaClip(60, 0); // no length
        none.id = d.allocateId();
        d.videoTracks[0].clips.push_back(none);
        ClipData neg = mediaClip(-5, 10, 3); // starts before 0
        neg.id = d.allocateId();
        d.audioTracks[0].clips.push_back(neg);
        d.nextId = 2; // too low
        QVERIFY(!ops::checkInvariants(d));

        TimelineData back;
        QStringList repairs;
        QString error;
        QVERIFY2(serialization::timelineFromJson(
                     serialization::timelineToJson(d), back, &error, &repairs),
            qPrintable(error));
        QVERIFY(!repairs.isEmpty());
        KK_VERIFY_INVARIANTS(back);
        // Nothing with length was lost: both effect clips, the overlapping one (moved to a
        // "Repaired" track at the same time) and the head-trimmed audio clip.
        int clips = 0;
        for (const TrackData& t : back.videoTracks)
            clips += int(t.clips.size());
        QCOMPARE(clips, 3);
        QCOMPARE(back.videoTracks.back().name, QStringLiteral("Repaired 1"));
        QCOMPARE(back.videoTracks.back().clips[0].start, int64_t(5));
        QCOMPARE(back.audioTracks[0].clips[0].start, int64_t(0));
        QCOMPARE(back.audioTracks[0].clips[0].duration, int64_t(5));
        // And it can be edited and saved again.
        ops::splitAt(back, 10);
        KK_VERIFY_INVARIANTS(back);
    }

    // Change Clip Duration on a selected linked pair ripples the sync-locked tracks once.
    void changeDurationOfLinkedPairRipplesOnce()
    {
        for (const int64_t target : {int64_t(30), int64_t(5)}) {
            TimelineFixture f;
            TimelineController c(f.ctx);
            f.project.beginEdit(QStringLiteral("setup"));
            TimelineData& d = f.data();
            const quint64 v = ops::overwriteClip(d, kV1, mediaClip(0, 10));
            const quint64 a = ops::overwriteClip(d, kA1, mediaClip(0, 10));
            ops::linkClips(d, {v, a});
            ops::overwriteClip(d, kV2, mediaClip(20, 10));
            ops::overwriteClip(d, kA2, mediaClip(20, 10));
            f.project.endEdit();
            f.selection.setClips({v, a});
            QVERIFY(c.setSelectionDuration(target));
            const int64_t shift = target - 10;
            QCOMPARE(f.view().videoTracks[1].clips[0].start, 20 + shift);
            QCOMPARE(f.view().audioTracks[1].clips[0].start, 20 + shift);
            QCOMPARE(f.view().videoTracks[0].clips[0].duration, target);
            QCOMPARE(f.view().audioTracks[0].clips[0].duration, target);
            KK_VERIFY_INVARIANTS(f.view());
        }
    }

    // Random timelines x random operation sequences: the invariants hold after every step.
    void randomCoreEditsKeepInvariants()
    {
        QRandomGenerator rng(20261114);
        for (int run = 0; run < 400; ++run) {
            TimelineData d = randomTimeline(rng);
            QVERIFY2(ops::checkInvariants(d), "random start timeline");
            QStringList trace;
            for (int step = 0; step < 25; ++step) {
                const QString op = randomOp(rng, d);
                trace << op;
                QString why;
                if (!ops::checkInvariants(d, &why))
                    QFAIL(qPrintable(QStringLiteral("run %1 step %2 after %3: %4 (%5)")
                                         .arg(run).arg(step).arg(op, why, trace.join(QLatin1Char(',')))));
            }
        }
    }

    // The same through the controller: every command is one undo step; Project::endEdit()
    // aborts (KK_STRICT_EDITS=1, set by CTest) if an edit leaves the timeline invalid.
    void randomControllerEditsKeepInvariants()
    {
        QRandomGenerator rng(1114);
        for (int run = 0; run < 60; ++run) {
            TimelineFixture f;
            TimelineController c(f.ctx);
            f.project.beginEdit(QStringLiteral("setup"));
            f.data() = randomTimeline(rng);
            f.data().nextId += 1000;
            f.project.endEdit();
            f.project.undoStack()->clear();
            for (int step = 0; step < 15; ++step) {
                const QVector<quint64> ids = allIds(f.view());
                if (ids.isEmpty())
                    break;
                QVector<quint64> sel {ids[rng.bounded(int(ids.size()))]};
                if (rng.bounded(2))
                    sel.append(ids[rng.bounded(int(ids.size()))]);
                f.selection.setClips(sel);
                f.project.timeline()->setPlayhead(rng.bounded(100));
                switch (rng.bounded(7)) {
                case 0:
                    c.setSelectionDuration(1 + rng.bounded(40));
                    break;
                case 1:
                    c.rippleTrimStartToPlayhead();
                    break;
                case 2:
                    c.rippleTrimEndToPlayhead();
                    break;
                case 3:
                    c.trimStartToPlayhead();
                    break;
                case 4:
                    c.trimEndToPlayhead();
                    break;
                case 5:
                    effectactions::applyEffect(
                        f.project, effectactions::effectTargets(f.project, sel, sel.first()),
                        QStringLiteral("gaussian_blur"));
                    break;
                default:
                    c.setSelectionSpeed(0.5 + rng.bounded(30) / 10.0);
                    break;
                }
                QString why;
                if (!ops::checkInvariants(f.view(), &why))
                    QFAIL(qPrintable(QStringLiteral("run %1 step %2: %3").arg(run).arg(step).arg(why)));
                if (rng.bounded(4) == 0) { // undo / redo keep it valid too
                    f.project.undo();
                    KK_VERIFY_INVARIANTS(f.view());
                    f.project.redo();
                    KK_VERIFY_INVARIANTS(f.view());
                }
            }
        }
    }
};

QTEST_MAIN(TestTimelineFuzz)
#include "test_timelinefuzz.moc"
