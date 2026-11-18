// SPDX-License-Identifier: GPL-3.0-or-later
// CompGroups — Group, Macro and Instance nodes of a composition (spec 120). A group is a
// container: its members stay ordinary nodes of the same flat graph (CompNode::group), so
// evaluation, rendering, links and undo are exactly those of the nodes alone; the group only
// decides what the node editor shows (a frame around the members, or one tile with the members
// hidden) and what moves together. A macro is a group with published controls: the control
// is a number on the macro node and the inner parameter is connected to it (Connect To).
// An instance is a node whose parameters are those of another node of the same type.
#pragma once

#include "core/Composition.h"
#include "core/EffectRegistry.h"

#include <QRectF>
#include <QString>
#include <QVector>

namespace kk::compgroups {

/// True if @p node is a Group or a Macro (the "group" node type).
[[nodiscard]] bool isGroup(const CompNode& node);

/// The nodes inside group @p group (direct members; with @p recursive also those of groups
/// inside it). Group nodes inside are listed too.
[[nodiscard]] QVector<quint64> members(const Composition& comp, quint64 group, bool recursive = true);

/// @p ids plus the members (recursively) of every group among them: what a move, a copy or
/// a delete of those nodes acts on.
[[nodiscard]] QVector<quint64> withMembers(const Composition& comp, const QVector<quint64>& ids);

/// The outermost collapsed group that hides @p node, or nullptr when the node is visible.
[[nodiscard]] const CompNode* hiddenBy(const Composition& comp, const CompNode& node);

/// Groups the nodes @p ids (MediaOut, nodes that already sit in a group other than the common
/// one, and ids that do not exist are skipped; a group among them is taken whole). The new
/// Group node sits where the members' middle is, expanded, and is inside the members' common
/// group. Returns its id, 0 when no node can be grouped.
quint64 makeGroup(Composition& comp, const QVector<quint64>& ids);

/// Dissolves group @p group (or macro): its members move to the group's own group and the
/// group node goes. A macro's controls are cut first, every inner parameter keeping the value it
/// showed at @p frame. False for an unknown or non-group node.
bool ungroup(Composition& comp, quint64 group, int64_t frame);

/// Collapses or expands group @p group. Collapsing puts the group tile at the middle of its
/// members. False if the node is not a group or nothing changes.
bool setCollapsed(Composition& comp, quint64 group, bool collapsed);

/// The area (grid units) an expanded group's frame covers: its members' tiles with a margin and
/// room for the title on top. Empty for a group without members.
[[nodiscard]] QRectF frameOf(const Composition& comp, const CompNode& group);

/// The node of group @p group whose picture is the group's output: a member with an image output
/// that nothing inside the group reads, the right-most if several (nodes read by something
/// outside the group first). 0 when there is none.
[[nodiscard]] quint64 outputNode(const Composition& comp, quint64 group);

// ---- Macros ----

/// A parameter that could become a control of a macro.
struct ControlCandidate
{
    quint64 node = 0;
    QString key;
    QString label; ///< "Blur1 > Size"
    bool published = false; ///< has a Publish modifier (offered pre-ticked)
    bool taken = false; ///< already a control of this macro
};
/// Number parameters of the members of @p group (published ones first).
[[nodiscard]] QVector<ControlCandidate> controlCandidates(const Composition& comp, quint64 group);

/// One control to publish.
struct ControlPick
{
    quint64 node = 0;
    QString key;
    QString label; ///< empty: the parameter's label
};
/// Makes @p group a macro named @p name with a control for every pick: the control is a number
/// on the macro node (key "c1", "c2", ...) starting at the inner parameter's value at @p frame,
/// and the inner parameter is connected to it. Picks that are not numbers of a member, are
/// connected already, or repeat are skipped. False when no control results (the group stays
/// as it was). A macro that already has controls gets the new ones added.
bool makeMacro(Composition& comp, quint64 group, const QString& name,
    const QVector<ControlPick>& picks, int64_t frame);

/// Removes control @p key of macro @p macro: the inner parameter is cut loose and keeps the
/// value it showed at @p frame. False if there is no such control.
bool removeControl(Composition& comp, quint64 macro, const QString& key, int64_t frame);

/// The descriptors of a macro's controls for the Inspector: those of the inner parameters (range,
/// step, unit) under the control's key and label. Controls whose inner parameter is gone are
/// left out.
[[nodiscard]] QVector<ParamDescriptor> controlDescriptors(const Composition& comp, const CompNode& macro);

// ---- Instances ----

/// A new node at @p position of the same type as @p original, whose parameters, path and
/// source are those of the original: an edit of the original shows in every instance. An
/// instance of an instance is an instance of the original. 0 for a node that cannot be
/// instanced (MediaIn, MediaOut, groups).
quint64 makeInstance(Composition& comp, quint64 original, QPointF position);
/// The node whose parameters @p node uses (itself when it is not an instance).
[[nodiscard]] quint64 instanceSource(const Composition& comp, const CompNode& node);
/// Cuts the instance loose: it keeps a copy of what it showed. False if it is not an instance.
bool makeIndependent(Composition& comp, quint64 instance);
/// Copies the parameters of every instance's original into the instance (in @p comp, which must
/// be a copy for rendering: complinks::resolved() calls this).
void applyInstances(Composition& comp);
/// True if any node of @p comp is an instance.
[[nodiscard]] bool hasInstances(const Composition& comp);

} // namespace kk::compgroups
