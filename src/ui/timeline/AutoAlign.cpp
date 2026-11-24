// SPDX-License-Identifier: GPL-3.0-or-later
// AutoAlign — see AutoAlign.h.
#include "ui/timeline/AutoAlign.h"

#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Multicam.h"
#include "core/ParamKeys.h"
#include "core/Retime.h"
#include "media/AudioSync.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QStringList>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <map>

namespace kk::autoalign {

namespace ops = editops;

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::AutoAlign", text);
}

/// Silence, like the Inspector's lowest volume.
constexpr double kSilenceDb = -96.0;
/// Largest media offset searched, in seconds (the longer recording of the pair, capped).
constexpr double kMaxSearchSeconds = 7200.0;

bool isUsable(const ClipData& c, const MediaPool& pool, Method method, QString* problem)
{
    if (c.kind != ClipKind::Media || c.mediaId.isEmpty()) {
        *problem = tr("only clips of media files can be aligned");
        return false;
    }
    const MediaItem* item = pool.item(c.mediaId);
    if (!item || pool.isOffline(c.mediaId)) {
        *problem = tr("the media is offline");
        return false;
    }
    if (retime::isRetimed(c)) {
        *problem = tr("the clip is retimed");
        return false;
    }
    if (method == Method::Waveform && !item->info.hasAudio) {
        *problem = tr("the media has no audio");
        return false;
    }
    if (method == Method::Timecode
        && !multicam::timecodeToUs(item->info.startTimecode, Rational {25, 1})) {
        *problem = tr("the media has no start timecode");
        return false;
    }
    return true;
}

/// The Item of clip @p id: the clip itself when it can be analysed, else a clip linked to it.
Item makeItem(const TimelineData& data, const MediaPool& pool, quint64 id, Method method)
{
    Item item;
    item.clip = id;
    item.members = ops::withLinkedClips(data, {id});
    QString firstProblem;
    QVector<quint64> order {id};
    for (quint64 m : std::as_const(item.members)) {
        if (m != id)
            order.append(m);
    }
    for (quint64 candidate : std::as_const(order)) {
        const ClipData* c = ops::clipById(data, candidate);
        if (!c)
            continue;
        QString problem;
        if (isUsable(*c, pool, method, &problem)) {
            const MediaItem* media = pool.item(c->mediaId);
            item.clip = candidate;
            item.name = c->name;
            item.mediaId = c->mediaId;
            item.path = media->path;
            item.info = media->info;
            item.start = c->start;
            item.sourceInUs = c->sourceInUs;
            item.problem.clear();
            return item;
        }
        if (candidate == id) {
            firstProblem = problem;
            item.name = c->name;
            item.start = c->start;
        }
    }
    item.problem = firstProblem.isEmpty() ? tr("no clip with usable audio") : firstProblem;
    return item;
}

} // namespace

QString statusId(Status status)
{
    switch (status) {
    case Status::Aligned:
        return QStringLiteral("aligned");
    case Status::Unchanged:
        return QStringLiteral("unchanged");
    case Status::Unreliable:
        return QStringLiteral("unreliable");
    case Status::Skipped:
        return QStringLiteral("skipped");
    case Status::Blocked:
        break;
    }
    return QStringLiteral("blocked");
}

int Result::count(Status status) const
{
    return int(std::count_if(
        entries.cbegin(), entries.cend(), [status](const Entry& e) { return e.status == status; }));
}

