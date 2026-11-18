// SPDX-License-Identifier: GPL-3.0-or-later
// ParamModifier — what changes a parameter's value on its way out of the ParamSet (spec 119):
// Perturb, Wiggle, Offset, Expression, Cycle, XY Path (one path per axis), Connect To (a link to
// another parameter) and Publish (a marker that offers the parameter to Connect To and Macros).
#pragma once

#include <QString>
#include <QVector>

#include <cstdint>

namespace kk {

/// The kinds of modifier. The numeric order is not stored: project files use modifierKindId().
enum class ModifierKind : quint8 {
    Perturb, ///< smooth random offset: a new random target every Period frames
    Wiggle, ///< layered noise offset: Wavelength, Amplitude, Octaves
    Offset, ///< value * Scale + Offset, read Time Offset frames earlier / later
    Expression, ///< a formula (core/Expression.h) with Number In 1 to 3
    Cycle, ///< repeats a frame range of the value (Loop, Ping-Pong, Relative)
    Path, ///< one axis of an XY Path: the value is the modifier's own animated "Path" curve
    Connect, ///< the value of another node's parameter
    Publish ///< no change of value; the parameter is offered to Connect To and Macros
};

/// One modifier on one parameter (AnimatedValue::modifiers, applied in list order, the first one
/// sees the keyframed value). Its numbers live in the same ParamSet under keys made by
/// modifiers::controlKey(id, name), so they are keyframeable, edited by the ordinary rows, saved
/// and undone with the node, and shown in the Curves view like any other parameter.
struct ParamModifier
{
    ModifierKind kind = ModifierKind::Offset;
    QString id; ///< unique within the ParamSet ("m1", "m2", ...)
    QString text; ///< Expression: the formula
    quint64 targetNode = 0; ///< Connect: id of the node read
    QString targetKey; ///< Connect: key of the parameter read
    bool enabled = true; ///< false = the modifier is skipped (kept for later)

    friend bool operator==(const ParamModifier&, const ParamModifier&) = default;
};

/// Stable id of @p kind in project files ("perturb", "wiggle", "offset", "expression", "cycle",
/// "path", "connect", "publish").
[[nodiscard]] QString modifierKindId(ModifierKind kind);
/// Parses modifierKindId(); false for an unknown id.
[[nodiscard]] bool modifierKindFromId(const QString& id, ModifierKind* kind);

} // namespace kk
