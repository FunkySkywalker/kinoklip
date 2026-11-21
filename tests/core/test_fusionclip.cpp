// SPDX-License-Identifier: GPL-3.0-or-later
// Fusion clips: creating one from timeline clips and from media, the graph on every instance
// (editing one updates all, also in other timelines and for later drops), rename, delete with
// and without instances in use, save / load (format 24) and undo.
#include "CoreTestUtils.h"
#include "core/CompOps.h"
#include "core/CompoundOps.h"
#include "core/EditOps.h"
#include "core/MediaUsage.h"
#include "core/NodeRegistry.h"
#include "core/Project.h"
#include "core/Serialization.h"

#include <QTemporaryDir>
#include <QTest>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

namespace {

/// A project whose active timeline holds two media clips on V1 (0:10, 10:10) and one on V2.
void fill(Project& p)
{
    p.beginEdit(QStringLiteral("Setup"));
    TimelineData s = timeline();
    s.uid = p.activeTimelineUid();
    s.name = p.timeline()->data().name;
    ops::overwriteClip(s, kV1, mediaClip(0, 10, 0, 100, QStringLiteral("a")));
    ops::overwriteClip(s, kV1, mediaClip(10, 10, 20, 100, QStringLiteral("b")));
    ops::overwriteClip(s, kV2, mediaClip(5, 10, 0, 0, QStringLiteral("top")));
    p.timeline()->mutableData() = s;
    p.endEdit();
}

QVector<quint64> allIds(const TimelineData& d)
{
    QVector<quint64> ids;
    for (const auto* tracks : {&d.videoTracks, &d.audioTracks}) {
        for (const TrackData& t : *tracks) {
            for (const ClipData& c : t.clips)
                ids.append(c.id);
        }
    }
    return ids;
}

/// Every Nested clip showing @p uid in timeline @p tl.
QVector<const ClipData*> instances(const TimelineData& tl, quint64 uid)
{
    QVector<const ClipData*> out;
    for (const TrackData& t : tl.videoTracks) {
        for (const ClipData& c : t.clips) {
            if (c.kind == ClipKind::Nested && c.nestedUid == uid)
                out.append(&c);
        }
    }
    return out;
}

/// The default graph plus a Blur-like node, so it differs from the default.
Composition editedGraph(const Composition& base)
{
    Composition comp = base;
    CompNode n;
    n.id = comp.allocateId();
    n.type = QStringLiteral("transform");
    n.name = QStringLiteral("Transform1");
    comp.nodes.append(n);
    return comp;
}

} // namespace

class TestFusionClip : public QObject
{
    Q_OBJECT
private slots:
    void createsFusionClipFromTimelineClips()
    {
        Project p;
        fill(p);
        const int undoCount = p.undoStack()->count();
        QString error;
        const quint64 uid = p.createFusionClip(allIds(p.timeline()->data()),
            QStringLiteral("Fusion A"), {}, &error);
        QVERIFY2(uid != 0, qPrintable(error));
        QCOMPARE(p.undoStack()->count(), undoCount + 1);

        // A Fusion clip is a compound timeline listed as a Fusion clip in the media pool.
        const auto nested = p.timelineData(uid);
        QVERIFY(nested->compound);
        QVERIFY(nested->fusion);
        QVERIFY(nested->fusionComposition);
        QVERIFY(p.isFusionClip(uid));
        const MediaItem* item = p.mediaPool()->item(p.timelineItemId(uid));
        QVERIFY(item);
        QCOMPARE(item->kind, MediaItemKind::FusionClip);
        QVERIFY(item->isTimeline());
        QCOMPARE(item->name, QStringLiteral("Fusion A"));
        QCOMPARE(item->info.durationUs, 20 * kUsPerFrame25);

        // The layers moved inside, the instance in the parent carries the graph:
        // MediaIn (the nested picture) -> MediaOut.
        QCOMPARE(layout(*nested, kV1), QStringLiteral("0:10 10:10"));
        QCOMPARE(layout(*nested, kV2), QStringLiteral("5:10"));
        const QVector<const ClipData*> inst = instances(p.timeline()->data(), uid);
        QCOMPARE(inst.size(), 1);
        QCOMPARE(inst[0]->mediaId, item->id);
        QVERIFY(inst[0]->composition);
        QCOMPARE(*inst[0]->composition, *nested->fusionComposition);
        const Composition& g = *inst[0]->composition;
        QVERIFY(g.node(g.mediaOutId));
        const CompNode* in = nullptr;
        for (const CompNode& n : g.nodes) {
            if (n.type == nodetypes::MediaIn)
                in = &n;
        }
        QVERIFY(in);
        QVERIFY(in->mediaId.isEmpty()); // the picture of the clip that owns the graph

        // Undo removes everything.
        p.undo();
        QVERIFY(!p.hasTimeline(uid));
        QCOMPARE(p.mediaPool()->item(p.timelineItemId(uid)), nullptr);
        QCOMPARE(layout(p.timeline()->data(), kV1), QStringLiteral("0:10 10:10"));
    }

