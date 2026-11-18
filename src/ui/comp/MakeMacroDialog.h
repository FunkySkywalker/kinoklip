// SPDX-License-Identifier: GPL-3.0-or-later
// MakeMacroDialog — "Make Macro" of the Compositing page (spec 120): name the macro and tick the
// parameters of the group's nodes that become its controls (published ones come ticked).
#pragma once

#include "core/CompGroups.h"

#include <QDialog>

class QLineEdit;
class QTreeWidget;
class QDialogButtonBox;

namespace kk {

class MakeMacroDialog : public QDialog
{
    Q_OBJECT
public:
    /// The dialog for group @p group of @p comp.
    MakeMacroDialog(const Composition& comp, quint64 group, QWidget* parent = nullptr);

    /// The name typed (the group's name to start with).
    [[nodiscard]] QString macroName() const;
    /// The ticked parameters.
    [[nodiscard]] QVector<compgroups::ControlPick> picks() const;
    /// Ticks or unticks the parameter (@p node, @p key) (tests).
    bool setTicked(quint64 node, const QString& key, bool ticked);

    [[nodiscard]] QLineEdit* nameField() const { return m_name; }
    [[nodiscard]] QTreeWidget* tree() const { return m_tree; }
    [[nodiscard]] QDialogButtonBox* buttons() const { return m_buttons; }

private:
    QLineEdit* m_name = nullptr;
    QTreeWidget* m_tree = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
};

} // namespace kk
