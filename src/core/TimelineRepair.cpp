// SPDX-License-Identifier: GPL-3.0-or-later
// Load-time repair of a timeline that breaks the structural invariants (EditOps.h).
//
// Policy (spec "Project load", owner report 1.8.1 damaged timeline): a project must always
// open. Nothing the user placed is thrown away unless it has no length left:
//  * a clip with duration < 1 is dropped (it has no frames);
//  * a clip that starts before 0 loses the part before 0 (head trimmed);
//  * a clip that overlaps the previous one, or sits on the wrong kind of track, moves to a new
//    track of the right kind named "Repaired N", at the same time (media is kept, nothing
//    shifts), with its transitions cleared;
//  * duplicate or zero ids (tracks, clips, effects, masks) are renumbered, nextId is raised
//    above every id and link group.
#include "core/EditOps.h"

#include "core/EditOpsInternal.h"
#include "core/Retime.h"

#include <QCoreApplication>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace kk::editops {

using namespace detail;

namespace {

struct Displaced
{
    ClipData clip;
    TrackType type = TrackType::Video;
};

QString say(const char* text, int n = -1)
{
    return QCoreApplication::translate("TimelineRepair", text, nullptr, n);
}

} // namespace

bool repairTimeline(TimelineData& data, QStringList* log)
{
    QStringList notes;

    if (!data.fps.isValid()) {
        data.fps = Rational {25, 1};
        notes.append(say("The frame rate was invalid and was reset to 25 fps."));
    }
    if (data.width <= 0 || data.height <= 0) {
        data.width = 1920;
        data.height = 1080;
        notes.append(say("The resolution was invalid and was reset to 1920x1080."));
    }
    normalizeIds(data);

    std::vector<Displaced> displaced;
    int dropped = 0;
    int trimmedHeads = 0;
    int moved = 0;
    int fixedValues = 0;

    for (TrackType type : kTrackTypes) {
        auto& list = data.tracks(type);
        for (TrackData& track : list) {
            if (track.type != type) {
                track.type = type;
                ++fixedValues;
            }
            // 1. Per-clip values.
            std::vector<ClipData> kept;
            for (ClipData& c : track.clips) {
                if (c.duration < 1) {
                    ++dropped;
                    continue;
                }
                if (!std::isfinite(c.speed)) {
                    c.speed = 1.0;
                    ++fixedValues;
                }
                if (c.sourceInUs < 0) {
                    c.sourceInUs = 0;
                    ++fixedValues;
                }
                if (c.start < 0) {
                    const int64_t cut = -c.start;
                    if (cut >= c.duration) {
                        ++dropped;
                        continue;
                    }
                    if (c.speed > 0.0 && !retime::hasCurve(c))
                        c.sourceInUs += c.framesToSourceUs(cut, data.fps);
                    c.duration -= cut;
                    c.start = 0;
                    clampFades(c, true);
                    ++trimmedHeads;
                }
                kept.push_back(std::move(c));
            }
            // 2. Wrong kind of track: the clip goes to a track of its own kind.
            std::vector<ClipData> fit;
            for (ClipData& c : kept) {
                const bool subtitleClip = c.kind == ClipKind::Subtitle;
                if (subtitleClip != (type == TrackType::Subtitle)) {
                    c.transitionIn.reset();
                    c.transitionOut.reset();
                    displaced.push_back(
                        {std::move(c), subtitleClip ? TrackType::Subtitle : TrackType::Video});
                    ++moved;
                } else {
                    fit.push_back(std::move(c));
                }
            }
            // 3. Order and overlaps.
            std::stable_sort(fit.begin(), fit.end(),
                [](const ClipData& a, const ClipData& b) { return a.start < b.start; });
            track.clips.clear();
            for (ClipData& c : fit) {
                if (!track.clips.empty() && track.clips.back().end() > c.start) {
                    c.transitionIn.reset();
                    c.transitionOut.reset();
                    displaced.push_back({std::move(c), type});
                    ++moved;
                } else {
                    track.clips.push_back(std::move(c));
                }
            }
        }
    }

    // Displaced clips fill new tracks, first fit, in start order.
    std::stable_sort(displaced.begin(), displaced.end(),
        [](const Displaced& a, const Displaced& b) { return a.clip.start < b.clip.start; });
    for (Displaced& d : displaced) {
        auto& list = data.tracks(d.type);
        TrackData* target = nullptr;
        int existing = 0;
        for (TrackData& t : list) {
            if (!t.name.startsWith(QLatin1String("Repaired ")))
                continue;
            ++existing;
            if (t.end() <= d.clip.start) {
                target = &t;
                break;
            }
        }
        if (!target) {
            TrackData track;
            track.type = d.type;
            track.name = QStringLiteral("Repaired %1").arg(existing + 1);
            list.push_back(std::move(track));
            target = &list.back();
        }
        target->clips.push_back(std::move(d.clip));
    }

    // Ids: every track, clip, effect and mask unique, non-zero and below nextId.
    normalizeIds(data);
    QSet<quint64> used;
    int renumbered = 0;
    auto claim = [&](quint64& id) {
        if (id == 0 || used.contains(id)) {
            id = data.nextId++;
            ++renumbered;
        }
        used.insert(id);
    };
    for (TrackType type : kTrackTypes) {
        for (TrackData& track : data.tracks(type)) {
            claim(track.id);
            for (ClipData& c : track.clips) {
                claim(c.id);
                for (EffectInstance& fx : c.effects)
                    claim(fx.id);
                for (MaskData& mask : c.masks)
                    claim(mask.id);
            }
        }
    }

    if (dropped > 0)
        notes.append(say("%n clip(s) with no length were removed.", dropped));
    if (trimmedHeads > 0)
        notes.append(say("%n clip(s) started before the timeline and were trimmed.", trimmedHeads));
    if (moved > 0)
        notes.append(say("%n overlapping or misplaced clip(s) were moved to new \"Repaired\" "
                         "tracks at the same time.",
            moved));
    if (renumbered > 0)
        notes.append(say("%n duplicate or missing id(s) were renumbered.", renumbered));
    if (fixedValues > 0)
        notes.append(say("%n invalid value(s) were reset.", fixedValues));

    if (log)
        *log += notes;
    return !notes.isEmpty();
}

} // namespace kk::editops