    void editingOneInstanceUpdatesAll()
    {
        Project p;
        fill(p);
        const quint64 uid = p.createFusionClip(allIds(p.timeline()->data()), QStringLiteral("F"));
        QVERIFY(uid);
        const QString itemId = p.timelineItemId(uid);
        const quint64 parent = p.activeTimelineUid();

        // A second instance, as when dragging the media pool entry to the timeline.
        p.beginEdit(QStringLiteral("Drop"));
        ClipData second = compound::makeNestedClip(*p.timelineData(uid), itemId);
        second.start = 100;
        QVERIFY(second.composition); // a new instance starts with the shared graph
        ops::overwriteClip(p.timeline()->mutableData(), kV1, second);
        p.endEdit();
        QCOMPARE(instances(p.timeline()->data(), uid).size(), 2);

        // Edit the graph of the first instance on the Compositing page: both change, in one step.
        const ClipData* first = instances(p.timeline()->data(), uid)[0];
        const quint64 firstId = first->id;
        const Composition edited = editedGraph(*first->composition);
        const int undoCount = p.undoStack()->count();
        QVERIFY(p.setClipComposition(parent, firstId, edited, QStringLiteral("Add Node")));
        QCOMPARE(p.undoStack()->count(), undoCount + 1);
        for (const ClipData* c : instances(p.timeline()->data(), uid)) {
            QVERIFY(c->composition);
            QCOMPARE(*c->composition, edited);
        }
        QCOMPARE(*p.timelineData(uid)->fusionComposition, edited);

        // A drop made later starts with the edited graph.
        QCOMPARE(*compound::makeNestedClip(*p.timelineData(uid), itemId).composition, edited);

        // The other instance edits the shared graph as well.
        const quint64 secondId = instances(p.timeline()->data(), uid)[1]->id;
        QVERIFY(p.setClipComposition(parent, secondId, editedGraph(edited), QStringLiteral("Add Node")));
        QCOMPARE(instances(p.timeline()->data(), uid)[0]->composition->nodes.size(),
            edited.nodes.size() + 1);

        // Undo restores both instances.
        p.undo();
        for (const ClipData* c : instances(p.timeline()->data(), uid))
            QCOMPARE(*c->composition, edited);
        p.undo();
        for (const ClipData* c : instances(p.timeline()->data(), uid))
            QCOMPARE(c->composition->nodes.size(), edited.nodes.size() - 1);
    }

