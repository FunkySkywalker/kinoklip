// SPDX-License-Identifier: GPL-3.0-or-later
// KeyRows — keyframe rows as the Color page's Keyframes palette shows them: one row is a group
// of parameters (a corrector node with its windows, Input Sizing, Output Sizing), its keys are
// the union of the keys of those parameters, and every edit (add, delete, move, Static /
// Dynamic, dissolve type) acts on all the parameters of the row at a frame at once. Pure
// functions on ParamSet; no UI.
#pragma once

#include "core/Animation.h"

#include <QStringList>
#include <QVector>

#include <cstdint>
#include <vector>

namespace kk::keyrows {

/// A ParamSet taking part in a row. @c keys limits it to those parameters (empty = all).
struct Slice
{
    ParamSet* set = nullptr;
    QStringList keys;
};
/// Read-only counterpart of Slice.
struct ConstSlice
{
    const ParamSet* set = nullptr;
    QStringList keys;
};
using Slices = std::vector<Slice>;
using ConstSlices = std::vector<ConstSlice>;

/// What a keyframe does between itself and the next one (Resolve: Static / Dynamic).
enum class Mode {
    Static, ///< holds its value until the next key, then jumps
    Dynamic ///< interpolates to the next key
};

/// Sorted, unique frames that have a key in any parameter of the row.
[[nodiscard]] QVector<int64_t> frames(const ConstSlices& row);
/// True if any parameter of the row has a key at @p frame.
[[nodiscard]] bool hasKeyAt(const ConstSlices& row, int64_t frame);
/// Static when every key at @p frame holds, else Dynamic (no key: Dynamic).
[[nodiscard]] Mode modeAt(const ConstSlices& row, int64_t frame);
/// The preset shared by the Dynamic keys at @p frame, Interp::Custom when they differ or there
/// is none.
[[nodiscard]] Interp interpAt(const ConstSlices& row, int64_t frame);

/// Gives every parameter of the row a key at @p frame holding the value it has there (a
/// parameter that is already keyed there is left alone). @p defaultOf gives the value of a
/// parameter without a stored value. Returns the number of keys added.
int addKeys(const Slices& row, int64_t frame, double (*defaultOf)(const QString&));
/// Removes the keys at @p frame; a parameter that loses its last key keeps its value there.
/// Returns the number removed.
int deleteKeys(const Slices& row, int64_t frame);
/// Moves the keys at @p from to @p to (a key already at @p to is replaced). Returns the number
/// moved.
int moveKeys(const Slices& row, int64_t from, int64_t to);
/// Static (hold) or Dynamic on the keys at @p frame. Returns the number of keys changed.
int setMode(const Slices& row, int64_t frame, Mode mode);
/// Dissolve type of the Dynamic keys at @p frame: Linear, EaseIn, EaseOut, EaseInOut or Smooth
/// (other presets are ignored); makes the keys Dynamic. Returns the number of keys changed.
int setInterp(const Slices& row, int64_t frame, Interp interp);

} // namespace kk::keyrows
