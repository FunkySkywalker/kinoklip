// SPDX-License-Identifier: GPL-3.0-or-later
// CompInspector: parameter modifiers (spec 119): the right-click menu of a number row (Modify
// With, Connect To, Publish, Remove), formulas typed into value fields, and the Modifiers
// sections that show the controls of every modifier of the node.
#include "core/CompLinks.h"
#include "core/Expression.h"
#include "core/Modifiers.h"
#include "core/NodeRegistry.h"
#include "ui/comp/CompAdapter.h"
#include "ui/comp/CompInspector.h"
#include "ui/comp/CompSession.h"
#include "ui/common/Metrics.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ParamRows.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QCursor>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

namespace kk {

namespace {

const CompNode* shown(const CompSession* s, quint64 id)
{
    return s ? s->composition().node(id) : nullptr;
}

void say(const QString& text)
{
    QToolTip::showText(QCursor::pos(), text);
}

QString linkProblem(complinks::LinkError e)
{
    switch (e) {
    case complinks::LinkError::ToItself:
        return QCoreApplication::translate("CompInspector", "A parameter cannot be connected to itself");
    case complinks::LinkError::Cycle:
        return QCoreApplication::translate(
            "CompInspector", "That would make a circle: the other parameter already reads this one");
    case complinks::LinkError::NotNumber:
        return QCoreApplication::translate("CompInspector", "Only number parameters can be connected");
    default:
        return QCoreApplication::translate("CompInspector", "That parameter no longer exists");
    }
}

} // namespace

QString CompInspector::partnerKey(const QString& key) const
{
    const CompNode* n = shown(m_session, m_node);
    return n ? complinks::pointPartner(*n, key) : QString();
}

QString CompInspector::paramExpression(const ParamRef& ref) const
{
    const CompNode* n = shown(m_session, m_node);
    if (!n)
        return {};
    const ParamModifier* m = modifiers::first(n->params, ref.key, ModifierKind::Expression);
    return m ? m->text : QString();
}

QString CompInspector::paramModifierNote(const ParamRef& ref) const
{
    const CompNode* n = shown(m_session, m_node);
    if (!n)
        return {};
    const auto it = n->params.values.constFind(ref.key);
    if (it == n->params.values.cend())
        return {};
    QStringList parts;
    for (const ParamModifier& m : it->modifiers) {
        if (m.kind == ModifierKind::Publish)
            continue;
        if (m.kind == ModifierKind::Connect) {
            const CompNode* t = m_session->composition().node(m.targetNode);
            parts.append(tr("Connected to %1").arg(t ? t->name + QStringLiteral(" > ") + complinks::paramLabel(*t, m.targetKey) : tr("(missing)")));
        } else if (m.kind == ModifierKind::Expression) {
            parts.append(tr("Expression: %1").arg(m.text));
        } else {
            parts.append(modifiers::kindName(m.kind));
        }
    }
    return parts.join(QStringLiteral("\n"));
}

void CompInspector::setParamExpression(
    const ParamRef& ref, const ParamDescriptor&, const QString& text)
{
    if (!m_session || !m_node)
        return;
    const CompNode* n = shown(m_session, m_node);
    if (!n || n->locked)
        return;
    const QString trimmed = text.trimmed();
    if (!trimmed.isEmpty()) {
        QString error;
        if (!expr::Program::compile(trimmed, &error)) {
            say(error);
            return;
        }
    }
    const quint64 id = m_node;
    const QString key = ref.key;
    bool circular = false;
    const bool done = m_session->edit(tr("Expression"), [&](Composition& c) {
        CompNode* node = c.node(id);
        if (!node)
            return false;
        const ParamModifier* old = modifiers::first(node->params, key, ModifierKind::Expression);
        if (trimmed.isEmpty()) {
            if (!old)
                return false;
            const QString oldId = old->id;
            return modifiers::remove(node->params, key, oldId);
        }
        Composition trial = c;
        CompNode* tn = trial.node(id);
        QString mid = old ? old->id : modifiers::add(tn->params, key, ModifierKind::Expression);
        modifiers::find(tn->params, key, mid)->text = trimmed;
        if (complinks::dependsOn(trial, {id, key}, {id, key})) {
            circular = true;
            return false;
        }
        if (!old)
            mid = modifiers::add(node->params, key, ModifierKind::Expression);
        else
            mid = old->id;
        modifiers::find(node->params, key, mid)->text = trimmed;
        return true;
    });
    if (circular)
        say(tr("That would make a circle: the formula reads a parameter that reads this one"));
    (void)done;
}

void CompInspector::fillParamMenu(const ParamRef& ref, const ParamDescriptor& desc, QMenu& menu)
{
    const CompNode* n = shown(m_session, m_node);
    if (!n || n->locked || !desc.isScalar() || modifiers::isControlKey(ref.key))
        return;
    menu.setToolTipsVisible(true);
    m_session->setActiveParam(n->id, ref.key);
    const quint64 id = n->id;
    const QString key = ref.key;
    const QString partner = partnerKey(key);

    // Modify With
    QMenu* modify = menu.addMenu(tr("Modify With"));
    modify->setToolTipsVisible(true);
    modify->menuAction()->setToolTip(tr("Changes the value over time or by a formula"));
    const auto addKind = [&](ModifierKind kind, const QString& text, bool) {
        QAction* a = modify->addAction(text);
        a->setToolTip(modifiers::kindDescription(kind));
        a->setObjectName(QStringLiteral("modify:") + modifierKindId(kind));
        connect(a, &QAction::triggered, this,
            [this, id, key, kind] { m_session->addModifier(id, key, kind); });
        return a;
    };
    addKind(ModifierKind::Perturb, tr("Perturb"), true);
    addKind(ModifierKind::Wiggle, tr("Wiggle"), true);
    addKind(ModifierKind::Offset, tr("Offset"), false);
    {
        QAction* a = addKind(ModifierKind::Expression, tr("Expression"), false);
        if (modifiers::first(n->params, key, ModifierKind::Expression))
            a->setEnabled(false);
    }
    addKind(ModifierKind::Cycle, tr("Cycle"), false);
    if (!partner.isEmpty()
        && !modifiers::first(n->params, key, ModifierKind::Path)) {
        addKind(ModifierKind::Path, tr("XY Path"), true);
    }

    // Connect To
    QMenu* connectMenu = menu.addMenu(tr("Connect To"));
    connectMenu->setToolTipsVisible(true);
    connectMenu->menuAction()->setToolTip(tr("Makes this value follow another node's parameter"));
    const Composition& comp = m_session->composition();
    const auto connectAction = [&](QMenu* into, const QString& text, quint64 node, const QString& tkey) {
        QAction* a = into->addAction(text);
        connect(a, &QAction::triggered, this, [this, id, key, node, tkey] {
            complinks::LinkError error = complinks::LinkError::None;
            m_session->edit(tr("Connect To"), [&](Composition& c) {
                error = complinks::connectParam(c, id, key, {node, tkey});
                return error == complinks::LinkError::None;
            });
            if (error != complinks::LinkError::None)
                say(linkProblem(error));
        });
    };
    const QVector<complinks::PublishedParam> pubs = complinks::published(comp);
    if (!pubs.isEmpty()) {
        QMenu* published = connectMenu->addMenu(tr("Published Values"));
        for (const complinks::PublishedParam& p : pubs) {
            if (p.node == id && p.key == key)
                continue;
            connectAction(published, p.label, p.node, p.key);
        }
        if (published->isEmpty())
            connectMenu->removeAction(published->menuAction());
    }
    for (const CompNode& other : comp.nodes) {
        const NodeDescriptor* d = NodeRegistry::instance().find(other.type);
        if (!d)
            continue;
        QMenu* sub = nullptr;
        for (const ParamDescriptor& p : d->params) {
            if (!p.isScalar() || p.hidden || (other.id == id && p.key == key))
                continue;
            if (!sub)
                sub = connectMenu->addMenu(other.name);
            connectAction(sub, p.label, other.id, p.key);
        }
    }
    if (connectMenu->isEmpty())
        connectMenu->setEnabled(false);

    menu.addSeparator();
    // Publish / Unpublish
    const ParamModifier* pub = modifiers::first(n->params, key, ModifierKind::Publish);
    QAction* publish = menu.addAction(pub ? tr("Unpublish") : tr("Publish"));
    publish->setToolTip(modifiers::kindDescription(ModifierKind::Publish));
    publish->setObjectName(QStringLiteral("modify:publish"));
    connect(publish, &QAction::triggered, this, [this, id, key, pub = pub != nullptr] {
        m_session->edit(pub ? tr("Unpublish") : tr("Publish"), [=](Composition& c) {
            CompNode* node = c.node(id);
            if (!node)
                return false;
            if (pub) {
                const ParamModifier* m = modifiers::first(node->params, key, ModifierKind::Publish);
                if (!m)
                    return false;
                const QString mid = m->id;
                return modifiers::remove(node->params, key, mid);
            }
            modifiers::add(node->params, key, ModifierKind::Publish);
            return true;
        });
    });
    // Remove / Disconnect
    for (const ParamModifier& m : n->params.values.value(key).modifiers) {
        if (m.kind == ModifierKind::Publish)
            continue;
        const QString mid = m.id;
        const bool isLink = m.kind == ModifierKind::Connect;
        QAction* a = menu.addAction(isLink ? tr("Disconnect") : tr("Remove %1").arg(modifiers::kindName(m.kind)));
        a->setToolTip(isLink ? tr("Cuts the link and keeps the value it showed")
                             : tr("Takes this modifier off the parameter"));
        a->setObjectName(QStringLiteral("remove:") + mid);
        connect(a, &QAction::triggered, this, [this, id, key, mid, isLink] {
            const int64_t frame = m_session->compFrame();
            m_session->edit(isLink ? tr("Disconnect") : tr("Remove Modifier"), [=](Composition& c) {
                if (isLink)
                    return complinks::disconnectParam(c, id, key, double(frame));
                CompNode* node = c.node(id);
                return node && modifiers::remove(node->params, key, mid);
            });
        });
    }
}

void CompInspector::buildModifiers(QVBoxLayout* layout, const CompNode& node)
{
    const compadapter::NodeTypeInfo* info = compadapter::nodeType(node.type);
    for (const QString& key : modifiers::modifiedKeys(node.params)) {
        const QString label = info ? complinks::paramLabel(node, key) : key;
        for (const ParamModifier& m : node.params.values.value(key).modifiers) {
            if (m.kind == ModifierKind::Publish)
                continue;
            auto* box = new ParamGroupBox(tr("%1: %2").arg(label, modifiers::kindName(m.kind)));
            box->setObjectName(QStringLiteral("CompModifier:") + m.id);
            const QString mid = m.id;
            const quint64 nodeId = node.id;
            const int64_t unused = 0;
            (void)unused;
            // Remove (and Disconnect for a link) in the header.
            QToolButton* remove = box->addHeaderButton(InspectorGlyph::Remove,
                m.kind == ModifierKind::Connect ? tr("Disconnect: cut the link and keep the value it showed")
                                                : tr("Remove this modifier"));
            remove->setObjectName(QStringLiteral("CompModifierRemove:") + mid);
            connect(remove, &QToolButton::clicked, this, [this, nodeId, key, mid, k = m.kind] {
                const int64_t frame = m_session->compFrame();
                m_session->edit(tr("Remove Modifier"), [=](Composition& c) {
                    if (k == ModifierKind::Connect)
                        return complinks::disconnectParam(c, nodeId, key, double(frame));
                    CompNode* n = c.node(nodeId);
                    return n && modifiers::remove(n->params, key, mid);
                });
            });
            QCheckBox* on = box->addEnableToggle();
            on->setObjectName(QStringLiteral("CompModifierEnable:") + mid);
            on->setToolTip(tr("Turns this modifier on or off without removing it"));
            on->setChecked(m.enabled);
            connect(on, &QCheckBox::clicked, this, [this, nodeId, key, mid](bool enabled) {
                m_session->edit(tr("Modifier On / Off"), [=](Composition& c) {
                    CompNode* n = c.node(nodeId);
                    ParamModifier* mod = n ? modifiers::find(n->params, key, mid) : nullptr;
                    if (!mod || mod->enabled == enabled)
                        return false;
                    mod->enabled = enabled;
                    return true;
                });
            });
            if (m.kind == ModifierKind::Expression) {
                auto* edit = new QLineEdit(m.text);
                edit->setObjectName(QStringLiteral("CompModifierFormula:") + mid);
                edit->setPlaceholderText(tr("for example  sin(time / 12) * 40 + n1"));
                edit->setToolTip(tr("The formula. Variables: time, value (the value without the "
                                    "expression), n1 to n3, pi, e. Other parameters: Node.Parameter "
                                    "(Blur1.Size) or a parameter of this node by name. Functions: %1.")
                                     .arg(expr::Program::functionNames().join(QStringLiteral(", "))));
                box->addWidgetRow(tr("Formula"), edit);
                connect(edit, &QLineEdit::editingFinished, this, [this, edit, key, mid] {
                    ParamRef r;
                    r.key = key;
                    const CompNode* n = shown(m_session, m_node);
                    const ParamModifier* mod = n ? modifiers::find(n->params, key, mid) : nullptr;
                    if (mod && mod->text == edit->text().trimmed())
                        return;
                    setParamExpression(r, ParamDescriptor {}, edit->text());
                });
            } else if (m.kind == ModifierKind::Connect) {
                const CompNode* t = m_session ? m_session->composition().node(m.targetNode) : nullptr;
                auto* text = new QLabel(t ? t->name + QStringLiteral(" > ") + complinks::paramLabel(*t, m.targetKey)
                                          : tr("(missing)"));
                text->setObjectName(QStringLiteral("CompModifierTarget:") + mid);
                text->setToolTip(tr("The parameter this value follows"));
                box->addWidgetRow(tr("Connected To"), text);
            }
            for (const modifiers::Control& c : modifiers::controls(m.kind)) {
                ParamDescriptor d;
                d.key = modifiers::controlKey(m.id, c.name);
                d.label = c.label;
                d.min = c.min;
                d.max = c.max;
                d.def = c.def;
                d.step = c.step;
                d.unit = c.unit;
                if (!c.choices.isEmpty()) {
                    d.type = ParamType::Choice;
                    d.choices = c.choices;
                    d.choiceLabels = c.choiceLabels;
                    d.defStatic = c.defChoice;
                } else {
                    d.type = c.isInt ? ParamType::Int : ParamType::Double;
                }
                ParamRef ref;
                ref.clipId = kProxyClipId;
                ref.scope = ParamScope::Effect;
                ref.effectId = node.id;
                ref.key = d.key;
                if (ParamRow* row = box->addParam(ref, d, *this)) {
                    for (QWidget* w : row->vimWidgets())
                        w->setToolTip(c.tooltip);
                }
            }
            layout->addWidget(box);
            m_modifierBoxes.append(box);
        }
    }
}

} // namespace kk