    void instancesInOtherTimelinesShareTheGraph()
    {
        Project p;
        fill(p);
        const quint64 parent = p.activeTimelineUid();
        const quint64 uid = p.createFusionClip(allIds(p.timeline()->data()), QStringLiteral("F"));
        const quint64 other = p.createTimeline(QStringLiteral("Other"));
        QVERIFY(other);
        // Place an instance in the other timeline, then go back to the first one.
        ClipData inst = compound::makeNestedClip(*p.timelineData(uid), p.timelineItemId(uid));
        QVERIFY(p.openTab(other));
        p.beginEdit(QStringLiteral("Place"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, inst);
        p.endEdit();
        QVERIFY(p.setActiveTimeline(parent));

        const quint64 id = instances(p.timeline()->data(), uid)[0]->id;
        const Composition edited = editedGraph(*instances(p.timeline()->data(), uid)[0]->composition);
        QVERIFY(p.setClipComposition(parent, id, edited, QStringLiteral("Add Node")));
        const auto otherData = p.timelineData(other);
        QCOMPARE(instances(*otherData, uid).size(), 1);
        QCOMPARE(*instances(*otherData, uid)[0]->composition, edited);
    }

    void resettingTheGraphResetsEveryInstance()
    {
        Project p;
        fill(p);
        const quint64 uid = p.createFusionClip(allIds(p.timeline()->data()), QStringLiteral("F"));
        const quint64 id = instances(p.timeline()->data(), uid)[0]->id;
        QVERIFY(p.setClipComposition(p.activeTimelineUid(), id, std::nullopt, QStringLiteral("Reset")));
        QVERIFY(!instances(p.timeline()->data(), uid)[0]->composition);
        QVERIFY(!p.timelineData(uid)->fusionComposition);
    }

    void compoundClipsKeepTheirOwnGraph()
    {
        Project p;
        fill(p);
        const quint64 uid = p.createCompoundClip(allIds(p.timeline()->data()), QStringLiteral("C"));
        QVERIFY(uid);
        QVERIFY(!p.isFusionClip(uid));
        QCOMPARE(p.mediaPool()->item(p.timelineItemId(uid))->kind, MediaItemKind::CompoundClip);
        QVERIFY(!instances(p.timeline()->data(), uid)[0]->composition);
    }

    void newFusionCompositionAndFromMedia()
    {
        Project p;
        const int undoCount = p.undoStack()->count();
        const quint64 blank = p.createFusionComposition();
        QVERIFY(blank);
        QCOMPARE(p.undoStack()->count(), undoCount + 1);
        QVERIFY(p.isFusionClip(blank));
        QCOMPARE(p.timelineData(blank)->duration(), int64_t(125)); // 5 s at 25 fps
        QCOMPARE(p.mediaPool()->item(p.timelineItemId(blank))->kind, MediaItemKind::FusionClip);
        QCOMPARE(p.timelineData(blank)->videoTracks[0].clips[0].kind, ClipKind::Solid);

        MediaItem a;
        a.path = QStringLiteral("/nonexistent/a.mov");
        a.name = QStringLiteral("a.mov");
        a.info.valid = a.info.hasVideo = a.info.hasAudio = true;
        a.info.frameRate = {25, 1};
        a.info.durationUs = 2'000'000;
        MediaItem b = a;
        b.path = QStringLiteral("/nonexistent/b.mov");
        b.name = QStringLiteral("b.mov");
        b.info.durationUs = 3'000'000;
        p.beginEdit(QStringLiteral("add"));
        const QString ida = p.mediaPool()->addItem(a);
        const QString idb = p.mediaPool()->addItem(b);
        p.endEdit();
        const quint64 uid = p.createFusionComposition({}, {}, {ida, idb});
        QVERIFY(uid);
        const auto n = p.timelineData(uid);
        QCOMPARE(n->name, QStringLiteral("Fusion Clip"));
        QCOMPARE(n->videoTracks.size(), size_t(2));
        QCOMPARE(layout(*n, kV1), QStringLiteral("0:50"));
        QCOMPARE(layout(*n, kV2), QStringLiteral("0:75"));
        QCOMPARE(layout(*n, kA2), QStringLiteral("0:75"));
        QCOMPARE(n->duration(), int64_t(75));
        QVERIFY(n->fusionComposition);
    }

    void renameAndDelete()
    {
        Project p;
        fill(p);
        const quint64 uid = p.createFusionClip(allIds(p.timeline()->data()), QStringLiteral("F"));
        const QString itemId = p.timelineItemId(uid);

        // Rename in the media pool renames the timeline and stays a Fusion clip.
        p.beginEdit(QStringLiteral("Rename"));
        p.mediaPool()->renameItem(itemId, QStringLiteral("Lower Third"));
        p.endEdit();
        QCOMPARE(p.timelineData(uid)->name, QStringLiteral("Lower Third"));
        QVERIFY(p.isFusionClip(uid));
        p.undo();
        QCOMPARE(p.timelineData(uid)->name, QStringLiteral("F"));

        // In use: the pool counts the instance (the panel warns before deleting).
        QCOMPARE(usage::compute(p).value(itemId, 0), 1);
        QCOMPARE(p.timelineUseCount(uid), 1);
        QVERIFY(p.deleteTimeline(uid));
        QVERIFY(!p.hasTimeline(uid));
        QCOMPARE(p.mediaPool()->item(itemId), nullptr);
        p.undo(); // everything back, graph included
        QVERIFY(p.isFusionClip(uid));
        QVERIFY(p.mediaPool()->item(itemId));
        QVERIFY(instances(p.timeline()->data(), uid)[0]->composition);

        // Not in use (instance deleted): no warning, nothing counted, deletes cleanly.
        p.beginEdit(QStringLiteral("Delete"));
        ops::liftClips(p.timeline()->mutableData(), {instances(p.timeline()->data(), uid)[0]->id});
        p.endEdit();
        QCOMPARE(p.timelineUseCount(uid), 0);
        QCOMPARE(usage::compute(p).value(itemId, 0), 0);
        QVERIFY(p.deleteTimeline(uid));
        QCOMPARE(p.mediaPool()->item(itemId), nullptr);
    }

    void saveLoadKeepsFusionClips()
    {
        QCOMPARE(serialization::kFormatVersion, 24);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.kkproj"));
        Project p;
        fill(p);
        const quint64 uid = p.createFusionClip(allIds(p.timeline()->data()), QStringLiteral("F"));
        const quint64 compoundUid = p.createCompoundClip(
            {instances(p.timeline()->data(), uid)[0]->id}, QStringLiteral("Wrapper"));
        QVERIFY(compoundUid);
        // A graph with a second node, so the round trip is not just the default.
        const quint64 blank = p.createFusionComposition(QStringLiteral("Blank"));
        const Composition edited = editedGraph(*p.timelineData(blank)->fusionComposition);
        const quint64 inner = instances(*p.timelineData(compoundUid), uid)[0]->id;
        QVERIFY(p.setClipComposition(compoundUid, inner, edited, QStringLiteral("Add Node")));
        QCOMPARE(*p.timelineData(uid)->fusionComposition, edited);
        QVERIFY(p.save(path));

        Project q;
        QString error;
        QVERIFY2(q.load(path, &error), qPrintable(error));
        QCOMPARE(q.timelineUids(), p.timelineUids());
        for (const quint64 id : p.timelineUids())
            QCOMPARE(*q.timelineData(id), *p.timelineData(id));
        QVERIFY(q.isFusionClip(uid));
        QVERIFY(q.isFusionClip(blank));
        QVERIFY(!q.isFusionClip(compoundUid));
        QCOMPARE(q.mediaPool()->state(), p.mediaPool()->state());
        QCOMPARE(q.mediaPool()->item(q.timelineItemId(uid))->kind, MediaItemKind::FusionClip);
        QCOMPARE(q.timelineData(uid)->fusionComposition, p.timelineData(uid)->fusionComposition);
    }
};

QTEST_GUILESS_MAIN(TestFusionClip)
#include "test_fusionclip.moc"
