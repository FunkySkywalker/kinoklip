// SPDX-License-Identifier: GPL-3.0-or-later
// GradeKeyframes — the keyframes of one corrector node as the Color page's Keyframes palette
// shows them: one row per node (its parameters, its windows' parameters and shape keys), Static
// or Dynamic keys, the dissolve type of the Dynamic ones. Thin wrappers over core/KeyRows.h; the
// grade is evaluated at the playhead by the one evaluation path (GPU and CPU alike), so keys
// need nothing else.
#pragma once

#include "core/Grade.h"
#include "core/KeyRows.h"

namespace kk::gradekeyframes {

/// The row of @p node: its parameters and those of its windows.
[[nodiscard]] keyrows::ConstSlices rowOf(const GradeNode& node);
[[nodiscard]] keyrows::Slices rowOf(GradeNode& node);

/// Sorted, unique frames with a key in the node (window shape keys included).
[[nodiscard]] QVector<int64_t> frames(const GradeNode& node);
[[nodiscard]] bool hasKeyAt(const GradeNode& node, int64_t frame);
[[nodiscard]] keyrows::Mode modeAt(const GradeNode& node, int64_t frame);
[[nodiscard]] Interp interpAt(const GradeNode& node, int64_t frame);

/// Keys every parameter of the node and of its windows at @p frame with its value there
/// (shape keys too when a window has them). Returns the number of keys added.
int addKeys(GradeNode& node, int64_t frame);
/// Removes the keys at @p frame. Returns the number removed.
int deleteKeys(GradeNode& node, int64_t frame);
/// Moves the keys at @p from to @p to. Returns the number moved.
int moveKeys(GradeNode& node, int64_t from, int64_t to);
/// Static / Dynamic for the keys at @p frame. Returns the number changed.
int setMode(GradeNode& node, int64_t frame, keyrows::Mode mode);
/// Dissolve type of the Dynamic keys at @p frame (Linear, EaseIn, EaseOut, EaseInOut, Smooth).
int setInterp(GradeNode& node, int64_t frame, Interp interp);

} // namespace kk::gradekeyframes
