// SPDX-License-Identifier: GPL-3.0-or-later
// Group, Macro and Instance operations, see CompGroups.h.
#include "core/CompGroups.h"

#include "core/CompLinks.h"
#include "core/CompOps.h"
#include "core/Modifiers.h"
#include "core/NodeRegistry.h"

#include <QSet>

#include <algorithm>

namespace kk::compgroups {

namespace {

constexpr double kFrameMargin = 0.5; ///< grid units around the members
constexpr double kFrameTitle = 0.9; ///< grid units of title above them

const ParamDescriptor* descriptorOf(const CompNode& node, const QString& key)
{
    const NodeDescriptor* d = NodeRegistry::instance().find(node.type);
    return d ? d->param(key) : nullptr;
}

/// A name without its trailing number ("Blur2" -> "Blur").
QString baseOf(const QString& name)
{
    qsizetype n = name.size();
    while (n > 1 && name.at(n - 1).isDigit())
        --n;
    return name.left(n);
}

bool isMember(const Composition& comp, const CompNode& n, quint64 group)
{
    for (quint64 g = n.group; g != 0;) {
        if (g == group)
            return true;
        const CompNode* parent = comp.node(g);
        g = parent ? parent->group : 0;
    }
    return false;
}

} // namespace

bool isGroup(const CompNode& node)
{
    return node.type == nodetypes::Group;
}

QVector<quint64> members(const Composition& comp, quint64 group, bool recursive)
{
    QVector<quint64> out;
    for (const CompNode& n : comp.nodes) {
        if (n.id == group)
            continue;
        if (recursive ? isMember(comp, n, group) : n.group == group)
            out.append(n.id);
    }
    return out;
}

QVector<quint64> withMembers(const Composition& comp, const QVector<quint64>& ids)
{
    QVector<quint64> out = ids;
    QSet<quint64> seen(ids.cbegin(), ids.cend());
    for (const quint64 id : ids) {
        const CompNode* n = comp.node(id);
        if (!n || !isGroup(*n))
            continue;
        for (const quint64 m : members(comp, id, true)) {
            if (!seen.contains(m)) {
                seen.insert(m);
                out.append(m);
            }
        }
    }
    return out;
}

const CompNode* hiddenBy(const Composition& comp, const CompNode& node)
{
    const CompNode* hidden = nullptr;
    for (quint64 g = node.group; g != 0;) {
        const CompNode* parent = comp.node(g);
        if (!parent)
            break;
        if (parent->collapsed)
            hidden = parent; // keep walking up: the outermost collapsed group wins
        g = parent->group;
    }
    return hidden;
}

quint64 makeGroup(Composition& comp, const QVector<quint64>& ids)
{
    // Members: existing nodes except MediaOut and groups' own members (taken with their group).
    QVector<quint64> picked;
    QSet<quint64> set;
    for (const quint64 id : ids) {
        const CompNode* n = comp.node(id);
        if (n && id != comp.mediaOutId && n->type != nodetypes::MediaOut && !set.contains(id)) {
            picked.append(id);
            set.insert(id);
        }
    }
    // The members' common group: the group of the first one; nodes elsewhere are skipped.
    // A node whose group is itself among the picked ones is not a direct member.
    QVector<quint64> direct;
    quint64 parent = 0;
    bool first = true;
    for (const quint64 id : picked) {
        const CompNode* n = comp.node(id);
        if (set.contains(n->group))
            continue; // comes with its group
        if (first) {
            parent = n->group;
            first = false;
        }
        if (n->group == parent)
            direct.append(id);
    }
    if (direct.isEmpty())
        return 0;
    QPointF middle;
    for (const quint64 id : direct)
        middle += comp.node(id)->position;
    middle /= double(direct.size());
    CompNode g;
    g.id = comp.allocateId();
    g.type = nodetypes::Group;
    g.name = compops::uniqueName(comp, QStringLiteral("Group"));
    g.position = middle;
    g.group = parent;
    const quint64 id = g.id;
    comp.nodes.append(g);
    for (const quint64 m : direct)
        comp.node(m)->group = id;
    return id;
}

bool ungroup(Composition& comp, quint64 group, int64_t frame)
{
    CompNode* g = comp.node(group);
    if (!g || !isGroup(*g))
        return false;
    if (g->macro) {
        for (const MacroControl& c : QVector<MacroControl>(g->controls))
            removeControl(comp, group, c.key, frame);
        g = comp.node(group);
    }
    const quint64 parent = g->group;
    for (CompNode& n : comp.nodes) {
        if (n.group == group)
            n.group = parent;
    }
    comp.nodes.removeIf([group](const CompNode& n) { return n.id == group; });
    for (CompNode& n : comp.nodes) {
        if (n.instanceOf == group)
            n.instanceOf = 0;
    }
    if (comp.leftViewerNode == group)
        comp.leftViewerNode = 0;
    if (comp.rightViewerNode == group)
        comp.rightViewerNode = 0;
    return true;
}

bool setCollapsed(Composition& comp, quint64 group, bool collapsed)
{
    CompNode* g = comp.node(group);
    if (!g || !isGroup(*g) || g->collapsed == collapsed)
        return false;
    g->collapsed = collapsed;
    if (collapsed) {
        const QVector<quint64> ms = members(comp, group, true);
        if (!ms.isEmpty()) {
            QRectF bounds;
            for (const quint64 id : ms)
                bounds |= QRectF(comp.node(id)->position, QSizeF(0.01, 0.01));
            g = comp.node(group);
            g->position = bounds.center();
        }
    }
    return true;
}

QRectF frameOf(const Composition& comp, const CompNode& group)
{
    QRectF bounds;
    for (const CompNode& n : comp.nodes) {
        if (n.group != group.id || n.id == group.id)
            continue;
        // A collapsed group inside draws as one tile.
        bounds |= QRectF(n.position, QSizeF(3.4, 1.0));
    }
    if (bounds.isNull())
        return {};
    return bounds.adjusted(-kFrameMargin, -kFrameMargin - kFrameTitle, kFrameMargin, kFrameMargin);
}

quint64 outputNode(const Composition& comp, quint64 group)
{
    const QVector<quint64> ms = members(comp, group, true);
    const QSet<quint64> inside(ms.cbegin(), ms.cend());
    quint64 best = 0;
    bool bestOutside = false;
    double bestX = -1e18;
    for (const quint64 id : ms) {
        const CompNode* n = comp.node(id);
        const NodeDescriptor* d = n ? NodeRegistry::instance().find(n->type) : nullptr;
        if (!d || d->outputs == 0 || isGroup(*n))
            continue;
        bool readInside = false, readOutside = false;
        for (const CompNode& other : comp.nodes) {
            for (const NodeLink& l : other.inputs) {
                if (l.nodeId != id)
                    continue;
                (inside.contains(other.id) ? readInside : readOutside) = true;
            }
        }
        if (readInside && !readOutside)
            continue;
        const bool better = (readOutside && !bestOutside)
            || (readOutside == bestOutside && n->position.x() > bestX);
        if (!best || better) {
            best = id;
            bestOutside = readOutside;
            bestX = n->position.x();
        }
    }
    return best;
}

// ---- Macros ----

QVector<ControlCandidate> controlCandidates(const Composition& comp, quint64 group)
{
    const CompNode* macro = comp.node(group);
    QVector<ControlCandidate> out;
    if (!macro || !isGroup(*macro))
        return out;
    for (const quint64 id : members(comp, group, true)) {
        const CompNode* n = comp.node(id);
        const NodeDescriptor* d = NodeRegistry::instance().find(n->type);
        if (!d || isGroup(*n))
            continue;
        for (const ParamDescriptor& p : d->params) {
            if (!p.isScalar() || p.hidden)
                continue;
            ControlCandidate c;
            c.node = id;
            c.key = p.key;
            c.label = n->name + QStringLiteral(" > ") + p.label;
            c.published = modifiers::first(n->params, p.key, ModifierKind::Publish) != nullptr;
            c.taken = std::any_of(macro->controls.cbegin(), macro->controls.cend(),
                [&](const MacroControl& m) { return m.node == id && m.paramKey == p.key; });
            out.append(c);
        }
    }
    std::stable_sort(out.begin(), out.end(),
        [](const ControlCandidate& a, const ControlCandidate& b) { return a.published && !b.published; });
    return out;
}

bool makeMacro(Composition& comp, quint64 group, const QString& name,
    const QVector<ControlPick>& picks, int64_t frame)
{
    CompNode* macro = comp.node(group);
    if (!macro || !isGroup(*macro))
        return false;
    const QVector<quint64> ms = members(comp, group, true);
    const QSet<quint64> inside(ms.cbegin(), ms.cend());
    int next = 1;
    for (const MacroControl& c : macro->controls)
        next = std::max(next, c.key.mid(1).toInt() + 1);
    int added = 0;
    QSet<QString> seen;
    for (const ControlPick& pick : picks) {
        CompNode* inner = comp.node(pick.node);
        const ParamDescriptor* pd = inner ? descriptorOf(*inner, pick.key) : nullptr;
        const QString token = QString::number(pick.node) + QLatin1Char(':') + pick.key;
        if (!inner || !pd || !pd->isScalar() || !inside.contains(pick.node) || seen.contains(token)
            || modifiers::first(inner->params, pick.key, ModifierKind::Connect))
            continue;
        seen.insert(token);
        macro = comp.node(group);
        MacroControl c;
        c.key = QStringLiteral("c%1").arg(next++);
        c.label = pick.label.trimmed().isEmpty() ? pd->label : pick.label.trimmed();
        c.node = pick.node;
        c.paramKey = pick.key;
        const double value = inner->params.valueAtTime(pick.key, double(frame), pd->def);
        macro->params.setBase(c.key, value);
        // The inner parameter follows the control. Connect To is added directly: the target is
        // a macro control, which has no node descriptor.
        const QString id = modifiers::add(inner->params, pick.key, ModifierKind::Connect);
        ParamModifier* m = modifiers::find(inner->params, pick.key, id);
        m->targetNode = group;
        m->targetKey = c.key;
        macro->controls.append(c);
        ++added;
    }
    if (added == 0)
        return false;
    macro = comp.node(group);
    macro->macro = true;
    if (!name.trimmed().isEmpty())
        compops::renameNode(comp, group, name); // kept as it was when the name is taken
    return true;
}

bool removeControl(Composition& comp, quint64 macroId, const QString& key, int64_t frame)
{
    CompNode* macro = comp.node(macroId);
    if (!macro)
        return false;
    const auto it = std::find_if(macro->controls.begin(), macro->controls.end(),
        [&](const MacroControl& c) { return c.key == key; });
    if (it == macro->controls.end())
        return false;
    const MacroControl c = *it;
    // The inner parameter keeps what it showed.
    if (CompNode* inner = comp.node(c.node)) {
        if (const ParamModifier* link = modifiers::first(inner->params, c.paramKey, ModifierKind::Connect)) {
            if (link->targetNode == macroId && link->targetKey == key)
                complinks::disconnectParam(comp, c.node, c.paramKey, double(frame));
        }
    }
    macro = comp.node(macroId);
    macro->controls.removeIf([&](const MacroControl& m) { return m.key == key; });
    macro->params.values.remove(key);
    if (macro->controls.isEmpty())
        macro->macro = false;
    return true;
}

QVector<ParamDescriptor> controlDescriptors(const Composition& comp, const CompNode& macro)
{
    QVector<ParamDescriptor> out;
    for (const MacroControl& c : macro.controls) {
        const CompNode* inner = comp.node(c.node);
        const ParamDescriptor* pd = inner ? descriptorOf(*inner, c.paramKey) : nullptr;
        if (!pd)
            continue;
        ParamDescriptor d = *pd;
        d.key = c.key;
        d.label = c.label;
        out.append(d);
    }
    return out;
}

// ---- Instances ----

quint64 instanceSource(const Composition& comp, const CompNode& node)
{
    quint64 id = node.id;
    const CompNode* n = &node;
    for (int guard = 0; n->instanceOf != 0 && guard < 64; ++guard) {
        const CompNode* src = comp.node(n->instanceOf);
        if (!src)
            break;
        id = src->id;
        n = src;
    }
    return id;
}

quint64 makeInstance(Composition& comp, quint64 original, QPointF position)
{
    const CompNode* o = comp.node(original);
    if (!o || o->type == nodetypes::MediaIn || o->type == nodetypes::MediaOut || isGroup(*o))
        return 0;
    const quint64 source = instanceSource(comp, *o);
    const CompNode& s = *comp.node(source);
    CompNode n = s;
    n.id = comp.allocateId();
    n.name = compops::uniqueName(comp, baseOf(s.name));
    n.position = position;
    n.inputs.clear();
    n.group = o->group;
    n.collapsed = false;
    n.instanceOf = source;
    n.locked = false;
    const quint64 id = n.id;
    comp.nodes.append(std::move(n));
    return id;
}

bool makeIndependent(Composition& comp, quint64 instance)
{
    CompNode* n = comp.node(instance);
    if (!n || n->instanceOf == 0)
        return false;
    const CompNode* src = comp.node(n->instanceOf);
    if (src) {
        n->params = src->params;
        n->path = src->path;
        n->mediaId = src->mediaId;
    }
    n->instanceOf = 0;
    return true;
}

bool hasInstances(const Composition& comp)
{
    return std::any_of(comp.nodes.cbegin(), comp.nodes.cend(),
        [](const CompNode& n) { return n.instanceOf != 0; });
}

void applyInstances(Composition& comp)
{
    for (CompNode& n : comp.nodes) {
        if (n.instanceOf == 0)
            continue;
        const quint64 sourceId = instanceSource(comp, n);
        if (sourceId == n.id)
            continue;
        const CompNode* src = comp.node(sourceId);
        if (!src || src->type != n.type)
            continue;
        n.params = src->params;
        n.path = src->path;
        n.mediaId = src->mediaId;
    }
}

} // namespace kk::compgroups
