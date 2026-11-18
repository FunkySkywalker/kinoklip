// SPDX-License-Identifier: GPL-3.0-or-later
// TrackStore — writes tracked positions into a Tracker node as keyframed X / Y data.
#pragma once

#include "compositing/tracking/TrackerRunner.h"
#include "core/Composition.h"

#include <vector>

namespace kk::tracking {

/// Stores @p samples as keys of the node's CenterX / CenterY (linear keys at the sample frames;
/// existing keys at those frames are replaced, other keys stay). A node whose point was not
/// animated yet also gets its Reference Frame set to @p referenceFrame, so the first track
/// leaves the picture where it is at the frame it started on. A Planar Tracker node gets the
/// eight corner keys instead (nodekeys::planarCorner()), from the samples' corners. False if
/// there is nothing to store.
/// A Tracker with several patterns stores into pattern @p pattern (1-based; nodekeys::patternKey()).
bool storeTrack(CompNode& node, const std::vector<TrackSample>& samples, int64_t referenceFrame,
    int pattern = 1);

/// Removes all tracked keys: the point (a planar node: every corner) stays where it is at
/// @p frame.
/// A Tracker clears only pattern @p pattern (0 = every pattern).
void clearTrack(CompNode& node, int64_t frame, int pattern = 0);

} // namespace kk::tracking
