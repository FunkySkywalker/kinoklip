// SPDX-License-Identifier: GPL-3.0-or-later
// CompLinks — parameter links of a composition (spec 119): Connect To, the references inside
// expressions, and Publish. A link reads the value of another node's parameter; the renderers do
// not follow links themselves: resolved() gives them a copy of the composition in which every
// link already holds its value for the frame.
#pragma once

#include "core/Composition.h"

#include <QString>
#include <QVector>

#include "core/Modifiers.h"

#include <QStringList>

#include <optional>

namespace kk::complinks {

/// One parameter of one node.
struct ParamTarget
{
    quint64 node = 0;
    QString key;
    friend bool operator==(const ParamTarget&, const ParamTarget&) = default;
};

/// True if any parameter of @p comp has a Connect To or an expression that reads another
/// parameter (a composition without links is rendered as it is, no copy).
[[nodiscard]] bool hasLinks(const Composition& comp);

/// A copy of @p comp in which the links are filled in for comp frame @p frame (ParamSet::bound).
/// Links that form a cycle, or that read something that no longer exists, are left unfilled: the
/// parameter then keeps its own value.
[[nodiscard]] Composition resolved(const Composition& comp, double frame);

/// What the reference @p ref written in a formula of node @p self means: "Blur1.XBlurSize" (node
/// name, then the parameter's key or label without spaces and dots, in any case) or "size" (a
/// parameter of @p self itself). nullopt when there is no such node or scalar parameter.
[[nodiscard]] std::optional<ParamTarget> findReference(
    const Composition& comp, const CompNode& self, const QString& ref);

/// Why a link cannot be made.
enum class LinkError { None, Missing, ToItself, Cycle, NotNumber };

/// True if reading @p from needs the value of @p to (through Connect To or expressions).
[[nodiscard]] bool dependsOn(const Composition& comp, const ParamTarget& from, const ParamTarget& to);

/// Connect To: makes parameter (@p node, @p key) follow (@p target). Replaces an earlier Connect
/// To of the same parameter. Changes nothing and says why when it cannot be done.
[[nodiscard]] LinkError connectParam(
    Composition& comp, quint64 node, const QString& key, const ParamTarget& target);
/// Removes the Connect To of (@p node, @p key); the parameter keeps what it showed at @p frame as
/// its constant value (like Fusion's Disconnect, which leaves the current value).
bool disconnectParam(Composition& comp, quint64 node, const QString& key, double frame);

/// A parameter offered by a Publish modifier.
struct PublishedParam
{
    quint64 node = 0;
    QString key;
    QString label; ///< "Transform1 > Size"
};
/// Every published parameter of @p comp, in node order.
[[nodiscard]] QVector<PublishedParam> published(const Composition& comp);

/// The other half of a point: "centerY" for "centerX" and back, when @p node has both as number
/// parameters; empty otherwise.
[[nodiscard]] QString pointPartner(const CompNode& node, const QString& key);

/// Adds a modifier of @p kind to parameter @p key of @p node; Perturb, Wiggle and XY Path act on
/// both halves of a point (X and Y). @p frame seeds an XY Path with the value there. Returns the
/// ids of the added modifiers (empty when the node or the key does not exist, or an Expression
/// is already there).
QStringList addModifier(Composition& comp, quint64 node, const QString& key, ModifierKind kind, int64_t frame);

/// Translated label of parameter @p key of @p node ("Size"); the key itself when it has no
/// descriptor.
[[nodiscard]] QString paramLabel(const CompNode& node, const QString& key);

} // namespace kk::complinks