std::optional<Job> prepare(const TimelineData& data, const MediaPool& pool, const Request& request,
    QString* error, QString* code)
{
    auto fail = [&](const char* c, const QString& message) -> std::optional<Job> {
        if (code)
            *code = QString::fromLatin1(c);
        if (error)
            *error = message;
        return std::nullopt;
    };
    QVector<quint64> ids;
    for (quint64 id : request.clips) {
        if (!ops::clipById(data, id))
            return fail("unknown_clip", tr("no clip with id %1").arg(id));
        if (!ids.contains(id))
            ids.append(id);
    }
    quint64 reference = request.reference;
    if (reference != 0 && !ops::clipById(data, reference))
        return fail("unknown_clip", tr("no clip with id %1").arg(reference));
    if (reference == 0) {
        // The clip on the lowest video track (Resolve's reference), else the first one.
        int best = -1;
        for (quint64 id : std::as_const(ids)) {
            const auto loc = ops::findClip(data, id);
            if (loc && loc->track.type == TrackType::Video
                && (best < 0 || loc->track.index < best)) {
                best = loc->track.index;
                reference = id;
            }
        }
        if (reference == 0 && !ids.isEmpty())
            reference = ids.first();
    }
    if (reference == 0)
        return fail("bad_value", tr("select the clips to align"));
    if (!ids.contains(reference))
        ids.append(reference);

    Job job;
    job.method = request.method;
    job.fps = data.fps;
    job.muteReference = request.muteReference;
    job.reference = makeItem(data, pool, reference, request.method);
    if (!job.reference.problem.isEmpty())
        return fail("cannot_align",
            tr("the reference clip cannot be used: %1").arg(job.reference.problem));
    for (quint64 m : std::as_const(job.reference.members)) {
        const auto loc = ops::findClip(data, m);
        if (loc && loc->track.type == TrackType::Audio)
            job.referenceAudio.append(m);
    }
    QVector<quint64> taken = job.reference.members;
    for (quint64 id : std::as_const(ids)) {
        if (taken.contains(id))
            continue; // the reference's own group (its picture and sound) never moves
        Item item = makeItem(data, pool, id, request.method);
        for (quint64 m : std::as_const(item.members))
            taken.append(m);
        job.others.append(item);
    }
    if (job.others.isEmpty())
        return fail("bad_value",
            tr("select at least two clips that are not linked to each other (the reference and "
               "the clips to line up with it)"));
    return job;
}

Result analyse(
    const Job& job, const std::atomic<bool>* cancel, const std::function<void(int, int)>& progress)
{
    Result result;
    const Item& ref = job.reference;
    auto entryOf = [](const Item& item) {
        Entry e;
        e.clip = item.clip;
        e.members = item.members;
        e.name = item.name;
        return e;
    };
    // Features of every distinct file (waveform only), decoded in parallel.
    std::map<QString, std::optional<media::SyncFeatures>> features;
    std::map<QString, QString> errors;
    if (job.method == Method::Waveform) {
        QStringList paths {ref.path};
        for (const Item& o : job.others) {
            if (o.problem.isEmpty() && !paths.contains(o.path))
                paths.append(o.path);
        }
        for (const QString& p : std::as_const(paths)) {
            features[p];
            errors[p];
        }
        std::atomic<int> done {0};
        QVector<QFuture<void>> jobs;
        for (const QString& p : std::as_const(paths)) {
            auto* slot = &features[p];
            auto* err = &errors[p];
            jobs.append(QtConcurrent::run([&, p, slot, err] {
                *slot = media::syncFeaturesFromFile(p, cancel, err);
                if (progress)
                    progress(++done, int(paths.size()));
            }));
        }
        for (QFuture<void>& f : jobs)
            f.waitForFinished();
        if (cancel && cancel->load()) {
            result.cancelled = true;
            return result;
        }
    }

    for (const Item& other : job.others) {
        Entry e = entryOf(other);
        if (!other.problem.isEmpty()) {
            e.status = Status::Skipped;
            e.reason = other.problem;
            result.entries.append(e);
            continue;
        }
        int64_t offsetUs = 0;
        if (job.method == Method::Timecode) {
            const auto tcs = multicam::offsetsFromTimecode({ref.info, other.info});
            if (tcs.size() != 2 || !tcs[0] || !tcs[1]) {
                e.status = Status::Skipped;
                e.reason = tr("no start timecode in the file");
                result.entries.append(e);
                continue;
            }
            offsetUs = *tcs[1] - *tcs[0];
            e.offsetSeconds = double(offsetUs) / 1e6;
        } else {
            const auto& refFeatures = features[ref.path];
            const auto& otherFeatures = features[other.path];
            if (!refFeatures || !otherFeatures) {
                e.status = Status::Skipped;
                e.reason = tr("the audio could not be read (%1)")
                               .arg(!refFeatures ? errors[ref.path] : errors[other.path]);
                result.entries.append(e);
                continue;
            }
            media::SyncOptions options;
            options.maxOffsetSeconds = std::min(
                kMaxSearchSeconds, std::max(refFeatures->seconds(), otherFeatures->seconds()));
            const media::SyncMatch m = media::findSyncOffset(*refFeatures, *otherFeatures, options);
            e.confidence = m.confidence;
            if (!m.found) {
                e.status = Status::Unreliable;
                e.reason = tr("no clear match with the reference sound (confidence %1 %); not "
                              "moved")
                               .arg(int(std::lround(m.confidence * 100)));
                result.entries.append(e);
                continue;
            }
            e.offsetSeconds = m.offsetSeconds;
            offsetUs = int64_t(std::llround(m.offsetSeconds * 1e6));
        }
        // Source time s of the clip's media sounds like source time s + offset of the
        // reference's media; the reference shows its source time t at timeline frame
        // ref.start + (t - ref.in) * fps.
        const double frames = double(other.sourceInUs + offsetUs - ref.sourceInUs) * job.fps.num
            / (1e6 * job.fps.den);
        e.newStart = ref.start + int64_t(std::llround(frames));
        e.deltaFrames = e.newStart - other.start;
        e.status = e.deltaFrames == 0 ? Status::Unchanged : Status::Aligned;
        result.entries.append(e);
    }
    return result;
}

