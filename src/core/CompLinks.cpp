// SPDX-License-Identifier: GPL-3.0-or-later
// Parameter links: resolving, cycle checks and Connect To. See CompLinks.h.
#include "core/CompLinks.h"

#include "core/CompGroups.h"
#include "core/Expression.h"
#include "core/Modifiers.h"
#include "core/NodeRegistry.h"

#include <QSet>

#include <algorithm>
#include <functional>

namespace kk::complinks {

namespace {

QString squeezed(const QString& s)
{
    QString out;
    for (const QChar c : s) {
        if (!c.isSpace() && c != QLatin1Char('.'))
            out.append(c.toLower());
    }
    return out;
}

const CompNode* nodeByName(const Composition& comp, const QString& name)
{
    for (const CompNode& n : comp.nodes) {
        if (n.name.compare(name, Qt::CaseInsensitive) == 0)
            return &n;
    }
    return nullptr;
}

/// The references of a modifier as ParamTargets (unknown ones are skipped).
QVector<std::pair<QString, ParamTarget>> referencesOf(
    const Composition& comp, const CompNode& node, const ParamModifier& m)
{
    QVector<std::pair<QString, ParamTarget>> out;
    if (!m.enabled)
        return out;
    if (m.kind == ModifierKind::Connect) {
        if (m.targetNode != 0)
            out.append({QString(), ParamTarget {m.targetNode, m.targetKey}});
    } else if (m.kind == ModifierKind::Expression) {
        if (const auto program = expr::Program::compile(m.text)) {
            for (const QString& ref : program->references()) {
                if (const auto t = findReference(comp, node, ref))
                    out.append({ref, *t});
            }
        }
    }
    return out;
}

struct Resolver
{
    Composition& comp;
    double frame;
    QSet<QString> done, active; // "node:key"

