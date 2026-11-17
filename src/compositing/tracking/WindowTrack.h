// SPDX-License-Identifier: GPL-3.0-or-later
// WindowTrack — the Tracker palette of the Color page (spec section 107): follows the subject of
// a window through the clip and stores the result as the node's tracker data (core/Grade.h,
// GradeTrack), so every window of the node follows it. planWindowTrack() turns the window (as the
// picture shows it at the start frame) and the checkboxes into a TrackJob for the TrackerRunner (a
// point when only Pan / Tilt are wanted, the planar tracker's region when Zoom, Rotate or 3D are);
// absoluteSample() turns a found position into a track sample; mergeSamples() writes a run into
// the track, extending a track made earlier. Pure maths: no widgets, no threads, no project.
#pragma once

#include "compositing/tracking/TrackerRunner.h"
#include "core/Grade.h"
#include "core/GradeMath.h"

#include <QString>

#include <optional>
#include <vector>

namespace kk::tracking {

/// A planned window track. The job's frames are grade frames (clip-local for a clip grade); the
/// caller adds the clip's start to render them.
struct WindowTrackPlan
{
    TrackJob job; ///< `source` is left for the caller
    grademath::WindowGeometry start; ///< the window as shown on the start frame
    GradeTrackSample base; ///< the track's sample on the start frame (identity for a new track)
    bool hadData = false; ///< the node already had tracker data
    QPointF pivot {0.5, 0.5}; ///< pivot of a new track
    double aspect = 16.0 / 9.0; ///< picture width / height
    bool planar = false;
};

/// Plans tracking window @p w of @p node from grade frame @p startFrame to @p endFrame
/// (either order; the job runs in that direction). nullopt with a sentence in @p why when the
/// window cannot be tracked (not a circle / linear / polygon window, nothing to follow).
[[nodiscard]] std::optional<WindowTrackPlan> planWindowTrack(const GradeNode& node,
    const GradeWindow& w, int64_t startFrame, int64_t endFrame, double aspect, QString* why);

/// The track sample (relative to the track start) of a found position: @p sample of a run made
/// with @p plan.
[[nodiscard]] GradeTrackSample absoluteSample(const WindowTrackPlan& plan, const TrackSample& sample);

/// Writes the samples of a run made with @p plan into @p track (frames outside the run keep their
/// samples; gaps are held). A track without data gets the plan's pivot.
void mergeSamples(GradeTrack& track, const WindowTrackPlan& plan,
    const std::vector<TrackSample>& samples);

} // namespace kk::tracking
