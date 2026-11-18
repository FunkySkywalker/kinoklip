// SPDX-License-Identifier: GPL-3.0-or-later
// TrackFollow — makes a node (a mask, or any node with a Center) follow a track with its
// position, size and angle, or moves the points of a path shape (the Mask Tracker and the Point
// Follower, spec section 117). Like TrackLink it copies the track into plain keyframes: there are
// no parameter links, so the result stays editable and the renderers do not change. Also bakes a
// Match Move effect's track into an Edit page mask. Pure maths on project types.
#pragma once

#include "core/Composition.h"
#include "core/MatchMove.h"
#include "core/TimelineData.h"

#include <QSizeF>
#include <QTransform>

#include <vector>

namespace kk::tracking {

/// What a node takes from a track. `points` moves the points of the path shape instead of the
/// node's own Center / Size / Angle (the shape is keyframed, the node's controls stay).
struct FollowOptions
{
    bool position = true;
    bool size = false;
    bool angle = false;
    bool points = false;
};

/// What a track did between comp frames @p refFrame and @p frame, in picture pixels (y down):
/// a Tracker's point moves the pivot, a Planar Tracker's region also turns and scales it, and a
/// Tracker with several patterns turns and scales about their middle.
[[nodiscard]] matchmove::Motion sourceMotion(
    const CompNode& source, int64_t refFrame, int64_t frame, QSizeF frameSize);

/// The transform of @p motion for the parts @p o asks for, about @p fallbackPivot when the
/// position is not followed (the node then turns / scales about its own centre).
[[nodiscard]] QTransform motionTransform(
    const matchmove::Motion& motion, const FollowOptions& o, QPointF fallbackPivot);

/// True if @p node has a path with points (Polygon, B-Spline and Bezier masks, a Text node with the
/// Path layout).
[[nodiscard]] bool hasPathPoints(const CompNode& node);
/// True if @p node has a Size (or Width and Height) / an Angle that can follow a track.
[[nodiscard]] bool canFollowSize(const CompNode& node);
[[nodiscard]] bool canFollowAngle(const CompNode& node);

/// Replaces the Center, Size and Angle keys (as far as @p o asks and the node has them), or the
/// shape keys when @p o.points, with linear keys at every frame of @p source's track. The node
/// keeps its place, size and angle at @p offsetFrame and moves as the track did from then on.
/// False if @p target cannot follow, @p source has no track, or either node is locked.
bool followTrack(CompNode& target, const CompNode& source, int64_t offsetFrame, QSizeF frameSize,
    const FollowOptions& o);

/// The Point Follower: path points @p indices of @p target follow the position of @p source's
/// track from @p offsetFrame on (shape keys at every track frame; other points stay). False for
/// no path, no track, no valid index or a locked node.
bool followPoints(CompNode& target, const std::vector<int>& indices, const CompNode& source,
    int64_t offsetFrame, QSizeF frameSize);

/// Edit page: bakes the track of Match Move effect @p fx of @p clip into @p mask (Position X / Y,
/// Scale X / Y and Rotation keys, or the shape keys with @p o.points) at every frame of the clip.
/// The mask keeps its place at clip-local frame @p offsetFrame. The clip's picture is taken to
/// fill the frame (chosen). False when the effect has no usable track.
bool followMatchMove(MaskData& mask, const ClipData& clip, const EffectInstance& fx,
    int64_t offsetFrame, QSizeF frameSize, Rational fps, const FollowOptions& o);

} // namespace kk::tracking
