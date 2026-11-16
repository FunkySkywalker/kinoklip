// SPDX-License-Identifier: GPL-3.0-or-later
// Owner report 1.8.1 "damaged timeline" (clip 458 on V1 effect id 314 is not unique): random
// edit sequences must never break editops::checkInvariants(); a damaged file loads repaired.
// Spec: docs/behavior-spec.md, project load (repair) and section 97 (Sync Lock).
#include "../core/CoreTestUtils.h"
#include "TimelineTestUtils.h"
#include "core/Serialization.h"
#include "ui/effects/EffectActions.h"
#include "core/RetimeOps.h"
#include "ui/timeline/TimelineController.h"

#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <functional>
#include <optional>
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

// ---- Sync property (spec 97.4, 97.11) ----
// Every frame of content is labelled (media id, source frame). After a ripple edit, each other
// unlocked sync-locked track must hold exactly its old frames moved by the edit's time map
// (frames removed by the edit are gone, the rest keep their offset to the edited content);
// locked tracks and tracks with sync lock off must not change at all.

using FrameMap = QMap<int64_t, QString>;
using TimeMap = std::function<std::optional<int64_t>(int64_t)>;

FrameMap framesOf(const TrackData& t)
{
    FrameMap m;
    for (const ClipData& c : t.clips)
        for (int64_t p = c.start; p < c.end(); ++p)
            m.insert(p, QStringLiteral("%1@%2").arg(c.mediaId).arg(c.sourceInUs / 40000 + (p - c.start)));
    return m;
}

TimeMap identityMap()
{
    return [](int64_t p) { return std::optional<int64_t>(p); };
}

/// Lengthening by @p n at @p point: later frames move right.
TimeMap lengthenMap(int64_t point, int64_t n)
{
    return [=](int64_t p) { return std::optional<int64_t>(p < point ? p : p + n); };
}

/// Shortening by @p n at @p point: frames [point - n, point) go, later ones move left.
TimeMap shortenMap(int64_t point, int64_t n)
{
    return [=](int64_t p) -> std::optional<int64_t> {
        if (p < point - n)
            return p;
        if (p < point)
            return std::nullopt;
        return p - n;
    };
}

TimeMap rangesMap(std::vector<std::pair<int64_t, int64_t>> ranges)
{
    std::sort(ranges.begin(), ranges.end());
    return [=](int64_t p) -> std::optional<int64_t> {
        int64_t shift = 0;
        int64_t end = -1; // merged coverage so far
        for (const auto& r : ranges) {
            const int64_t s = std::max(r.first, end);
            if (r.second <= s)
                continue;
            if (p >= s && p < r.second)
                return std::nullopt;
            if (r.second <= p)
                shift += r.second - s;
            end = std::max(end, r.second);
        }
        return p - shift;
    };
}

using TrackKey = std::pair<int, int>;

TrackKey keyOf(TrackRef r)
{
    return {int(r.type), r.index};
}

/// Empty string when the property holds, else what broke.
QString syncProblem(const TimelineData& before, const TimelineData& after, const TimeMap& map,
    const QSet<TrackKey>& excluded)
{
    for (TrackType type : {TrackType::Video, TrackType::Audio}) {
        const auto& oldList = before.tracks(type);
        const auto& newList = after.tracks(type);
        if (newList.size() < oldList.size())
            return QStringLiteral("tracks lost");
        for (size_t i = 0; i < oldList.size(); ++i) {
            if (excluded.contains(keyOf(TrackRef {type, int(i)})))
                continue;
            const TrackData& o = oldList[i];
            const FrameMap now = framesOf(newList[i]);
            FrameMap want;
            const bool follows = !o.locked && o.syncLock;
            const FrameMap old = framesOf(o);
            for (auto it = old.cbegin(); it != old.cend(); ++it) {
                const auto to = follows ? map(it.key()) : std::optional<int64_t>(it.key());
                if (to)
                    want.insert(*to, it.value());
            }
            if (now != want) {
                QString diff;
                for (auto it = want.cbegin(); it != want.cend(); ++it)
                    if (now.value(it.key()) != it.value()) {
                        diff = QStringLiteral(" first diff at %1: want %2 got %3").arg(it.key()).arg(it.value(), now.value(it.key()));
                        break;
                    }
                if (diff.isEmpty())
                    diff = QStringLiteral(" extra frames (%1 vs %2)").arg(now.size()).arg(want.size());
                return QStringLiteral("%1 track %2 (%3) is out of sync").arg(type == TrackType::Video ? "video" : "audio").arg(i + 1)
                    .arg(o.locked ? "locked" : o.syncLock ? "sync lock on" : "sync lock off") + diff;
            }
            if (false)
                return QStringLiteral("%1 track %2 (%3) is out of sync")
                    .arg(type == TrackType::Video ? "video" : "audio").arg(i + 1)
                    .arg(o.locked ? "locked" : o.syncLock ? "sync lock on" : "sync lock off");
        }
    }
    return {};
}

