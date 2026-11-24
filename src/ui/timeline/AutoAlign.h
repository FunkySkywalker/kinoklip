// SPDX-License-Identifier: GPL-3.0-or-later
// AutoAlign — Auto Align Clips (Resolve's timeline clip menu, issue 16): moves clips that are
// already on the timeline so their sound (Based on Waveform) or their start timecode (Based on
// Timecode) matches a reference clip. The correlator is media::findSyncOffset(), the same one
// Sync and Place on Timeline and multicam clips use.
//
// Three steps so the slow part can run on a worker thread without touching the project:
//   prepare()  main thread: reads the clips and their media into a Job (a plain value).
//   analyse()  any thread: decodes the audio and finds the offsets; the Job is not changed.
//   apply()    main thread, inside one undo step: moves what was found, refuses what would
//              overwrite other clips.
#pragma once

#include "core/Rational.h"
#include "core/TimelineData.h"
#include "media/MediaInfo.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>
#include <optional>

namespace kk {

class MediaPool;

namespace autoalign {

enum class Method { Waveform, Timecode };

/// What to align.
struct Request
{
    /// The clip that never moves; 0 = the clip on the lowest video track among @c clips (else
    /// the first one).
    quint64 reference = 0;
    QVector<quint64> clips; ///< the clips to line up (may contain the reference)
    Method method = Method::Waveform;
    /// After aligning, silence the reference's own audio (the camera sound), so only the
    /// external recording is heard. Only when at least one clip was aligned.
    bool muteReference = false;
};

enum class Status {
    Aligned, ///< moved (or would be moved by apply())
    Unchanged, ///< already in sync
    Unreliable, ///< no clear match: not moved
    Skipped, ///< cannot be analysed (no audio, offline, retimed, no timecode, ...)
    Blocked ///< found, but moving would overwrite clips, hit a locked track or pass frame 0
};

[[nodiscard]] QString statusId(Status status);

/// One clip (with its link group) to align.
struct Item
{
    quint64 clip = 0; ///< the clip the analysis is about (reference or clip to move)
    QVector<quint64> members; ///< clip and everything linked to it: all move together
    QString name;
    QString mediaId;
    QString path;
    media::MediaInfo info;
    int64_t start = 0; ///< timeline frame of `clip`
    int64_t sourceInUs = 0;
    QString problem; ///< not empty: cannot be used (becomes Status::Skipped)
};

struct Job
{
    Method method = Method::Waveform;
    Rational fps {25, 1};
    bool muteReference = false;
    Item reference;
    QVector<Item> others;
    /// The audio-track clips of the reference's group (what muteReference silences).
    QVector<quint64> referenceAudio;
};

struct Entry
{
    quint64 clip = 0;
    QVector<quint64> members;
    QString name;
    Status status = Status::Skipped;
    QString reason; ///< why not aligned (empty when Aligned / Unchanged)
    int64_t deltaFrames = 0; ///< how far the clip (and its group) moves
    int64_t newStart = 0;
    double offsetSeconds = 0.0; ///< sound offset found, media time of the clip vs the reference
    double confidence = -1.0; ///< 0..1 (waveform), -1 = not applicable
};

struct Result
{
    QVector<Entry> entries;
    bool cancelled = false;
    bool muted = false; ///< the reference's audio was silenced
    [[nodiscard]] int count(Status status) const;
};

/// Reads @p request against @p data. nullopt with @p error set (and @p code, a stable command
/// error code) when it cannot be done at all: unknown clip, fewer than two clips, the
/// reference is not a media clip.
[[nodiscard]] std::optional<Job> prepare(const TimelineData& data, const MediaPool& pool,
    const Request& request, QString* error, QString* code = nullptr);

/// Finds the offsets (blocking; slow for Waveform: decodes the audio of every distinct file,
/// in parallel). @p cancel is polled; @p progress is called with (done, total) from workers.
[[nodiscard]] Result analyse(const Job& job, const std::atomic<bool>* cancel = nullptr,
    const std::function<void(int, int)>& progress = {});

/// Moves the Aligned entries of @p result on @p data (call inside one edit). An entry that
/// cannot move safely becomes Blocked with its reason and nothing of it changes: the clips of
/// its group would overwrite other clips (the editor's plain move overwrites; Auto Align
/// refuses instead), a track is locked, or a clip would start before frame 0. Applies
/// muteReference. Returns whether anything changed.
bool apply(TimelineData& data, const Job& job, Result& result);

/// {"reference","method","aligned":n,"clips":[{"clip","name","status","reason","delta",
/// "newStart","offsetSeconds","confidence"}],"muted"}.
[[nodiscard]] QJsonObject toJson(const Job& job, const Result& result);
/// One line per clip, for messages: "Interview: moved 12 frames", "Cam B: not reliable ...".
[[nodiscard]] QString summary(const Result& result);

} // namespace autoalign
} // namespace kk