bool apply(TimelineData& data, const Job& job, Result& result)
{
    bool changed = false;
    for (Entry& e : result.entries) {
        if (e.status != Status::Aligned)
            continue;
        QString why;
        for (quint64 m : std::as_const(e.members)) {
            const auto loc = ops::findClip(data, m);
            if (!loc) {
                why = tr("the clip is gone");
                break;
            }
            const TrackData* track = data.track(loc->track);
            const ClipData& c = track->clips[size_t(loc->clipIndex)];
            if (track->locked) {
                why = tr("a track of the clip is locked");
                break;
            }
            const int64_t start = c.start + e.deltaFrames;
            if (start < 0) {
                why = tr("it would start before the beginning of the timeline");
                break;
            }
            for (const ClipData& o : track->clips) {
                if (!e.members.contains(o.id) && o.start < start + c.duration && o.end() > start) {
                    why = tr("it would overwrite another clip on its track");
                    break;
                }
            }
            if (!why.isEmpty())
                break;
        }
        if (why.isEmpty() && !ops::moveClips(data, e.members, e.deltaFrames, 0))
            why = tr("the clip could not be moved");
        if (why.isEmpty()) {
            changed = true;
        } else {
            e.status = Status::Blocked;
            e.reason = why;
        }
    }
    if (job.muteReference && result.count(Status::Aligned) + result.count(Status::Unchanged) > 0) {
        for (quint64 id : job.referenceAudio) {
            if (ClipData* c = ops::clipById(data, id)) {
                c->audio.setBase(keys::VolumeDb, kSilenceDb);
                result.muted = true;
                changed = true;
            }
        }
    }
    return changed;
}

QJsonObject toJson(const Job& job, const Result& result)
{
    QJsonArray clips;
    for (const Entry& e : result.entries) {
        QJsonObject o {{QStringLiteral("clip"), qint64(e.clip)}, {QStringLiteral("name"), e.name},
            {QStringLiteral("status"), statusId(e.status)},
            {QStringLiteral("delta"), qint64(e.status == Status::Aligned ? e.deltaFrames : 0)}};
        if (!e.reason.isEmpty())
            o.insert(QStringLiteral("reason"), e.reason);
        if (e.status == Status::Aligned || e.status == Status::Unchanged
            || e.status == Status::Blocked) {
            o.insert(QStringLiteral("newStart"), qint64(e.newStart));
            o.insert(QStringLiteral("offsetSeconds"), e.offsetSeconds);
        }
        if (e.confidence >= 0)
            o.insert(QStringLiteral("confidence"), e.confidence);
        QJsonArray members;
        for (quint64 m : e.members)
            members.append(qint64(m));
        o.insert(QStringLiteral("clips"), members);
        clips.append(o);
    }
    return {{QStringLiteral("reference"), qint64(job.reference.clip)},
        {QStringLiteral("method"),
            job.method == Method::Waveform ? QStringLiteral("waveform")
                                           : QStringLiteral("timecode")},
        {QStringLiteral("aligned"), result.count(Status::Aligned)},
        {QStringLiteral("unreliable"), result.count(Status::Unreliable)},
        {QStringLiteral("clips"), clips}, {QStringLiteral("muted"), result.muted}};
}

QString summary(const Result& result)
{
    QStringList lines;
    for (const Entry& e : result.entries) {
        const QString name = e.name.isEmpty() ? QString::number(e.clip) : e.name;
        switch (e.status) {
        case Status::Aligned:
            lines.append(tr("%1: moved %2 frames").arg(name).arg(e.deltaFrames));
            break;
        case Status::Unchanged:
            lines.append(tr("%1: already in sync").arg(name));
            break;
        default:
            lines.append(tr("%1: not moved, %2").arg(name, e.reason));
            break;
        }
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace kk::autoalign
