// SPDX-License-Identifier: GPL-3.0-or-later
// Project, part 6: Fusion clips (New Fusion Clip, New Fusion Composition) and the sharing of
// their Compositing page graph between every instance.
#include "core/CompOps.h"
#include "core/CompoundOps.h"
#include "core/EditOps.h"
#include "core/Project.h"

namespace kk {

namespace {

/// Sets the composition of every Nested clip of @p data that shows @p nestedUid; true if any
/// clip changed.
bool setInstances(TimelineData& data, quint64 nestedUid, const std::optional<Composition>& comp)
{
    bool changed = false;
    for (auto* tracks : {&data.videoTracks, &data.audioTracks}) {
        for (TrackData& t : *tracks) {
            for (ClipData& c : t.clips) {
                if (c.kind == ClipKind::Nested && c.nestedUid == nestedUid
                    && c.composition != comp) {
                    c.composition = comp;
                    changed = true;
                }
            }
        }
    }
    return changed;
}

} // namespace

quint64 Project::createFusionClip(
    const QVector<quint64>& clipIds, const QString& name, const QString& binId, QString* error)
{
    TimelineData parent = m_timeline->data();
    const quint64 uid = m_nextTimelineUid;
    const QString finalName = name.isEmpty() ? uniqueTimelineName(tr("Fusion Clip")) : name;
    std::optional<compound::CollapseResult> result
        = compound::collapse(parent, clipIds, uid, finalName, error);
    if (!result)
        return 0;
    ++m_nextTimelineUid;
    result->nested.fusion = true;
    result->nested.fusionComposition = compops::defaultComposition();
    const std::optional<Composition> comp = result->nested.fusionComposition;
    beginEdit(tr("New Fusion Clip"));
    m_timeline->mutableData() = std::move(parent);
    addTimeline(std::move(result->nested), binId);
    const QString itemId = timelineItemId(uid);
    for (const quint64 id : std::as_const(result->instanceIds)) {
        if (ClipData* c = editops::clipById(m_timeline->mutableData(), id)) {
            c->mediaId = itemId;
            c->composition = comp;
        }
    }
    endEdit();
    return uid;
}

quint64 Project::createFusionComposition(
    const QString& name, const QString& binId, const QStringList& mediaIds)
{
    QVector<const MediaItem*> items;
    for (const QString& id : mediaIds) {
        const MediaItem* item = m_mediaPool->item(id);
        if (item && item->kind == MediaItemKind::File && (item->info.hasVideo || item->info.hasAudio))
            items.append(item);
    }
    const QString finalName = name.isEmpty()
        ? uniqueTimelineName(items.isEmpty() ? tr("Fusion Composition") : tr("Fusion Clip"))
        : name;
    TimelineData t = makeDefaultTimeline(finalName, m_format.fps, m_format.width, m_format.height,
        std::max<int>(1, int(items.size())), std::max<int>(1, int(items.size())));
    t.uid = m_nextTimelineUid++;
    t.compound = true;
    t.fusion = true;
    t.fusionComposition = compops::defaultComposition();
    t.retimeProcess = m_format.retimeProcess;
    t.toneMap = m_format.toneMap;
    t.startTimecode = std::max<int64_t>(0, m_format.timelineStart);
    const int64_t fiveSeconds = std::max<int64_t>(1, int64_t(5 * t.fps.toDouble() + 0.5));
    if (items.isEmpty()) {
        // A transparent five second solid is the picture MediaIn shows (like Resolve's blank
        // Fusion Composition): the graph's generators and effects build on it.
        ClipData solid;
        solid.id = t.allocateId();
        solid.kind = ClipKind::Solid;
        solid.name = tr("Fusion Composition");
        solid.solidColor = QColor(0, 0, 0, 0);
        solid.duration = fiveSeconds;
        t.videoTracks.front().clips.push_back(std::move(solid));
    }
    for (qsizetype k = 0; k < items.size(); ++k) {
        const MediaItem& item = *items[k];
        ClipData c;
        c.kind = ClipKind::Media;
        c.mediaId = item.id;
        c.name = item.name;
        c.sourceDurationUs = item.info.isStillImage ? 0 : item.info.durationUs;
        c.duration = item.info.isStillImage || item.info.durationUs <= 0
            ? fiveSeconds
            : std::max<int64_t>(1, usToFrames(item.info.durationUs, t.fps));
        if (item.info.hasVideo) {
            ClipData v = c;
            v.id = t.allocateId();
            t.videoTracks[size_t(k)].clips.push_back(std::move(v));
        }
        if (item.info.hasAudio && !item.info.isStillImage) {
            ClipData a = c;
            a.id = t.allocateId();
            t.audioTracks[size_t(k)].clips.push_back(std::move(a));
        }
    }
    const quint64 uid = t.uid;
    beginEdit(tr("New Fusion Composition"));
    addTimeline(std::move(t), binId);
    endEdit();
    return uid;
}

bool Project::isFusionClip(quint64 timelineUid) const
{
    const TimelineData* t = lookup(timelineUid);
    return t && t->fusion;
}

void Project::syncFusionInstances(quint64 nestedUid, const std::optional<Composition>& comp)
{
    // Inside an open edit. The shared graph lives in the nested timeline too, so a clip that is
    // dropped later starts with it (compound::makeNestedClip()).
    for (const quint64 uid : timelineUids()) {
        const auto data = timelineData(uid);
        if (!data)
            continue;
        const bool isTarget = uid == nestedUid && data->fusion;
        bool needs = isTarget && data->fusionComposition != comp;
        if (!needs) {
            for (const auto* tracks : {&data->videoTracks, &data->audioTracks}) {
                for (const TrackData& t : *tracks) {
                    for (const ClipData& c : t.clips) {
                        if (c.kind == ClipKind::Nested && c.nestedUid == nestedUid
                            && c.composition != comp)
                            needs = true;
                    }
                }
            }
        }
        if (!needs)
            continue;
        auto copy = std::make_shared<TimelineData>(*data);
        if (isTarget)
            copy->fusionComposition = comp;
        setInstances(*copy, nestedUid, comp);
        replaceTimeline(uid, std::move(copy));
    }
}

} // namespace kk