    void resolve(CompNode& node, const QString& key)
    {
        const QString token = QString::number(node.id) + QLatin1Char(':') + key;
        if (done.contains(token) || active.contains(token))
            return;
        const auto it = node.params.values.constFind(key);
        if (it == node.params.values.cend() || it->modifiers.isEmpty()) {
            done.insert(token);
            return;
        }
        active.insert(token);
        const QVector<ParamModifier> mods = it->modifiers;
        for (const ParamModifier& m : mods) {
            for (const auto& [ref, target] : referencesOf(comp, node, m)) {
                CompNode* other = comp.node(target.node);
                if (!other)
                    continue;
                const QString otherToken
                    = QString::number(other->id) + QLatin1Char(':') + target.key;
                if (active.contains(otherToken))
                    continue; // a cycle: this reference stays unfilled
                resolve(*other, target.key);
                const double v = other->params.valueAtTime(target.key, frame, 0.0);
                const QString bkey = ref.isEmpty() ? m.id : m.id + QLatin1Char('/') + ref;
                node.params.bound.values.insert(bkey, v);
            }
        }
        active.remove(token);
        done.insert(token);
    }
};

} // namespace

QString paramLabel(const CompNode& node, const QString& key)
{
    for (const MacroControl& c : node.controls) {
        if (c.key == key)
            return c.label;
    }
    if (const NodeDescriptor* d = NodeRegistry::instance().find(node.type)) {
        if (const ParamDescriptor* p = d->param(key))
            return p->label;
    }
    return key;
}

QString pointPartner(const CompNode& node, const QString& key)
{
    const NodeDescriptor* d = NodeRegistry::instance().find(node.type);
    if (!d || key.size() < 2)
        return {};
    const QChar last = key.back();
    if (last != QLatin1Char('X') && last != QLatin1Char('Y'))
        return {};
    const QString other = key.left(key.size() - 1) + (last == QLatin1Char('X') ? QLatin1Char('Y') : QLatin1Char('X'));
    const ParamDescriptor* p = d->param(other);
    const ParamDescriptor* self = d->param(key);
    return p && p->isScalar() && self && self->isScalar() ? other : QString();
}

QStringList addModifier(Composition& comp, quint64 node, const QString& key, ModifierKind kind, int64_t frame)
{
    CompNode* n = comp.node(node);
    if (!n || key.isEmpty() || modifiers::isControlKey(key))
        return {};
    if (kind == ModifierKind::Expression && modifiers::first(n->params, key, kind))
        return {};
    QStringList keys {key};
    const bool pair = kind == ModifierKind::Perturb || kind == ModifierKind::Wiggle
        || kind == ModifierKind::Path;
    const QString partner = pair ? pointPartner(*n, key) : QString();
    if (!partner.isEmpty())
        keys.append(partner);
    QStringList ids;
    for (const QString& k : keys) {
        if (kind == ModifierKind::Path && modifiers::first(n->params, k, kind))
            continue;
        const QString id = modifiers::add(n->params, k, kind);
        if (kind == ModifierKind::Path)
            modifiers::seedPath(n->params, k, id, frame);
        ids.append(id);
    }
    return ids;
}

std::optional<ParamTarget> findReference(
    const Composition& comp, const CompNode& self, const QString& ref)
{
    const CompNode* node = &self;
    QString param = ref;
    const qsizetype dot = ref.indexOf(QLatin1Char('.'));
    if (dot > 0) {
        node = nodeByName(comp, ref.left(dot));
        param = ref.mid(dot + 1);
    }
    if (!node)
        return std::nullopt;
    const QString want = squeezed(param);
    const NodeDescriptor* d = NodeRegistry::instance().find(node->type);
    if (d) {
        for (const ParamDescriptor& p : d->params) {
            if (!p.isScalar())
                continue;
            if (squeezed(p.key) == want || squeezed(p.label) == want)
                return ParamTarget {node->id, p.key};
        }
    }
    for (const MacroControl& c : node->controls) {
        if (squeezed(c.label) == want || squeezed(c.key) == want)
            return ParamTarget {node->id, c.key};
    }
    for (auto it = node->params.values.cbegin(); it != node->params.values.cend(); ++it) {
        if (!modifiers::isControlKey(it.key()) && squeezed(it.key()) == want)
            return ParamTarget {node->id, it.key()};
    }
    return std::nullopt;
}

bool hasLinks(const Composition& comp)
{
    if (compgroups::hasInstances(comp))
        return true;
    for (const CompNode& n : comp.nodes) {
        for (auto it = n.params.values.cbegin(); it != n.params.values.cend(); ++it) {
            for (const ParamModifier& m : it->modifiers) {
                if (!m.enabled)
                    continue;
                if (m.kind == ModifierKind::Connect)
                    return true;
                if (m.kind == ModifierKind::Expression) {
                    const auto program = expr::Program::compile(m.text);
                    if (program && !program->references().isEmpty())
                        return true;
                }
            }
        }
    }
    return false;
}

Composition resolved(const Composition& comp, double frame)
{
    Composition out = comp;
    compgroups::applyInstances(out);
    Resolver r {out, frame, {}, {}};
    for (CompNode& n : out.nodes) {
        for (const QString& key : modifiers::modifiedKeys(n.params))
            r.resolve(n, key);
    }
    return out;
}

bool dependsOn(const Composition& comp, const ParamTarget& from, const ParamTarget& to)
{
    QSet<QString> seen;
    const std::function<bool(const ParamTarget&)> visit = [&](const ParamTarget& t) {
        if (t == to)
            return true;
        const QString token = QString::number(t.node) + QLatin1Char(':') + t.key;
        if (seen.contains(token))
            return false;
        seen.insert(token);
        const CompNode* n = comp.node(t.node);
        if (!n)
            return false;
        const auto it = n->params.values.constFind(t.key);
        if (it == n->params.values.cend())
            return false;
        for (const ParamModifier& m : it->modifiers) {
            for (const auto& ref : referencesOf(comp, *n, m)) {
                if (visit(ref.second))
                    return true;
            }
        }
        return false;
    };
    // `from` itself is not the answer: ask about what it reads.
    const CompNode* n = comp.node(from.node);
    if (!n)
        return false;
    const auto it = n->params.values.constFind(from.key);
    if (it == n->params.values.cend())
        return false;
    seen.insert(QString::number(from.node) + QLatin1Char(':') + from.key);
    for (const ParamModifier& m : it->modifiers) {
        for (const auto& ref : referencesOf(comp, *n, m)) {
            if (visit(ref.second))
                return true;
        }
    }
    return false;
}

LinkError connectParam(
    Composition& comp, quint64 node, const QString& key, const ParamTarget& target)
{
    CompNode* n = comp.node(node);
    const CompNode* t = comp.node(target.node);
    if (!n || !t)
        return LinkError::Missing;
    if (node == target.node && key == target.key)
        return LinkError::ToItself;
    const NodeDescriptor* td = NodeRegistry::instance().find(t->type);
    const ParamDescriptor* tp = td ? td->param(target.key) : nullptr;
    const bool macroControl = t->macro
        && std::any_of(t->controls.cbegin(), t->controls.cend(),
            [&](const MacroControl& c) { return c.key == target.key; });
    if (!macroControl && (!tp || !tp->isScalar()))
        return LinkError::NotNumber;
    if (dependsOn(comp, target, ParamTarget {node, key}))
        return LinkError::Cycle;
    // Replace an earlier Connect To.
    if (const ParamModifier* old = modifiers::first(n->params, key, ModifierKind::Connect)) {
        const QString id = old->id;
        modifiers::remove(n->params, key, id);
    }
    const QString id = modifiers::add(n->params, key, ModifierKind::Connect);
    ParamModifier* m = modifiers::find(n->params, key, id);
    m->targetNode = target.node;
    m->targetKey = target.key;
    return LinkError::None;
}

bool disconnectParam(Composition& comp, quint64 node, const QString& key, double frame)
{
    CompNode* n = comp.node(node);
    if (!n)
        return false;
    const ParamModifier* old = modifiers::first(n->params, key, ModifierKind::Connect);
    if (!old)
        return false;
    const QString id = old->id;
    const Composition shown = resolved(comp, frame);
    const double value = shown.node(node) ? shown.node(node)->params.valueAtTime(key, frame, 0.0) : 0.0;
    modifiers::remove(n->params, key, id);
    // Constant value: what it showed, no keys left under it.
    AnimatedValue& av = n->params.animated(key, value);
    av.keys.clear();
    av.base = value;
    return true;
}

QVector<PublishedParam> published(const Composition& comp)
{
    QVector<PublishedParam> out;
    for (const CompNode& n : comp.nodes) {
        for (auto it = n.params.values.cbegin(); it != n.params.values.cend(); ++it) {
            if (modifiers::first(n.params, it.key(), ModifierKind::Publish))
                out.append({n.id, it.key(), n.name + QStringLiteral(" > ") + paramLabel(n, it.key())});
        }
    }
    return out;
}

} // namespace kk::complinks
