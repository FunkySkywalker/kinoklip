// SPDX-License-Identifier: GPL-3.0-or-later
// Modifiers — evaluation and editing of parameter modifiers (spec 119). The value of a modified
// parameter is computed here: ParamSet::value() calls evaluate() for every AnimatedValue that has
// modifiers. Pure functions, no widgets; the Compositing page Inspector and the commands only
// call the editing functions.
#pragma once

#include "core/Animation.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace kk::modifiers {

/// One control of a modifier (a number or a choice) as the Inspector shows it.
struct Control
{
    QString name; ///< "period", "strength", ... (the key is controlKey(id, name))
    QString label; ///< translated
    QString tooltip; ///< translated
    double min = 0.0, max = 1.0, def = 0.0, step = 1.0;
    QString unit;
    bool isInt = false; ///< a whole number
    QStringList choices; ///< non-empty: a Choice stored as a static string (ids)
    QStringList choiceLabels; ///< translated
    QString defChoice;
};

/// The controls of @p kind in Inspector order (Expression: Number In 1 to 3; Perturb: Period,
/// Strength, Random Seed; ...). Kinds without controls (Connect, Publish) give an empty list.
[[nodiscard]] QVector<Control> controls(ModifierKind kind);

/// Translated name of @p kind ("Perturb", "XY Path", "Connect To", ...).
[[nodiscard]] QString kindName(ModifierKind kind);
/// One-sentence translated description of @p kind (tooltips, the Effects Library).
[[nodiscard]] QString kindDescription(ModifierKind kind);

/// The ParamSet key of control @p name of modifier @p id: "~m1.period". The "~" prefix keeps it
/// apart from every descriptor key.
[[nodiscard]] QString controlKey(const QString& id, const QString& name);
/// True for a key made by controlKey().
[[nodiscard]] bool isControlKey(const QString& key);

/// Value of @p value with its modifiers applied at fractional frame @p frame. Called by
/// ParamSet::value() / valueAtTime(); modifiers read their controls from @p params.
[[nodiscard]] double evaluate(const ParamSet& params, const AnimatedValue& value, double frame);

/// Adds a modifier of @p kind to parameter @p key (created with base 0 when absent) and the
/// default values of its controls; returns its id. Perturb / Wiggle get a seed from the key so
/// the X and Y of a point do not move together. Publish is added once only (the existing id is
/// returned).
QString add(ParamSet& params, const QString& key, ModifierKind kind);
/// Removes modifier @p id of parameter @p key and its control keys. False if there is none.
bool remove(ParamSet& params, const QString& key, const QString& id);
/// The modifier @p id of parameter @p key, or nullptr.
[[nodiscard]] const ParamModifier* find(const ParamSet& params, const QString& key, const QString& id);
[[nodiscard]] ParamModifier* find(ParamSet& params, const QString& key, const QString& id);
/// First modifier of @p kind on @p key, or nullptr.
[[nodiscard]] const ParamModifier* first(const ParamSet& params, const QString& key, ModifierKind kind);
/// True if parameter @p key has any modifier that changes its value (not just Publish).
[[nodiscard]] bool isModified(const ParamSet& params, const QString& key);
/// True if @p kind replaces the value with something else (Expression, Path, Connect), so the
/// keyframes and the field under it have no effect while it is enabled.
[[nodiscard]] bool replacesValue(ModifierKind kind);

/// Parameter keys of @p params that carry modifiers.
[[nodiscard]] QStringList modifiedKeys(const ParamSet& params);

/// Seeds the Path modifier of an XY Path: its curve starts as the parameter's value at
/// @p frame, with a key there.
void seedPath(ParamSet& params, const QString& key, const QString& id, int64_t frame);

} // namespace kk::modifiers