QSet<TrackKey> tracksOf(const TimelineData& d, const QVector<quint64>& ids)
{
    QSet<TrackKey> set;
    for (quint64 id : ids)
        if (const auto loc = ops::findClip(d, id))
            set.insert(keyOf(loc->track));
    return set;
}

/// Six tracks, unique media per clip, some linked V+A pairs, random sync locks, maybe a lock.
TimelineData syncTimeline(QRandomGenerator& rng, int& mediaCounter)
{
    TimelineData d = makeDefaultTimeline(QStringLiteral("Sync"), Rational {25, 1}, 1920, 1080, 3, 3);
    auto free = [](const TrackData& t, int64_t s, int64_t e) {
        for (const ClipData& c : t.clips)
            if (c.start < e && c.end() > s)
                return false;
        return true;
    };
    auto media = [&](int64_t s, int64_t len) {
        ClipData c = mediaClip(s, len, rng.bounded(5), 0);
        c.mediaId = QStringLiteral("m%1").arg(mediaCounter++);
        return c;
    };
    for (int i = 0; i < 14; ++i) {
        const int64_t s = rng.bounded(90);
        const int64_t len = 3 + rng.bounded(25);
        const int vt = rng.bounded(3);
        const int at = rng.bounded(3);
        const bool pair = rng.bounded(3) == 0;
        TrackData& v = d.videoTracks[size_t(vt)];
        TrackData& a = d.audioTracks[size_t(at)];
        if (pair && free(v, s, s + len) && free(a, s, s + len)) {
            const quint64 x = ops::overwriteClip(d, TrackRef {TrackType::Video, vt}, media(s, len));
            ClipData ac = media(s, len);
            ac.mediaId = QStringLiteral("a%1").arg(ac.mediaId);
            const quint64 y = ops::overwriteClip(d, TrackRef {TrackType::Audio, at}, ac);
            ops::linkClips(d, {x, y});
        } else if (rng.bounded(2) && free(v, s, s + len)) {
            ops::overwriteClip(d, TrackRef {TrackType::Video, vt}, media(s, len));
        } else if (free(a, s, s + len)) {
            ops::overwriteClip(d, TrackRef {TrackType::Audio, at}, media(s, len));
        }
    }
    for (TrackType type : {TrackType::Video, TrackType::Audio})
        for (TrackData& t : d.tracks(type)) {
            t.syncLock = rng.bounded(4) != 0;
            // Transitions at touching cuts, and some compound (nested) clips: every ripple
            // path must treat them like any other clip.
            for (size_t i = 0; i < t.clips.size(); ++i) {
                ClipData& c = t.clips[i];
                if (i > 0 && t.clips[i - 1].end() == c.start && c.duration > 4
                    && t.clips[i - 1].duration > 4 && rng.bounded(2) == 0) {
                    TransitionData tr;
                    tr.duration = 2 + rng.bounded(3);
                    c.transitionIn = tr;
                }
                if (rng.bounded(8) == 0) {
                    c.kind = ClipKind::Nested;
                    c.nestedUid = 4242;
                }
            }
        }
    if (rng.bounded(3) == 0)
        d.tracks(rng.bounded(2) ? TrackType::Video : TrackType::Audio)[size_t(rng.bounded(3))].locked = true;
    return d;
}

