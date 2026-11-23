// SPDX-License-Identifier: GPL-3.0-or-later
// Sizing — the Color page's Sizing palette. Input Sizing is the clip's own transform (the Edit
// page Inspector's Position, Zoom, Rotation and Flip: one source of truth, ClipData::transform);
// Output Sizing is the same set of parameters on the timeline (TimelineData::outputSizing),
// applied to the composite of every track after the timeline grade, before the captions.
// Keyframes of Input Sizing are clip-local frames, those of Output Sizing timeline frames.
// Pure functions; the renderers call outputTransform().
#pragma once

#include "core/KeyRows.h"
#include "core/ParamKeys.h"

#include <QSizeF>
#include <QStringList>
#include <QTransform>

#include <cstdint>

namespace kk::sizing {

/// The transform parameters the Sizing palette edits (and its keyframe row shows): Pan (posX),
/// Tilt (posY), Zoom X / Y, Rotate. Flip H / V are statics (they do not animate).
[[nodiscard]] const QStringList& sizingKeys();

/// The keyframe row of a Sizing ParamSet.
[[nodiscard]] keyrows::Slices rowOf(ParamSet& params);
[[nodiscard]] keyrows::ConstSlices rowOf(const ParamSet& params);
/// Keys the sizing parameters at @p frame with their values there. Returns the number added.
int addKeys(ParamSet& params, int64_t frame);

/// Zoom as the palette shows it: the geometric mean of Width (Zoom X) and Height (Zoom Y), so
/// Zoom changes both together and keeps their ratio.
[[nodiscard]] double zoomOf(const ParamSet& params, int64_t frame);
/// Sets the parameter @p key at @p frame: a key when @p forceKey (Auto Keyframe) or when the
/// parameter is animated, else its constant value.
void setAt(ParamSet& params, const QString& key, int64_t frame, double value, bool forceKey);
/// Sets Zoom (see zoomOf()): Width and Height scale by the same factor.
void setZoomAt(ParamSet& params, int64_t frame, double zoom, bool forceKey);

/// True when @p params leaves the picture unchanged at @p frame.
[[nodiscard]] bool isIdentity(const ParamSet& params, int64_t frame);
/// True when @p params holds nothing (the default; not saved).
[[nodiscard]] bool isEmpty(const ParamSet& params);

/// Output Sizing as a picture transform in pixels of an output of @p outSize (origin top-left,
/// y down): zoom, flip and rotation about the picture centre, then the pan / tilt offset. The
/// offsets are timeline pixels (@p timelineSize) and scale with the output. Identity for an
/// empty set.
[[nodiscard]] QTransform outputTransform(
    const ParamSet& params, int64_t frame, QSizeF outSize, QSizeF timelineSize);

} // namespace kk::sizing
