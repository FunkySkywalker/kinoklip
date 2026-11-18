// SPDX-License-Identifier: GPL-3.0-or-later
// MakeMacroDialog implementation.
#include "ui/comp/MakeMacroDialog.h"

#include "core/NodeRegistry.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace kk {

namespace {
constexpr int kNodeRole = Qt::UserRole + 1;
constexpr int kKeyRole = Qt::UserRole + 2;
}

MakeMacroDialog::MakeMacroDialog(const Composition& comp, quint64 group, QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("MakeMacroDialog"));
    setWindowTitle(tr("Make Macro"));
    setModal(true);
    resize(420, 480);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Name of the macro")));
    m_name = new QLineEdit(comp.node(group) ? comp.node(group)->name : QString());
    m_name->setObjectName(QStringLiteral("MacroName"));
    m_name->setToolTip(tr("The macro's name. It is also the name of the title when you save it to the Effects Library."));
    layout->addWidget(m_name);
    layout->addWidget(new QLabel(tr("Controls: tick the parameters the macro shows")));
    m_tree = new QTreeWidget;
    m_tree->setObjectName(QStringLiteral("MacroControls"));
    m_tree->setHeaderHidden(true);
    m_tree->setToolTip(tr("Every ticked parameter becomes a control on the macro; the parameter "
                          "inside follows it. Published parameters are ticked for you."));
    layout->addWidget(m_tree, 1);
    QTreeWidgetItem* current = nullptr;
    quint64 currentNode = 0;
    for (const compgroups::ControlCandidate& c : compgroups::controlCandidates(comp, group)) {
        if (c.node != currentNode || !current) {
            const CompNode* n = comp.node(c.node);
            current = new QTreeWidgetItem(m_tree, {n ? n->name : QString()});
            current->setFlags(Qt::ItemIsEnabled);
            current->setExpanded(true);
            currentNode = c.node;
        }
        const CompNode* n = comp.node(c.node);
        const NodeDescriptor* d = n ? NodeRegistry::instance().find(n->type) : nullptr;
        const ParamDescriptor* p = d ? d->param(c.key) : nullptr;
        auto* item = new QTreeWidgetItem(current, {p ? p->label : c.key});
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(0, c.published || c.taken ? Qt::Checked : Qt::Unchecked);
        if (c.taken)
            item->setFlags(Qt::NoItemFlags); // already a control: shown, not changeable
        item->setData(0, kNodeRole, c.node);
        item->setData(0, kKeyRole, c.key);
    }
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QString MakeMacroDialog::macroName() const
{
    return m_name->text().trimmed();
}

QVector<compgroups::ControlPick> MakeMacroDialog::picks() const
{
    QVector<compgroups::ControlPick> out;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* node = m_tree->topLevelItem(i);
        for (int j = 0; j < node->childCount(); ++j) {
            const QTreeWidgetItem* item = node->child(j);
            if (item->checkState(0) == Qt::Checked && (item->flags() & Qt::ItemIsEnabled))
                out.append({item->data(0, kNodeRole).toULongLong(), item->data(0, kKeyRole).toString(), {}});
        }
    }
    return out;
}

bool MakeMacroDialog::setTicked(quint64 node, const QString& key, bool ticked)
{
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* parent = m_tree->topLevelItem(i);
        for (int j = 0; j < parent->childCount(); ++j) {
            QTreeWidgetItem* item = parent->child(j);
            if (item->data(0, kNodeRole).toULongLong() == node && item->data(0, kKeyRole).toString() == key) {
                item->setCheckState(0, ticked ? Qt::Checked : Qt::Unchecked);
                return true;
            }
        }
    }
    return false;
}

} // namespace kk