QVector<quint64> pickSome(QRandomGenerator& rng, const TimelineData& d, int most)
{
    QVector<quint64> out;
    const QVector<quint64> ids = allIds(d);
    if (ids.isEmpty())
        return out;
    for (int i = 0, n = 1 + rng.bounded(most); i < n; ++i)
        out.append(ids[rng.bounded(int(ids.size()))]);
    return out;
}

} // namespace

class TestTimelineFuzz : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        initTimelineTestCase("test_timelinefuzz");
        qputenv("KK_STRICT_EDITS", "1"); // an edit that breaks the invariants aborts the test
    }

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
    // aborts (KK_STRICT_EDITS=1, set in initTestCase) if an edit leaves the timeline invalid.
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

    // Property (owner reports 1.8.2): every ripple edit keeps the other sync-locked tracks in
    // sync at the core level; locked and sync-off tracks never change.
    void rippleEditsKeepSyncLockedTracksInSync()
    {
        QRandomGenerator rng(971);
        int media = 0;
        for (int run = 0; run < 1500; ++run) {
            const TimelineData before = syncTimeline(rng, media);
            TimelineData d = before;
            QString op;
            TimeMap map = identityMap();
            QSet<TrackKey> excluded;
            switch (rng.bounded(8)) {
            case 0: { // ripple delete of any clips (audio only, video only, mixed, linked or not)
                op = QStringLiteral("rippleDelete");
                QVector<quint64> ids = pickSome(rng, d, 3);
                if (rng.bounded(2))
                    ids = ops::withLinkedClips(d, ids);
                std::vector<std::pair<int64_t, int64_t>> ranges;
                for (quint64 id : ids) {
                    const auto loc = ops::findClip(d, id);
                    if (!loc || d.track(loc->track)->locked)
                        continue;
                    const ClipData* c = ops::clipById(d, id);
                    ranges.emplace_back(c->start, c->end());
                    excluded.insert(keyOf(loc->track));
                }
                map = rangesMap(ranges);
                ops::rippleDeleteClips(d, ids, ops::RippleScope::AllTracks);
                break;
            }
            case 1:
            case 2: { // ripple trim, head or tail, both directions
                op = QStringLiteral("rippleTrim");
                const quint64 id = pick(rng, d);
                const ClipData* c = ops::clipById(d, id);
                if (!c)
                    continue;
                const bool head = rng.bounded(2) == 0;
                QVector<quint64> group {id};
                for (quint64 l : ops::linkedClips(d, id))
                    if (l != id && (head ? ops::clipById(d, l)->start == c->start
                                         : ops::clipById(d, l)->end() == c->end()))
                        group.append(l);
                excluded = tracksOf(d, group);
                const int64_t oldStart = c->start;
                const int64_t oldEnd = c->end();
                const int64_t applied = ops::rippleTrimSynced(d, group, head, int64_t(rng.bounded(31)) - 15);
                const int64_t n = head ? -applied : applied; // change in length
                if (n > 0)
                    map = lengthenMap(head ? oldStart : oldEnd, n);
                else if (n < 0)
                    map = shortenMap(head ? oldStart - n : oldEnd, -n);
                break;
            }
            case 3: { // insert
                op = QStringLiteral("insert");
                const TrackType type = rng.bounded(2) ? TrackType::Video : TrackType::Audio;
                const TrackRef ref {type, int(rng.bounded(3))};
                ClipData c = mediaClip(rng.bounded(100), 1 + rng.bounded(20));
                c.mediaId = QStringLiteral("ins");
                const int64_t at = c.start;
                const int64_t len = c.duration;
                if (ops::insertClip(d, ref, c, ops::RippleScope::AllTracks)) {
                    excluded.insert(keyOf(ref));
                    map = lengthenMap(at, len);
                }
                break;
            }
            case 4: { // ripple overwrite, shorter and longer
                op = QStringLiteral("rippleOverwrite");
                const TrackType type = rng.bounded(2) ? TrackType::Video : TrackType::Audio;
                const TrackRef ref {type, int(rng.bounded(3))};
                const int64_t a = rng.bounded(80);
                const int64_t b = a + 1 + rng.bounded(30);
                ClipData c = mediaClip(a, 1 + rng.bounded(40));
                c.mediaId = QStringLiteral("ovr");
                const int64_t len = c.duration;
                if (!ops::rippleOverwriteClips(d, {ops::PlacedClip {ref, c}}, a, b,
                        ops::RippleScope::AllTracks).isEmpty()) {
                    excluded.insert(keyOf(ref));
                    const int64_t delta = len - (b - a);
                    if (delta > 0)
                        map = lengthenMap(b, delta);
                    else if (delta < 0)
                        map = shortenMap(b, -delta);
                }
                break;
            }
            case 6: { // insert of clips on several tracks at once (smart insert, paste insert)
                op = QStringLiteral("insertSeveral");
                const int64_t at = rng.bounded(100);
                QVector<ops::PlacedClip> clips;
                int64_t gapEnd = at;
                for (TrackType type : {TrackType::Video, TrackType::Audio}) {
                    if (rng.bounded(3) == 0)
                        continue;
                    ClipData c = mediaClip(at + rng.bounded(3), 1 + rng.bounded(25));
                    c.mediaId = QStringLiteral("ins");
                    gapEnd = std::max(gapEnd, c.end());
                    clips.append(ops::PlacedClip {TrackRef {type, int(rng.bounded(3))}, c});
                }
                const int64_t first = clips.isEmpty() ? at : clips.first().clip.start;
                int64_t insertAt = first;
                for (const auto& p : clips)
                    insertAt = std::min(insertAt, p.clip.start);
                if (!ops::insertClips(d, clips, ops::RippleScope::AllTracks).isEmpty()) {
                    for (const auto& p : clips)
                        excluded.insert(keyOf(p.track));
                    map = lengthenMap(insertAt, gapEnd - insertAt);
                }
                break;
            }
            case 7: { // Change Clip Speed with Ripple Sequence (and a clip with its partners)
                op = QStringLiteral("changeClipSpeed");
                const quint64 id = pick(rng, d);
                const ClipData* c = ops::clipById(d, id);
                if (!c)
                    continue;
                const QVector<quint64> group = ops::linkedClips(d, id);
                for (quint64 g : group) {
                    const auto loc = ops::findClip(d, g);
                    excluded.insert(keyOf(loc->track));
                }
                retimeops::SpeedChange ch;
                ch.speed = 0.5 + double(rng.bounded(30)) / 10.0;
                ch.ripple = true;
                retimeops::changeClipSpeed(d, id, ch);
                // A partner on a locked track does not change; the first one that did decides.
                for (quint64 g : group) {
                    const ClipData* o = ops::clipById(before, g);
                    const int64_t n = ops::clipById(d, g)->duration - o->duration;
                    if (n > 0)
                        map = lengthenMap(o->end(), n);
                    else if (n < 0)
                        map = shortenMap(o->end(), -n);
                    if (n != 0)
                        break;
                }
                break;
            }
            default: { // ripple delete of a range: unlocked tracks (sync lock off is auto select's call)
                op = QStringLiteral("extractRange");
                const int64_t a = rng.bounded(80);
                const int64_t b = a + 1 + rng.bounded(30);
                for (TrackType type : {TrackType::Video, TrackType::Audio})
                    for (size_t i = 0; i < before.tracks(type).size(); ++i)
                        if (!before.tracks(type)[i].syncLock)
                            excluded.insert(keyOf(TrackRef {type, int(i)}));
                map = rangesMap({{a, b}});
                ops::extractRange(d, a, b);
                break;
            }
            }
            QString why;
            QVERIFY2(ops::checkInvariants(d, &why), qPrintable(op + QStringLiteral(": ") + why));
            const QString problem = syncProblem(before, d, map, excluded);
            QString dump;
            if (!problem.isEmpty())
                for (TrackType type : {TrackType::Video, TrackType::Audio})
                    for (size_t i = 0; i < before.tracks(type).size(); ++i)
                        dump += QStringLiteral("\n  %1%2 lock=%3 sync=%4: %5 -> %6")
                                    .arg(type == TrackType::Video ? "V" : "A").arg(i + 1)
                                    .arg(before.tracks(type)[i].locked).arg(before.tracks(type)[i].syncLock)
                                    .arg(layout(before, TrackRef {type, int(i)}), layout(d, TrackRef {type, int(i)}));
            QVERIFY2(problem.isEmpty(),
                qPrintable(QStringLiteral("run %1 %2: %3%4").arg(run).arg(op, problem, dump)));
        }
    }

    // Property (spec 104): several edit points ripple-trimmed together keep every other
    // sync-locked track in sync, each edit shifting it once (by the sum of the trims at or
    // before each frame); tracks holding a trimmed clip are left to their own trims.
    void multiEditRippleKeepsSyncLockedTracksInSync()
    {
        QRandomGenerator rng(104);
        int media = 0;
        int trimmed = 0;
        for (int run = 0; run < 1500; ++run) {
            const TimelineData before = syncTimeline(rng, media);
            TimelineData d = before;
            // 2-4 edit points: a random clip's head or tail, its linked same-edge partners
            // trimming with it; no edge twice.
            ops::MultiTrim multi;
            QSet<std::pair<quint64, bool>> used;
            QVector<quint64> all;
            struct Edit
            {
                QVector<quint64> ids;
                bool head;
            };
            std::vector<Edit> edits;
            for (int i = 0, n = 2 + rng.bounded(3); i < n; ++i) {
                const quint64 id = pick(rng, before);
                const ClipData* c = ops::clipById(before, id);
                if (!c)
                    continue;
                const bool head = rng.bounded(2) == 0;
                if (used.contains({id, head}) || used.contains({id, !head}))
                    continue; // (both edges of one clip: its length change would be ambiguous)
                ops::TrimGroup g;
                g.edges.append({id, head});
                used.insert({id, head});
                for (quint64 l : ops::linkedClips(before, id)) {
                    const ClipData* o = ops::clipById(before, l);
                    if (l != id && o && (head ? o->start == c->start : o->end() == c->end())
                        && !used.contains({l, head}) && !used.contains({l, !head})) {
                        g.edges.append({l, head});
                        used.insert({l, head});
                    }
                }
                for (const ops::TrimEdge& e : g.edges)
                    all.append(e.clipId);
                multi.trims.append(g);
                Edit e {{}, head};
                for (const ops::TrimEdge& te : g.edges)
                    e.ids.append(te.clipId);
                edits.push_back(e);
            }
            if (multi.trims.size() < 2)
                continue;
            const int64_t delta = int64_t(rng.bounded(31)) - 15;
            const bool roundRobin = rng.bounded(3) == 0;
            const int64_t got = ops::multiTrimTogether(d, multi, delta, true,
                roundRobin ? ops::TrimShare::RoundRobin : ops::TrimShare::SameAmount);
            QString why;
            QVERIFY2(ops::checkInvariants(d, &why), qPrintable(why));
            if (got != 0)
                ++trimmed;
            // The time map of every edit that changed its clip, applied right to left.
            std::vector<std::pair<int64_t, TimeMap>> maps;
            QString mapDump;
            for (const Edit& e : edits) {
                // A partner on a locked track does not trim; the first that did decides.
                for (quint64 id : e.ids) {
                    const ClipData* old = ops::clipById(before, id);
                    const int64_t n = ops::clipById(d, id)->duration - old->duration;
                    if (n == 0)
                        continue;
                    const int64_t at = e.head ? (n < 0 ? old->start - n : old->start) : old->end();
                    maps.emplace_back(at, n > 0 ? lengthenMap(at, n) : shortenMap(at, -n));
                    mapDump += QStringLiteral(" [at %1 n %2]").arg(at).arg(n);
                    break;
                }
            }
            std::stable_sort(maps.begin(), maps.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
            const TimeMap map = [maps](int64_t p) -> std::optional<int64_t> {
                std::optional<int64_t> q = p;
                for (const auto& m : maps) {
                    if (!q)
                        break;
                    q = m.second(*q);
                }
                return q;
            };
            const QString problem = syncProblem(before, d, map, tracksOf(before, all));
            QString dump;
            if (!problem.isEmpty()) {
                for (const auto& g : multi.trims) {
                    dump += QStringLiteral("\n  edit");
                    for (const auto& e : g.edges)
                        dump += QStringLiteral(" %1%2").arg(e.head ? "head " : "tail ").arg(e.clipId);
                }
                dump += QStringLiteral("\n  delta %1 got %2 rr=%3 maps%4").arg(delta).arg(got).arg(roundRobin).arg(mapDump);
                for (TrackType type : {TrackType::Video, TrackType::Audio})
                    for (size_t i = 0; i < before.tracks(type).size(); ++i)
                        dump += QStringLiteral("\n  %1%2 lock=%3 sync=%4: %5 -> %6")
                                    .arg(type == TrackType::Video ? "V" : "A").arg(i + 1)
                                    .arg(before.tracks(type)[i].locked).arg(before.tracks(type)[i].syncLock)
                                    .arg(layout(before, TrackRef {type, int(i)}), layout(d, TrackRef {type, int(i)}));
            }
            QVERIFY2(problem.isEmpty(), qPrintable(QStringLiteral("run %1: %2%3").arg(run).arg(problem, dump)));
        }
        QVERIFY(trimmed > 200); // the property was exercised, not vacuous
    }

    // The same through the controller commands the UI calls (one selected clip).
    void controllerRippleCommandsKeepSyncLockedTracksInSync()
    {
        QRandomGenerator rng(972);
        int media = 0;
        for (int run = 0; run < 1200; ++run) {
            TimelineFixture f;
            TimelineController c(f.ctx);
            f.project.beginEdit(QStringLiteral("setup"));
            f.data() = syncTimeline(rng, media);
            f.project.endEdit();
            f.project.undoStack()->clear();
            const TimelineData before = f.view();
            const quint64 id = pick(rng, before);
            const ClipData* p = ops::clipById(before, id);
            if (!p)
                continue;
            const int which0 = rng.bounded(11);
            QVector<quint64> picked {id};
            if (const int more = rng.bounded(3); which0 < 2 && more > 0)
                picked += pickSome(rng, before, more); // several clips, audio only or not
            // Linked Selection off: only the clicked clips are edited, their partners follow as
            // any other track (the group of one).
            const bool linked = rng.bounded(4) != 0;
            c.setLinkedSelection(linked);
            const QVector<quint64> group = linked ? ops::withLinkedClips(before, picked) : picked;
            const QSet<TrackKey> excluded = tracksOf(before, group);
            const int64_t oldStart = p->start;
            const int64_t oldEnd = p->end();
            const int64_t oldDur = p->duration;
            f.selection.setClips(picked);
            f.project.timeline()->setPlayhead(oldStart + 1 + rng.bounded(int(oldDur)));
            const int which = which0;
            const bool head = which == 3 || which == 7;
            TimeMap map = identityMap();
            int64_t pastedAt = -1;
            int64_t pastedLength = 0;
            switch (which) {
            case 0:
            case 1: {
                std::vector<std::pair<int64_t, int64_t>> ranges;
                for (quint64 g : group) {
                    const auto loc = ops::findClip(before, g);
                    if (loc && !before.track(loc->track)->locked)
                        ranges.emplace_back(ops::clipById(before, g)->start, ops::clipById(before, g)->end());
                }
                map = rangesMap(ranges);
                which == 0 ? c.rippleDeleteSelection() : c.rippleCut();
                break;
            }
            case 2:
                c.rippleTrimEndToPlayhead();
                break;
            case 3:
                c.rippleTrimStartToPlayhead();
                break;
            case 4:
            case 5:
                c.setSelectionDuration(1 + rng.bounded(40));
                break;
            case 6:
            case 7: { // , . [ ] with an edit point selected in the Trim tool (ripple trim)
                c.setTool(TimelineTool::Trim);
                c.setSelectedEdit(head ? EditPointSelection {0, id} : EditPointSelection {id, 0});
                c.nudge(int64_t(rng.bounded(21)) - 10);
                break;
            }
            case 8:
                c.setSelectionSpeed(0.5 + double(rng.bounded(30)) / 10.0);
                break;
            case 9: { // Ctrl+C, Ctrl+Shift+V: paste insert at the playhead
                f.selection.setClips(picked);
                c.copy();
                const int64_t at = rng.bounded(100);
                f.project.timeline()->setPlayhead(at);
                const int64_t length = c.clipboard().duration;
                c.pasteInsert();
                if (f.project.undoStack()->count() > 0) {
                    pastedAt = at;
                    pastedLength = length;
                }
                break;
            }
            default: { // Alt+drag-free ripple delete of the clips under the playhead (no selection)
                f.selection.clear();
                f.project.timeline()->setPlayhead(oldStart + 1);
                c.rippleCut();
                break;
            }
            }
            const TimelineData& after = f.view();
            QString why;
            QVERIFY2(ops::checkInvariants(after, &why), qPrintable(why));
            if (which == 10) { // owner: with nothing selected Ripple Cut must not jump the playhead
                QCOMPARE(f.project.timeline()->playhead(), oldStart + 1);
                QCOMPARE(f.project.undoStack()->count(), 0);
            }
            if (which == 9 && pastedAt >= 0)
                map = lengthenMap(pastedAt, pastedLength);
            if (which >= 2 && which != 9 && which != 10) {
                // The selected clip may sit on a locked track while its linked partner is trimmed.
                for (quint64 g : group) {
                    const ClipData* o = ops::clipById(before, g);
                    const ClipData* q = ops::clipById(after, g);
                    QVERIFY(o && q);
                    const int64_t n = q->duration - o->duration;
                    if (n > 0)
                        map = lengthenMap(head ? o->start : o->end(), n);
                    else if (n < 0)
                        map = shortenMap(head ? o->start - n : o->end(), -n);
                    if (n != 0)
                        break;
                }
            }
            const QString problem = syncProblem(before, after, map, excluded);
            QString dump;
            if (!problem.isEmpty()) {
                for (TrackType type : {TrackType::Video, TrackType::Audio})
                    for (size_t i = 0; i < before.tracks(type).size(); ++i)
                        dump += QStringLiteral("\n  %1%2 lock=%3 sync=%4: %5 -> %6")
                                    .arg(type == TrackType::Video ? "V" : "A").arg(i + 1)
                                    .arg(before.tracks(type)[i].locked).arg(before.tracks(type)[i].syncLock)
                                    .arg(layout(before, TrackRef {type, int(i)}), layout(after, TrackRef {type, int(i)}));
                for (const ClipData& c : before.videoTracks[1].clips)
                    dump += QStringLiteral("\n  B %1 %2:%3 in=%4 spd=%5 id=%6").arg(c.mediaId).arg(c.start).arg(c.duration).arg(c.sourceInUs).arg(c.speed).arg(c.id);
                for (const ClipData& c : after.videoTracks[1].clips)
                    dump += QStringLiteral("\n  A %1 %2:%3 in=%4 spd=%5 id=%6").arg(c.mediaId).arg(c.start).arg(c.duration).arg(c.sourceInUs).arg(c.speed).arg(c.id);
                dump += QStringLiteral("\n  selected clip %1 at %2:%3, playhead %4")
                            .arg(id).arg(oldStart).arg(oldDur).arg(f.project.timeline()->playhead());
            }
            QVERIFY2(problem.isEmpty(),
                qPrintable(QStringLiteral("run %1 command %2: %3%4").arg(run).arg(which).arg(problem, dump)));
        }
    }
};

QTEST_MAIN(TestTimelineFuzz)
#include "test_timelinefuzz.moc"
